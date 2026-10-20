#include "quant/backtest/accounting.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/detail.hpp"
#include <type_traits>

namespace quant::backtest {
namespace {
using detail::take;
using detail::fail;
const ValuationProjection &mark(const LedgerRow &row) {
    if (const auto *fill = std::get_if<Fill>(&row.event)) {
        return fill->mark;
    }
    return std::get<DividendEvent>(row.event).prior_close;
}
void bound(const Account &a, const Ledger &ledger) {
    detail::account(a);
    const auto &expected = ledger.rows().empty() ? ledger.initial() : ledger.rows().back().after;
    if (a != expected) {
        fail(ErrorCode::Integrity, "account differs from its ledger snapshot");
    }
}
void succession(const Ledger &ledger, const ValuationProjection &next) {
    if (!ledger.rows().empty()) {
        const auto &last = mark(ledger.rows().back());
        if (next.key() < last.key() || (next.key() == last.key() && next.phase() <= last.phase())) {
            fail(ErrorCode::InvalidData, "duplicate or regressing financial event phase");
        }
    }
}
} // namespace
struct LedgerAccess {
    static void commit(Account &account, Ledger &ledger, Account next,
                       std::variant<Fill, DividendEvent> event) {
        detail::account(next);
        if (ledger.rows_.size() >= ledger.max_events_) {
            fail(ErrorCode::ResourceExceeded, "financial ledger event bound exceeded");
        }
        static_assert(std::is_nothrow_swappable_v<Account>);
        static_assert(std::is_nothrow_move_constructible_v<LedgerRow>);
        LedgerRow row{detail::next(static_cast<std::uint64_t>(ledger.rows_.size())),
                      std::move(event), next};
        ledger.rows_.push_back(std::move(row)); // Strong guarantee before account swap.
        std::swap(account, next);
    }
};
BacktestResult<Ledger> Ledger::create(const Account &initial, std::size_t maximum) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<Ledger> {
            detail::account(initial);
            if (!maximum || maximum > 100000 || initial.revision || initial.cash_revision ||
                initial.units.raw() || initial.cash != initial.initial_cash ||
                initial.realized.amount() || initial.dividends.amount() || initial.fees.amount()) {
                fail(ErrorCode::InvalidConfig,
                     "ledger requires bounded capacity and an untouched funded account");
            }
            Ledger result{initial, maximum};
            result.rows_.reserve(maximum);
            return result;
        },
        initial.trial);
}
BacktestResult<void> apply_fill(Account &a, Ledger &ledger, const Fill &fill) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<void> {
            bound(a, ledger);
            take(validate_fill(fill, a));
            succession(ledger, fill.mark);
            for (const auto &row : ledger.rows()) {
                if (const auto *prior = std::get_if<Fill>(&row.event);
                    prior && prior->id == fill.id) {
                    fail(ErrorCode::InvalidData, "duplicate trial-scoped fill ID");
                }
            }
            auto next = a;
            if (fill.side == data::Side::Buy) {
                if (a.units.raw()) {
                    fail(ErrorCode::InvalidData, "pyramiding is unsupported");
                }
                next.basis = fill.gross + fill.fee;
                next.cash = a.cash - next.basis;
                next.units = fill.quantity;
            } else {
                if (!a.units.raw() || fill.quantity != a.units) {
                    fail(ErrorCode::InvalidData, "sale must close the complete existing position");
                }
                const auto proceeds = fill.gross - fill.fee;
                next.cash = a.cash + proceeds;
                next.realized = a.realized + (proceeds - a.basis);
                next.basis = data::Money::from_raw(a.source.currency, 0);
                next.units = data::Quantity::from_raw(0);
            }
            next.fees = a.fees + fill.fee;
            next.revision = detail::next(a.revision);
            next.last_event = Account::EventLocation{fill.mark.key(), fill.mark.phase()};
            if (next.cash != a.cash) {
                next.cash_revision = detail::next(a.cash_revision);
            }
            LedgerAccess::commit(a, ledger, std::move(next), fill);
            return ok();
        },
        a.trial);
}
BacktestResult<void> apply_dividend(Account &a, Ledger &ledger,
                                    const DividendEvent &event) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<void> {
            bound(a, ledger);
            detail::source(a, event.prior_close);
            if (event.trial != a.trial || event.per_unit.currency() != a.source.currency ||
                event.prior_close.phase() != ValuationPhase::PriorClose ||
                event.entitled_units != a.units) {
                fail(ErrorCode::InvalidData,
                     "dividend must credit the matching prior-close holdings/currency");
            }
            succession(ledger, event.prior_close);
            const auto payment = take(settle_dividend(event.per_unit, event.entitled_units));
            auto next = a;
            next.cash = a.cash + payment;
            next.dividends = a.dividends + payment;
            next.revision = detail::next(a.revision);
            next.last_event =
                Account::EventLocation{event.prior_close.key(), ValuationPhase::PriorClose};
            if (payment.amount()) {
                next.cash_revision = detail::next(a.cash_revision);
            }
            LedgerAccess::commit(a, ledger, std::move(next), event);
            return ok();
        },
        a.trial);
}
BacktestResult<EquityPoint> mark_account(const Account &a, const ValuationProjection &projection,
                                         ValuationPhase phase) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<EquityPoint> {
            detail::account(a);
            detail::source(a, projection);
            if (projection.phase() != phase) {
                fail(ErrorCode::FutureRead,
                     "valuation phase does not match the requested financial phase");
            }
            auto value = take(settle_gross(projection.price(), a.units, a.source.currency));
            auto equity = a.cash + value;
            auto unrealized = value - a.basis;
            if (equity - a.initial_cash != a.realized + unrealized + a.dividends) {
                fail(ErrorCode::Integrity, "marked P&L identity failed");
            }
            return EquityPoint{a.trial,          a.revision,        projection,
                               std::move(value), std::move(equity), std::move(unrealized)};
        },
        a.trial);
}
BacktestResult<void> verify_financial_invariants(const Account &a, const Ledger &ledger,
                                                 const EquityPoint &point) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<void> {
            bound(a, ledger);
            if (take(mark_account(a, point.mark, point.mark.phase())) != point) {
                fail(ErrorCode::Integrity, "equity evidence differs from the current account");
            }
            std::int64_t fees = 0, dividends = 0;
            std::uint64_t sequence = 0;
            for (const auto &row : ledger.rows()) {
                if (row.sequence != ++sequence || row.after.revision != sequence) {
                    fail(ErrorCode::Integrity, "ledger sequence mismatch");
                }
                if (const auto *fill = std::get_if<Fill>(&row.event)) {
                    fees = detail::add(fees, fill->fee.amount());
                } else {
                    const auto &d = std::get<DividendEvent>(row.event);
                    dividends = detail::add(
                        dividends, take(settle_dividend(d.per_unit, d.entitled_units)).amount());
                }
            }
            if (fees != a.fees.amount() || dividends != a.dividends.amount()) {
                fail(ErrorCode::Integrity, "ledger fee/dividend totals do not reconcile");
            }
            return ok();
        },
        a.trial);
}
} // namespace quant::backtest
