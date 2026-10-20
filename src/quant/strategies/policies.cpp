#include "quant/strategies/policies.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/backtest/arithmetic.hpp"
namespace quant::strategies {
backtest::BacktestResult<backtest::Action>
sma20_50_decision(const backtest::DecisionContext &context, const backtest::Account *account,
                  std::span<const backtest::PriceFeature> features) noexcept {
    return backtest::detail::guard([&]() -> backtest::BacktestResult<backtest::Action> {
        std::optional<data::Price> fast, slow;
        for (const auto &feature : features) {
            if (feature.key != context.key) {
                backtest::detail::fail(backtest::ErrorCode::FutureRead,
                                       "S1 requires current-key SMA observations");
            }
            auto *target = feature.request == "sma20"   ? &fast
                           : feature.request == "sma50" ? &slow
                                                        : nullptr;
            if (!target || *target) {
                backtest::detail::fail(backtest::ErrorCode::InvalidConfig,
                                       "S1 received an unknown or duplicate observation");
            }
            *target = feature.value;
        }
        if (!context.may_submit) {
            return backtest::Action{backtest::ActionKind::Hold, data::Quantity::from_raw(0),
                                    "warmup"};
        }
        if (!context.funded || !account) {
            backtest::detail::fail(backtest::ErrorCode::InvalidData,
                                   "S1 cannot trade before funding");
        }
        if (!fast || !slow) {
            return backtest::Action{backtest::ActionKind::Hold, data::Quantity::from_raw(0),
                                    "warming"};
        }
        return backtest::Action{
            *fast > *slow ? backtest::ActionKind::TargetLong : backtest::ActionKind::TargetCash,
            data::Quantity::from_raw(0), *fast > *slow ? "sma20_gt_sma50" : "sma20_le_sma50"};
    });
}
backtest::BacktestResult<backtest::Action>
keltner_decision(KeltnerState &state, const backtest::DecisionContext &c,
                 const backtest::Account *account,
                 std::span<const backtest::PriceFeature> features) noexcept {
    return backtest::detail::guard([&]() -> backtest::BacktestResult<backtest::Action> {
        using namespace backtest;
        using detail::fail;
        using detail::take;
        if (state.last_key && c.key != *state.last_key + 1) {
            fail(ErrorCode::Coverage, "Keltner decisions require successive source keys");
        }
        std::optional<data::Price> close, ema, atr, upper, tr;
        for (const auto &f : features) {
            if (f.key != c.key) {
                fail(ErrorCode::FutureRead, "Keltner requires current features");
            }
            auto *target = f.request == state.prefix + "close"   ? &close
                           : f.request == state.prefix + "ema"   ? &ema
                           : f.request == state.prefix + "atr"   ? &atr
                           : f.request == state.prefix + "upper" ? &upper
                           : f.request == state.prefix + "tr"    ? &tr
                                                                 : nullptr;
            if (!target || *target) {
                fail(ErrorCode::InvalidConfig, "unknown or duplicate Keltner observation");
            }
            *target = f.value;
        }
        state.last_key = c.key;
        const auto hold = [&](std::string why) -> Action {
            return {ActionKind::Hold, data::Quantity::from_raw(0), std::move(why)};
        };
        if (!close || !ema || !atr || !upper) {
            if (state.previous_close) {
                fail(ErrorCode::SourceBinding,
                     "required Keltner feature disappeared after readiness");
            }
            return hold("warming");
        }
        if (close->raw() <= 0 || ema->raw() <= 0 || atr->raw() < 0 || upper->raw() < 0) {
            fail(ErrorCode::InvalidData, "invalid Keltner feature domain");
        }
        const bool crossing = state.previous_close && state.previous_upper &&
                              *state.previous_close <= *state.previous_upper && *close > *upper;
        state.previous_close = *close;
        state.previous_upper = *upper;
        if (!c.may_submit) {
            return hold("warmup");
        }
        if (!c.funded || !account || state.held != (account->units.raw() > 0)) {
            fail(ErrorCode::Integrity, "Keltner account/execution state differs");
        }
        if (state.held) {
            if (!state.threshold) {
                fail(ErrorCode::Integrity, "held Keltner position has no protective threshold");
            }
            state.highest_close =
                state.highest_close ? std::max(*state.highest_close, *close) : *close;
            const auto distance = data::detail::checked_add(atr->raw(), atr->raw());
            const auto candidate = data::Price::from_raw(
                data::detail::checked_subtract(state.highest_close->raw(), distance));
            state.threshold = std::max(*state.threshold, candidate);
            if (*close <= *state.threshold) {
                return Action{ActionKind::TargetCash, data::Quantity::from_raw(0),
                              "protective_close"};
            }
            if (*close < *ema) {
                return Action{ActionKind::TargetCash, data::Quantity::from_raw(0), "below_ema"};
            }
            return hold("held");
        }
        if (!crossing) {
            return hold("no_crossing");
        }
        if (atr->raw() == 0) {
            return hold("zero_atr");
        }
        auto quantity = take(keltner_quantity(*account, *close, *atr, state.costs));
        auto distance = data::Price::from_raw(data::detail::checked_add(atr->raw(), atr->raw()));
        return Action{ActionKind::FixedEntry, quantity, "keltner_crossing", distance};
    });
}
backtest::BacktestResult<void> keltner_execution(KeltnerState &state,
                                                 const backtest::FillQuote &result,
                                                 const backtest::Account &account) noexcept {
    return backtest::detail::guard([&]() -> backtest::BacktestResult<void> {
        using namespace backtest;
        using detail::fail;
        if (std::holds_alternative<OrderRejection>(result)) {
            if (state.held || account.units.raw()) {
                fail(ErrorCode::Integrity, "unexpected rejected Keltner exit");
            }
            return ok();
        }
        const auto &fill = std::get<Fill>(result);
        if (fill.trial != account.trial) {
            fail(ErrorCode::Integrity, "Keltner receipt belongs to another account");
        }
        if (fill.side == data::Side::Buy) {
            if (state.held || !account.units.raw() || !fill.intent) {
                fail(ErrorCode::Integrity, "invalid Keltner entry receipt");
            }
            const auto *fixed = std::get_if<BuyFixedQuantity>(&fill.intent->order);
            if (!fixed || !fixed->initial_stop_distance) {
                fail(ErrorCode::Integrity, "entry receipt lacks frozen ATR distance");
            }
            const auto stop = data::detail::checked_subtract(fill.executed_price.raw(),
                                                             fixed->initial_stop_distance->raw());
            if (stop <= 0) {
                fail(ErrorCode::Integrity, "nonpositive protective threshold reached posting");
            }
            state.threshold = data::Price::from_raw(stop);
            state.highest_close.reset();
            state.held = true;
        } else {
            if (!state.held || account.units.raw()) {
                fail(ErrorCode::Integrity, "invalid Keltner sale receipt");
            }
            state.held = false;
            state.threshold.reset();
            state.highest_close.reset();
        }
        return ok();
    });
}
} // namespace quant::strategies
