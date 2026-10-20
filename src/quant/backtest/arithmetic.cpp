#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/research/daily_dataset.hpp"
#include <array>

namespace quant::backtest {
namespace {
using detail::fail;
using detail::take;
// Fixed-size portable intermediate. Only nonnegative financial products enter it.
struct Wide {
    std::array<std::uint32_t, 8> words{};
    explicit Wide(std::uint64_t n = 0) {
        words[0] = static_cast<std::uint32_t>(n);
        words[1] = static_cast<std::uint32_t>(n >> 32);
    }
    bool bit(unsigned index) const {
        return (words[index / 32] >> (index % 32)) & 1U;
    }
    void set(unsigned index) {
        words[index / 32] |= std::uint32_t{1} << (index % 32);
    }
    Wide plus(const Wide &other) const {
        Wide out;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto sum = std::uint64_t{words[i]} + other.words[i] + carry;
            out.words[i] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
        if (carry) {
            fail(ErrorCode::NumericRange, "wide financial addition overflow");
        }
        return out;
    }
    Wide shifted() const {
        Wide out;
        std::uint32_t carry = 0;
        for (std::size_t i = 0; i < words.size(); ++i) {
            out.words[i] = (words[i] << 1) | carry;
            carry = words[i] >> 31;
        }
        if (carry) {
            fail(ErrorCode::NumericRange, "wide financial shift overflow");
        }
        return out;
    }
    Wide times(std::uint64_t factor) const {
        Wide out, term = *this;
        while (factor) {
            if (factor & 1) {
                out = out.plus(term);
            }
            factor >>= 1;
            if (factor) {
                term = term.shifted();
            }
        }
        return out;
    }
    friend bool operator<=(const Wide &a, const Wide &b) {
        for (std::size_t i = a.words.size(); i-- > 0;) {
            if (a.words[i] != b.words[i]) {
                return a.words[i] < b.words[i];
            }
        }
        return true;
    }
    std::int64_t coefficient() const {
        for (std::size_t i = 2; i < words.size(); ++i) {
            if (words[i]) {
                fail(ErrorCode::NumericRange, "settlement exceeds signed64");
            }
        }
        const auto n = std::uint64_t{words[0]} | (std::uint64_t{words[1]} << 32);
        if (n > INT64_MAX) {
            fail(ErrorCode::NumericRange, "settlement exceeds signed64");
        }
        return static_cast<std::int64_t>(n);
    }
};
enum class Rounding { Floor, Ceil, Even };
Wide divide(const Wide &n, std::uint64_t d, Rounding rounding) {
    if (!d || d > UINT64_MAX / 2) {
        fail(ErrorCode::NumericRange, "unsupported wide divisor");
    }
    Wide q;
    std::uint64_t remainder = 0;
    for (unsigned i = 256; i-- > 0;) {
        remainder = remainder * 2 + static_cast<unsigned>(n.bit(i));
        if (remainder >= d) {
            remainder -= d;
            q.set(i);
        }
    }
    const bool up = rounding == Rounding::Ceil
                        ? remainder != 0
                        : rounding == Rounding::Even && (remainder > d - remainder ||
                                                         (remainder == d - remainder && q.bit(0)));
    return up ? q.plus(Wide{1}) : q;
}
Wide gross(std::uint64_t price, std::uint64_t quantity) {
    return divide(Wide{price}.times(quantity), 10000000000000ULL, Rounding::Even);
}
Wide fee(std::uint64_t price, std::uint64_t quantity, std::int32_t bps) {
    return divide(Wide{price}.times(quantity).times(static_cast<std::uint64_t>(bps)),
                  100000000000000000ULL, Rounding::Ceil);
}
Wide cost(std::uint64_t price, std::uint64_t quantity, std::int32_t bps) {
    return gross(price, quantity).plus(fee(price, quantity, bps));
}
std::int64_t cap(std::int64_t cash) {
    return divide(Wide{static_cast<std::uint64_t>(cash)}.times(95), 100, Rounding::Floor)
        .coefficient();
}
void positive(data::Price price) {
    if (price.raw() <= 0) {
        fail(ErrorCode::InvalidData, "executed/marked price must be positive");
    }
}
Fill make_fill(const Account &a, std::uint64_t id, const ValuationProjection &mark, data::Side side,
               data::Quantity quantity, const ExecutionPolicy &policy,
               std::optional<Intent> intent) {
    auto price = take(execution_price(mark.price(), side, policy));
    auto g = take(settle_gross(price, quantity, a.source.currency));
    auto f = take(settle_fee(price, quantity, a.source.currency, policy));
    const bool terminal = !intent;
    auto reason = intent ? intent->reason : std::string{"synthetic_terminal_liquidation"};
    return {a.trial,          id,    a.revision,   std::move(intent), mark,   side,
            quantity,         price, std::move(g), std::move(f),      policy, terminal,
            std::move(reason)};
}
FillQuote ordinary(const Intent &intent, const ValuationProjection &mark, const Account &a,
                   const ExecutionPolicy &policy) {
    detail::account(a);
    detail::policy(policy);
    detail::source(a, mark);
    if (mark.phase() != ValuationPhase::Open || intent.trial != a.trial ||
        intent.source != a.source || !intent.id || intent.reason.empty() ||
        intent.reason.size() > 128 || intent.reason.find_first_of("\r\n\t") != std::string::npos ||
        intent.reason.find('\0') != std::string::npos ||
        intent.decision_key >= intent.earliest_open_key) {
        fail(ErrorCode::InvalidConfig, "invalid opening intent identity/timing/reason");
    }
    const auto reject = [&](RejectionReason reason) -> FillQuote {
        return OrderRejection{a.trial, intent.id, reason};
    };
    if (mark.key() < intent.earliest_open_key) {
        return reject(RejectionReason::NotEligible);
    }
    if (std::holds_alternative<SellAll>(intent.order)) {
        if (!a.units.raw()) {
            return reject(RejectionReason::NoPosition);
        }
        return make_fill(a, intent.id, mark, data::Side::Sell, a.units, policy, intent);
    }
    if (a.units.raw()) {
        return reject(RejectionReason::PositionAlreadyOpen);
    }
    const auto price = take(execution_price(mark.price(), data::Side::Buy, policy));
    const auto allocation = cap(a.cash.amount());
    std::uint64_t quantity = 0;
    if (const auto *budget = std::get_if<BuyCashBudget>(&intent.order)) {
        if (budget->budget.currency() != a.source.currency ||
            budget->decision_cash.currency() != a.source.currency ||
            budget->decision_cash.amount() < 0 ||
            budget->budget.amount() != cap(budget->decision_cash.amount())) {
            fail(ErrorCode::InvalidConfig, "invalid frozen 95-percent cash budget");
        }
        if (budget->cash_revision != a.cash_revision || budget->decision_cash != a.cash) {
            return reject(RejectionReason::CashChanged);
        }
        const auto limit =
            Wide{static_cast<std::uint64_t>(std::min(allocation, budget->budget.amount()))};
        const auto lot = static_cast<std::uint64_t>(a.lot_step.raw());
        std::uint64_t low = 0, high = static_cast<std::uint64_t>(INT64_MAX) / lot;
        // A mathematically affordable out-of-domain quantity is an explicit range
        // failure, not a silent maximum-quantity execution policy.
        if (cost(static_cast<std::uint64_t>(price.raw()), (high + 1) * lot,
                 policy.commission_bps) <= limit) {
            fail(ErrorCode::NumericRange, "affordable quantity exceeds Quantity6 range");
        }
        while (low < high) {
            const auto mid = low + (high - low) / 2 + 1;
            if (cost(static_cast<std::uint64_t>(price.raw()), mid * lot, policy.commission_bps) <=
                limit) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        quantity = low * lot;
    } else {
        const auto &fixed = std::get<BuyFixedQuantity>(intent.order);
        if (fixed.initial_stop_distance && fixed.initial_stop_distance->raw() <= 0) {
            fail(ErrorCode::InvalidConfig, "protective distance must be positive");
        }
        const auto raw = fixed.quantity.raw();
        if (raw % a.lot_step.raw() != 0) {
            fail(ErrorCode::InvalidConfig, "fixed quantity is not a lot multiple");
        }
        quantity = static_cast<std::uint64_t>(raw);
    }
    if (!quantity) {
        return reject(RejectionReason::BelowMinimumLot);
    }
    if (const auto *fixed = std::get_if<BuyFixedQuantity>(&intent.order);
        fixed && fixed->initial_stop_distance && price <= *fixed->initial_stop_distance) {
        return reject(RejectionReason::NonpositiveProtectiveThreshold);
    }
    const auto committed =
        cost(static_cast<std::uint64_t>(price.raw()), quantity, policy.commission_bps);
    if (!(committed <= Wide{static_cast<std::uint64_t>(a.cash.amount())})) {
        return reject(RejectionReason::InsufficientCash);
    }
    if (!(committed <= Wide{static_cast<std::uint64_t>(allocation)})) {
        return reject(RejectionReason::AllocationCap);
    }
    return make_fill(a, intent.id, mark, data::Side::Buy,
                     data::Quantity::from_raw(static_cast<std::int64_t>(quantity)), policy, intent);
}
FillQuote terminal(const ValuationProjection &mark, const Account &a, const ExecutionPolicy &policy,
                   const TerminalPolicy &endpoint) {
    detail::account(a);
    detail::source(a, mark);
    detail::policy(policy);
    if (endpoint.mode != TerminalMode::SyntheticClose || !endpoint.fill_id ||
        mark.phase() != ValuationPhase::Close) {
        fail(ErrorCode::InvalidConfig,
             "terminal fill requires explicit synthetic-close policy and ID");
    }
    if (!a.units.raw()) {
        return OrderRejection{a.trial, endpoint.fill_id, RejectionReason::NoPosition};
    }
    return make_fill(a, endpoint.fill_id, mark, data::Side::Sell, a.units, policy, std::nullopt);
}
} // namespace
BacktestResult<Account> fund_account(std::uint32_t trial, SourceIdentity source,
                                     data::Money initial, data::Quantity lot) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<Account> {
            auto zero = data::Money::from_raw(source.currency, 0);
            Account a{trial, std::move(source),           initial, initial, zero, zero, zero,
                      zero,  data::Quantity::from_raw(0), lot};
            detail::account(a);
            return a;
        },
        trial);
}
BacktestResult<BuyCashBudget> freeze_cash_budget(const Account &a) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<BuyCashBudget> {
            detail::account(a);
            return BuyCashBudget{data::Money::from_raw(a.source.currency, cap(a.cash.amount())),
                                 a.cash, a.cash_revision};
        },
        a.trial);
}
BacktestResult<data::Price> execution_price(data::Price p, data::Side side,
                                            const ExecutionPolicy &policy) noexcept {
    return detail::guard([&]() -> BacktestResult<data::Price> {
        positive(p);
        detail::policy(policy);
        if (side != data::Side::Buy && side != data::Side::Sell) {
            fail(ErrorCode::InvalidConfig, "invalid fill side");
        }
        const auto factor =
            side == data::Side::Buy ? 10000 + policy.slippage_bps : 10000 - policy.slippage_bps;
        auto price = data::Price::from_raw(
            divide(
                Wide{static_cast<std::uint64_t>(p.raw())}.times(static_cast<std::uint64_t>(factor)),
                10000, side == data::Side::Buy ? Rounding::Ceil : Rounding::Floor)
                .coefficient());
        positive(price);
        return price;
    });
}
BacktestResult<data::Money> settle_gross(data::Price p, data::Quantity q,
                                         const data::CurrencyCode &currency) noexcept {
    return detail::guard([&]() -> BacktestResult<data::Money> {
        positive(p);
        detail::currency(currency);
        return data::Money::from_raw(currency, gross(static_cast<std::uint64_t>(p.raw()),
                                                     static_cast<std::uint64_t>(q.raw()))
                                                   .coefficient());
    });
}
BacktestResult<data::Money> settle_fee(data::Price p, data::Quantity q,
                                       const data::CurrencyCode &currency,
                                       const ExecutionPolicy &policy) noexcept {
    return detail::guard([&]() -> BacktestResult<data::Money> {
        positive(p);
        detail::currency(currency);
        detail::policy(policy);
        return data::Money::from_raw(currency,
                                     fee(static_cast<std::uint64_t>(p.raw()),
                                         static_cast<std::uint64_t>(q.raw()), policy.commission_bps)
                                         .coefficient());
    });
}
BacktestResult<data::Money> settle_dividend(const DividendPerUnit9 &per_unit,
                                            data::Quantity q) noexcept {
    return detail::guard([&]() -> BacktestResult<data::Money> {
        detail::currency(per_unit.currency());
        return data::Money::from_raw(
            per_unit.currency(),
            gross(static_cast<std::uint64_t>(per_unit.raw()), static_cast<std::uint64_t>(q.raw()))
                .coefficient());
    });
}
BacktestResult<ValuationProjection>
opening_valuation(const research::OpeningProjection &p) noexcept {
    return detail::guard([&]() -> BacktestResult<ValuationProjection> {
        return ValuationProjection{p.source(), p.price(),           p.key(),
                                   p.key(),    p.available_at_ns(), ValuationPhase::Open};
    });
}
BacktestResult<ValuationProjection> closing_valuation(const research::CloseProjection &p) noexcept {
    return detail::guard([&]() -> BacktestResult<ValuationProjection> {
        return ValuationProjection{p.source(), p.bar().close,       p.key(),
                                   p.key(),    p.available_at_ns(), ValuationPhase::Close};
    });
}
BacktestResult<ValuationProjection>
prior_close_valuation(const research::CloseProjection &p,
                      const research::OpeningProjection &next) noexcept {
    return detail::guard([&]() -> BacktestResult<ValuationProjection> {
        if (p.key() == UINT64_MAX || p.key() + 1 != next.key() || p.source() != next.source() ||
            p.available_at_ns() >= next.available_at_ns()) {
            fail(ErrorCode::FutureRead,
                 "prior-close mark must precede the same source's next opening");
        }
        return ValuationProjection{p.source(), p.bar().close,       next.key(),
                                   p.key(),    p.available_at_ns(), ValuationPhase::PriorClose};
    });
}
BacktestResult<FillQuote> quote_fill(const Intent &i, const research::OpeningProjection &p,
                                     const Account &a, const ExecutionPolicy &policy) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<FillQuote> {
            return ordinary(i, take(opening_valuation(p)), a, policy);
        },
        a.trial);
}
BacktestResult<FillQuote> quote_terminal_liquidation(const research::CloseProjection &p,
                                                     const Account &a,
                                                     const ExecutionPolicy &policy,
                                                     const TerminalPolicy &endpoint) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<FillQuote> {
            return terminal(take(closing_valuation(p)), a, policy, endpoint);
        },
        a.trial);
}
BacktestResult<void> validate_fill(const Fill &fill, const Account &a) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<void> {
            if (fill.trial != a.trial || !fill.id || fill.expected_revision != a.revision ||
                fill.quantity.raw() <= 0) {
                fail(ErrorCode::InvalidData, "fill identity/revision/quantity invalid");
            }
            auto expected =
                fill.synthetic_terminal
                    ? terminal(fill.mark, a, fill.policy, {TerminalMode::SyntheticClose, fill.id})
                : fill.intent
                    ? ordinary(*fill.intent, fill.mark, a, fill.policy)
                    : throw detail::Failure{error(ErrorCode::InvalidData,
                                                  "ordinary fill lacks intent", Phase::Settlement)};
            const auto *candidate = std::get_if<Fill>(&expected);
            if (!candidate || *candidate != fill) {
                fail(ErrorCode::Integrity, "fill does not equal its fully revalidated quote");
            }
            return ok();
        },
        a.trial);
}
BacktestResult<bool> loss_at_least(data::Money baseline, data::Money current,
                                   std::uint32_t bps) noexcept {
    return detail::guard([&]() -> BacktestResult<bool> {
        if (baseline.currency() != current.currency() || baseline.amount() <= 0 ||
            current.amount() < 0 || !bps || bps > 10000) {
            fail(ErrorCode::InvalidConfig, "invalid loss threshold/baseline");
        }
        if (current.amount() >= baseline.amount()) {
            return false;
        }
        return Wide{static_cast<std::uint64_t>(baseline.amount())}.times(bps) <=
               Wide{static_cast<std::uint64_t>(baseline.amount() - current.amount())}.times(10000);
    });
}
BacktestResult<bool> ratio_greater(std::int64_t n, std::int64_t d, std::int64_t on,
                                   std::int64_t od) noexcept {
    return detail::guard([&]() -> BacktestResult<bool> {
        if (n < 0 || on < 0 || d <= 0 || od <= 0) {
            fail(ErrorCode::InvalidConfig,
                 "ratio comparison requires nonnegative numerators and positive denominators");
        }
        return !(Wide{static_cast<std::uint64_t>(n)}.times(static_cast<std::uint64_t>(od)) <=
                 Wide{static_cast<std::uint64_t>(on)}.times(static_cast<std::uint64_t>(d)));
    });
}
BacktestResult<data::Price> weighted_price(data::Price a, std::uint64_t wa, data::Price b,
                                           std::uint64_t wb, std::uint64_t d) noexcept {
    return detail::guard([&]() -> BacktestResult<data::Price> {
        if (a.raw() < 0 || b.raw() < 0 || !d) {
            fail(ErrorCode::InvalidConfig,
                 "weighted Price requires nonnegative inputs and a divisor");
        }
        return data::Price::from_raw(
            divide(Wide{static_cast<std::uint64_t>(a.raw())}.times(wa).plus(
                       Wide{static_cast<std::uint64_t>(b.raw())}.times(wb)),
                   d, Rounding::Even)
                .coefficient());
    });
}
BacktestResult<data::Quantity> keltner_quantity(const Account &a, data::Price close,
                                                data::Price atr,
                                                const ExecutionPolicy &policy) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<data::Quantity> {
            detail::account(a);
            detail::policy(policy);
            positive(close);
            positive(atr);
            if (a.units.raw()) {
                fail(ErrorCode::InvalidData,
                     "Keltner entry sizing requires a flat single-position account");
            }
            const auto price = take(execution_price(close, data::Side::Buy, policy));
            const auto budget = Wide{static_cast<std::uint64_t>(cap(a.cash.amount()))};
            // q_raw * ATR_raw * 40000 <= equity_cents * 10^15. No preliminary
            // rounding of the exact equity/200 risk budget or stop-distance quotient.
            const auto risk =
                Wide{static_cast<std::uint64_t>(a.cash.amount())}.times(1000000000000000ULL);
            const auto affordable = [&](std::uint64_t q) {
                return Wide{q}.times(static_cast<std::uint64_t>(atr.raw())).times(40000) <= risk &&
                       cost(static_cast<std::uint64_t>(price.raw()), q, policy.commission_bps) <=
                           budget;
            };
            const auto lot = static_cast<std::uint64_t>(a.lot_step.raw());
            std::uint64_t low = 0, high = static_cast<std::uint64_t>(INT64_MAX) / lot;
            if (affordable((high + 1) * lot)) {
                fail(ErrorCode::NumericRange, "Keltner quantity exceeds Quantity6 range");
            }
            while (low < high) {
                const auto mid = low + (high - low) / 2 + 1;
                if (affordable(mid * lot)) {
                    low = mid;
                } else {
                    high = mid - 1;
                }
            }
            return data::Quantity::from_raw(static_cast<std::int64_t>(low * lot));
        },
        a.trial);
}
} // namespace quant::backtest
