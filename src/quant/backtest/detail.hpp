#pragma once
#include "quant/backtest/model.hpp"
#include <limits>
#include <new>

namespace quant::backtest::detail {
struct Failure {
    BacktestError value;
};
[[noreturn]] inline void fail(ErrorCode code, std::string_view message) {
    throw Failure{error(code, message, Phase::Settlement)};
}
template <class T> T take(std::variant<T, BacktestError> result) {
    if (auto *e = std::get_if<BacktestError>(&result)) {
        throw Failure{*e};
    }
    return std::get<T>(std::move(result));
}
template <class F>
auto guard(F &&fn, std::optional<std::uint32_t> trial = {}) noexcept -> decltype(fn()) {
    try {
        return fn();
    } catch (const Failure &e) {
        auto out = e.value;
        out.trial = trial;
        return out;
    } catch (const std::overflow_error &e) {
        auto out = error(ErrorCode::NumericRange, e.what(), Phase::Settlement);
        out.trial = trial;
        return out;
    } catch (const std::bad_alloc &) {
        auto out =
            error(ErrorCode::ResourceExceeded, "financial allocation failed", Phase::Settlement);
        out.trial = trial;
        return out;
    } catch (const std::exception &e) {
        auto out = error(ErrorCode::InvalidConfig, e.what(), Phase::Settlement);
        out.trial = trial;
        return out;
    } catch (...) {
        return error(ErrorCode::InvalidData, "unknown financial failure", Phase::Settlement);
    }
}
inline std::int64_t add(std::int64_t a, std::int64_t b) {
    return data::detail::checked_add(a, b);
}
inline std::int64_t sub(std::int64_t a, std::int64_t b) {
    return data::detail::checked_subtract(a, b);
}
inline std::uint64_t next(std::uint64_t value) {
    if (value == UINT64_MAX) {
        fail(ErrorCode::NumericRange, "financial sequence overflow");
    }
    return value + 1;
}
inline void identity(const SourceIdentity &s) {
    if (!s.instrument || !s.venue || !s.source || s.currency.value() != "USD" ||
        s.manifest_sha256.size() != 64 ||
        !std::all_of(s.manifest_sha256.begin(), s.manifest_sha256.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
        fail(ErrorCode::InvalidConfig,
             "financial source requires explicit USD instrument and manifest identity");
    }
}
inline void policy(const ExecutionPolicy &p) {
    if (p.commission_bps < 0 || p.commission_bps >= 10000 || p.slippage_bps < 0 ||
        p.slippage_bps >= 10000) {
        fail(ErrorCode::InvalidConfig, "cost basis points must be in [0,10000)");
    }
}
inline void currency(const data::CurrencyCode &c) {
    if (c.value() != "USD") {
        fail(ErrorCode::InvalidConfig, "unsupported financial currency");
    }
}
inline void account(const Account &a) {
    identity(a.source);
    for (const auto *money :
         {&a.initial_cash, &a.cash, &a.basis, &a.realized, &a.dividends, &a.fees}) {
        if (money->currency() != a.source.currency) {
            fail(ErrorCode::InvalidData, "account currency mismatch");
        }
    }
    if (a.initial_cash.amount() <= 0 || a.cash.amount() < 0 || a.basis.amount() < 0 ||
        a.dividends.amount() < 0 || a.fees.amount() < 0 || a.lot_step.raw() <= 0 ||
        a.units.raw() % a.lot_step.raw() != 0 || (!a.units.raw() && a.basis.amount() != 0) ||
        a.cash_revision > a.revision || a.last_event.has_value() != (a.revision != 0)) {
        fail(ErrorCode::InvalidData, "invalid funded account balances or lot state");
    }
    if (sub(add(a.cash.amount(), a.basis.amount()), a.initial_cash.amount()) !=
        add(a.realized.amount(), a.dividends.amount())) {
        fail(ErrorCode::Integrity, "cash/basis account identity failed");
    }
}
inline void source(const Account &a, const ValuationProjection &m) {
    if (a.source != m.source() || m.price().raw() <= 0) {
        fail(ErrorCode::InvalidData, "valuation source or positive price mismatch");
    }
    if (a.last_event && (m.key() < a.last_event->key ||
                         (m.key() == a.last_event->key && m.phase() < a.last_event->phase))) {
        fail(ErrorCode::FutureRead, "valuation predates the account's latest financial phase");
    }
}
} // namespace quant::backtest::detail
