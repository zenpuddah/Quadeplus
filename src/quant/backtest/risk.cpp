#include "quant/backtest/risk.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/accounting.hpp"
#include "quant/backtest/detail.hpp"
namespace quant::backtest {
BacktestResult<void> validate_risk_policy(const RiskPolicy &p) noexcept {
    return detail::guard([&]() -> BacktestResult<void> {
        for (auto b : {p.drawdown_bps, p.daily_loss_bps}) {
            if (b && (!*b || *b > 10000)) {
                detail::fail(ErrorCode::InvalidConfig,
                             "risk thresholds must be positive basis points at most 10000");
            }
        }
        return ok();
    });
}
BacktestResult<RiskState> initialize_risk(data::Money initial) noexcept {
    return detail::guard([&]() -> BacktestResult<RiskState> {
        detail::currency(initial.currency());
        if (initial.amount() <= 0) {
            detail::fail(ErrorCode::InvalidConfig, "risk requires positive initial equity");
        }
        return RiskState{initial, initial, false, RiskReason::None, {}};
    });
}
BacktestResult<RiskDecision> evaluate_risk(RiskState &state, const Account &a,
                                           const EquityPoint &point,
                                           const RiskPolicy &policy) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<RiskDecision> {
            detail::take(validate_risk_policy(policy));
            if (point != detail::take(mark_account(a, point.mark, ValuationPhase::Close)) ||
                state.high_water.currency() != a.source.currency ||
                state.previous_close.currency() != a.source.currency ||
                state.high_water.amount() <= 0 || state.previous_close.amount() < 0 ||
                (state.last_close && point.mark.key() <= *state.last_close) ||
                (a.last_event && a.last_event->key == point.mark.key() &&
                 a.last_event->phase == ValuationPhase::Close)) {
                detail::fail(ErrorCode::InvalidData,
                             "risk requires a new ordinary pre-terminal close");
            }
            auto next = state;
            bool trigger = false;
            if (point.equity.amount() > next.high_water.amount()) {
                next.high_water = point.equity;
            }
            if (!next.latched) {
                if (policy.drawdown_bps && detail::take(loss_at_least(next.high_water, point.equity,
                                                                      *policy.drawdown_bps))) {
                    next.reason = RiskReason::Drawdown;
                    next.latched = trigger = true;
                } else if (policy.daily_loss_bps &&
                           detail::take(loss_at_least(next.previous_close, point.equity,
                                                      *policy.daily_loss_bps))) {
                    next.reason = RiskReason::DailyLoss;
                    next.latched = trigger = true;
                }
            }
            next.previous_close = point.equity;
            next.last_close = point.mark.key();
            state = std::move(next);
            return RiskDecision{state.latched && a.units.raw() > 0, state.latched, trigger,
                                state.reason};
        },
        a.trial);
}
} // namespace quant::backtest
