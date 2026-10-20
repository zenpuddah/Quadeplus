#include "quant/backtest/accounting.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/research/daily_dataset.hpp"
#include "quant/storage/file_integrity.hpp"
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace quant::backtest;
namespace qd = quant::data;
template <class T> T take(std::variant<T, BacktestError> value) {
    if (auto *e = std::get_if<BacktestError>(&value)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(value));
}
void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string{message});
    }
}
template <class T>
void rejects(std::variant<T, BacktestError> value, std::optional<ErrorCode> code = {}) {
    const auto *failure = std::get_if<BacktestError>(&value);
    require(failure != nullptr, "expected fatal rejection");
    if (code) {
        require(failure->code == *code, "wrong fatal rejection code");
    }
}
void rejection(BacktestResult<FillQuote> value, RejectionReason reason) {
    auto quote = take(std::move(value));
    const auto *r = std::get_if<OrderRejection>(&quote);
    require(r && r->reason == reason, "wrong ordinary order rejection");
}
qd::Money dollars(std::int64_t cents) {
    return qd::Money::from_raw(qd::CurrencyCode{"USD"}, cents);
}
std::filesystem::path root;
std::ostringstream trace;
quant::research::DailyDataset fixture() {
    namespace qr = quant::research;
    namespace files = quant::storage::files;
    const std::string csv =
        "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n2026-01-02,100,100,100,100,1,100,"
        "0\n2026-01-05,100,100,100,100,1,100,1.889\n2026-01-06,110,110,110,110,1,110,1\n2026-01-07,"
        "90,90,90,90,1,90,0\n2026-01-08,90,90,90,90,1,90,0\n";
    const std::string grid = "Date\n2026-01-02\n2026-01-05\n2026-01-06\n2026-01-07\n2026-01-08\n";
    files::write_text(root / "prices.csv", csv);
    files::write_text(root / "sessions.csv", grid);
    auto resource = ::lexongine::ResourceAccount::create(
        {{16777216, 16777216, 16777216, 16777216, 16777216, 16777216, 8388608},
         1024,
         1024,
         8192,
         128});
    require(std::holds_alternative<std::shared_ptr<::lexongine::ResourceAccount>>(resource),
            "fixture resource account");
    qr::DatasetSpec spec{"accounting-fixture-v1",
                         "TEST",
                         "USD",
                         "authored-accounting-input",
                         1,
                         1,
                         1,
                         "synthetic_session_grid_v1",
                         std::string{qr::adjustment_profile},
                         std::string{qr::decimal_profile},
                         std::string{qr::volume_profile},
                         std::string{qr::clock_profile},
                         "2026-01-02",
                         "2026-01-05",
                         "2026-01-08"};
    return take(qr::prepare_daily_dataset(
        {root / "prices.csv",
         root / "sessions.csv",
         root / "bundle",
         files::sha256(csv),
         files::sha256(grid),
         spec,
         {1048576, 8, 64},
         std::get<std::shared_ptr<::lexongine::ResourceAccount>>(resource)}));
}
struct Environment {
    quant::research::DailyDataset data;
    auto open(std::uint64_t key) const {
        return take(data.opening(key, data.rows()[key].bar.interval_start.unix_nanos()));
    }
    auto close(std::uint64_t key) const {
        return take(data.closing(key, data.rows()[key].close_available_ns));
    }
    auto prior(std::uint64_t key) const {
        return take(prior_close_valuation(close(key - 1), open(key)));
    }
    auto account(std::int64_t cash = 1000000, std::int64_t lot = 1) const {
        return take(fund_account(7, open(0).source(), dollars(cash), qd::Quantity::from_raw(lot)));
    }
};
Intent intent(const Account &a, std::uint64_t id, std::uint64_t key,
              std::variant<BuyCashBudget, BuyFixedQuantity, SellAll> order) {
    return {a.trial, id, a.source, key - 1, key, "synthetic_check", std::move(order)};
}
Fill filled(BacktestResult<FillQuote> result) {
    auto quote = take(std::move(result));
    require(std::holds_alternative<Fill>(quote), "unexpected ordinary order rejection");
    return std::get<Fill>(std::move(quote));
}
void invariant(const Account &a, const Ledger &ledger, const ValuationProjection &mark) {
    take(verify_financial_invariants(a, ledger, take(mark_account(a, mark, mark.phase()))));
}
void emit(std::string_view name, const ExecutionPolicy &policy, std::string_view kind,
          const Account &a, const Ledger &ledger, const ValuationProjection &mark,
          const Fill *fill = nullptr, const DividendEvent *dividend = nullptr) {
    auto point = take(mark_account(a, mark, mark.phase()));
    take(verify_financial_invariants(a, ledger, point));
    std::string order = "none";
    std::int64_t budget = 0, requested = 0;
    std::uint64_t decision = 0, eligible = 0;
    if (fill && fill->intent) {
        const auto &i = *fill->intent;
        decision = i.decision_key;
        eligible = i.earliest_open_key;
        if (const auto *b = std::get_if<BuyCashBudget>(&i.order)) {
            order = "budget";
            budget = b->budget.amount();
        } else if (const auto *q = std::get_if<BuyFixedQuantity>(&i.order)) {
            order = "fixed";
            requested = q->quantity.raw();
        } else {
            order = "sell";
        }
    }
    trace << name << ',' << policy.commission_bps << ',' << policy.slippage_bps << ','
          << a.lot_step.raw() << ',' << a.initial_cash.amount() << ',' << kind << ',' << mark.key()
          << ',' << static_cast<int>(mark.phase()) << ',' << order << ',' << decision << ','
          << eligible << ',' << budget << ',' << requested << ',' << mark.price().raw() << ','
          << (dividend ? dividend->per_unit.raw() : 0) << ','
          << (fill       ? fill->quantity.raw()
              : dividend ? dividend->entitled_units.raw()
                         : 0)
          << ',' << (fill ? fill->executed_price.raw() : 0) << ','
          << (fill ? fill->gross.amount() : 0) << ',' << (fill ? fill->fee.amount() : 0) << ','
          << a.cash.amount() << ',' << a.units.raw() << ',' << a.basis.amount() << ','
          << a.realized.amount() << ',' << a.dividends.amount() << ',' << a.fees.amount() << ','
          << point.position_value.amount() << ',' << point.equity.amount() << ','
          << point.unrealized.amount() << ',' << (fill && fill->synthetic_terminal) << '\n';
}
void settlement_boundaries() {
    qd::CurrencyCode usd{"USD"};
    require(take(settle_dividend({usd, 1889000000}, qd::Quantity::from_integer(10))).amount() ==
                1889,
            "per-unit dividend rounded before multiplication");
    require(take(settle_gross(qd::Price::from_raw(5000000), qd::Quantity::from_integer(1), usd))
                    .amount() == 0,
            "even zero-cent half tie");
    require(take(settle_gross(qd::Price::from_raw(15000000), qd::Quantity::from_integer(1), usd))
                    .amount() == 2,
            "odd cent half tie");
    require(
        take(settle_fee(qd::Price::from_raw(1), qd::Quantity::from_raw(1), usd, {5, 5})).amount() ==
            1,
        "commission must use exact pre-settlement gross");
    rejects(execution_price(qd::Price::from_raw(INT64_MAX), qd::Side::Buy, {0, 1}),
            ErrorCode::NumericRange);
    rejects(settle_gross(qd::Price::from_raw(INT64_MAX), qd::Quantity::from_raw(INT64_MAX), usd),
            ErrorCode::NumericRange);
    bool negative_dividend = false;
    try {
        (void)DividendPerUnit9{usd, -1};
    } catch (const std::invalid_argument &) {
        negative_dividend = true;
    }
    require(negative_dividend, "negative per-unit dividend admitted");
    rejects(settle_gross(qd::Price::from_raw(0), qd::Quantity::from_integer(1), usd));
    bool negative_units = false;
    try {
        (void)qd::Quantity::from_raw(-1);
    } catch (const std::invalid_argument &) {
        negative_units = true;
    }
    require(negative_units, "negative Quantity6 admitted");
}
void hand_trace(const Environment &e) {
    auto a = e.account();
    auto ledger = take(Ledger::create(a, 16));
    ExecutionPolicy costs{0, 0};
    emit("hand", costs, "fund", a, ledger, take(closing_valuation(e.close(0))));
    auto buy = filled(quote_fill(intent(a, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(95)}),
                                 e.open(1), a, costs));
    take(apply_fill(a, ledger, buy));
    require(a.cash.amount() == 50000 && a.basis.amount() == 950000, "hand buy");
    emit("hand", costs, "buy", a, ledger, buy.mark, &buy);
    DividendEvent d{a.trial, e.prior(2), {a.source.currency, 1000000000}, a.units};
    take(apply_dividend(a, ledger, d));
    require(a.cash.amount() == 59500 && a.dividends.amount() == 9500, "hand dividend");
    emit("hand", costs, "dividend", a, ledger, d.prior_close, nullptr, &d);
    auto sell = filled(quote_fill(intent(a, 2, 2, SellAll{}), e.open(2), a, costs));
    take(apply_fill(a, ledger, sell));
    require(a.cash.amount() == 1104500 && a.realized.amount() == 95000 && a.units.raw() == 0,
            "hand sale/PnL");
    emit("hand", costs, "sell", a, ledger, sell.mark, &sell);
    require(take(mark_account(a, sell.mark, ValuationPhase::Open)).equity.amount() -
                    a.initial_cash.amount() ==
                104500,
            "hand total PnL");
    std::cout << "HAND_TRACE cash_cents=1104500 realized_cents=95000 dividends_cents=9500 "
                 "pnl_cents=104500\n";
}
void sizing_and_rejections(const Environment &e) {
    auto a = e.account();
    ExecutionPolicy costs{5, 5};
    auto i = intent(a, 1, 1, take(freeze_cash_budget(a)));
    auto buy = filled(quote_fill(i, e.open(1), a, costs));
    auto spent = buy.gross + buy.fee;
    require(spent.amount() <= 950000 && buy.quantity.raw() > 0, "cash budget not respected");
    auto extra = qd::Quantity::from_raw(buy.quantity.raw() + a.lot_step.raw());
    require((take(settle_gross(buy.executed_price, extra, a.source.currency)) +
             take(settle_fee(buy.executed_price, extra, a.source.currency, costs)))
                    .amount() > 950000,
            "quantity is not maximal affordable lot");
    auto tiny = e.account(1);
    rejection(
        quote_fill(intent(tiny, 1, 1, take(freeze_cash_budget(tiny))), e.open(1), tiny, costs),
        RejectionReason::BelowMinimumLot);
    rejection(quote_fill(intent(a, 1, 1, BuyFixedQuantity{qd::Quantity::from_raw(0)}), e.open(1), a,
                         costs),
              RejectionReason::BelowMinimumLot);
    rejection(quote_fill(intent(a, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(100)}),
                         e.open(1), a, {0, 0}),
              RejectionReason::AllocationCap);
    rejection(quote_fill(intent(a, 1, 2, BuyFixedQuantity{qd::Quantity::from_integer(95)}),
                         e.open(2), a, {0, 0}),
              RejectionReason::InsufficientCash);
    rejection(quote_fill(intent(a, 1, 2, BuyFixedQuantity{qd::Quantity::from_integer(90)}),
                         e.open(2), a, {0, 0}),
              RejectionReason::AllocationCap);
    auto changed = i;
    std::get<BuyCashBudget>(changed.order).cash_revision = 1;
    rejection(quote_fill(changed, e.open(1), a, costs), RejectionReason::CashChanged);
    rejection(quote_fill(intent(a, 1, 2, take(freeze_cash_budget(a))), e.open(1), a, costs),
              RejectionReason::NotEligible);
    rejection(quote_fill(intent(a, 1, 1, SellAll{}), e.open(1), a, costs),
              RejectionReason::NoPosition);
    auto lots = e.account(1000000, 1000000);
    rejects(quote_fill(intent(lots, 1, 1, BuyFixedQuantity{qd::Quantity::from_raw(1)}), e.open(1),
                       lots, costs));
    for (const auto p : std::vector<ExecutionPolicy>{{-1, 5}, {10000, 5}, {5, -1}, {5, 10000}}) {
        rejects(quote_fill(i, e.open(1), a, p));
    }
    auto huge = e.account(INT64_MAX);
    rejects(
        quote_fill(intent(huge, 1, 1, take(freeze_cash_budget(huge))), e.open(1), huge, {0, 0}));
    auto ledger = take(Ledger::create(a, 16));
    take(apply_fill(a, ledger, buy));
    rejection(quote_fill(intent(a, 2, 2, take(freeze_cash_budget(a))), e.open(2), a, costs),
              RejectionReason::PositionAlreadyOpen);
    rejects(mark_account(a, take(closing_valuation(e.close(1))), ValuationPhase::Open));
    rejects(prior_close_valuation(e.close(1), e.open(1)));
    rejects(prior_close_valuation(e.close(0), e.open(2)));
    rejects(quote_terminal_liquidation(e.close(1), a, costs, {TerminalMode::Mark, 3}));
    auto restored = e.account();
    auto old = intent(restored, 3, 1, take(freeze_cash_budget(restored)));
    auto history = take(Ledger::create(restored, 4));
    auto entry =
        filled(quote_fill(intent(restored, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(1)}),
                          e.open(1), restored, {0, 0}));
    take(apply_fill(restored, history, entry));
    auto exit = filled(quote_terminal_liquidation(e.close(1), restored, {0, 0},
                                                  {TerminalMode::SyntheticClose, 2}));
    take(apply_fill(restored, history, exit));
    require(restored.cash == std::get<BuyCashBudget>(old.order).decision_cash &&
                restored.cash_revision == 2,
            "cash flow fixture did not restore the balance");
    rejection(quote_fill(old, e.open(3), restored, {0, 0}), RejectionReason::CashChanged);
}
void atomic_rejections(const Environment &e) {
    auto a = e.account();
    auto ledger = take(Ledger::create(a, 16));
    auto buy = filled(quote_fill(intent(a, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(10)}),
                                 e.open(1), a, {5, 5}));
    for (int variant = 0; variant < 5; ++variant) {
        auto bad = buy;
        switch (variant) {
        case 0:
            bad.quantity = qd::Quantity::from_raw(0);
            break;
        case 1:
            bad.gross = dollars(-1);
            break;
        case 2:
            bad.fee = qd::Money::from_raw(qd::CurrencyCode{"EUR"}, 1);
            break;
        case 3:
            ++bad.expected_revision;
            break;
        case 4:
            ++bad.trial;
            break;
        }
        const auto before = a;
        rejects(apply_fill(a, ledger, bad));
        require(a == before && ledger.rows().empty(), "bad fill was not atomic");
    }
    take(apply_fill(a, ledger, buy));
    const auto held = a;
    rejects(apply_fill(a, ledger, buy));
    require(a == held && ledger.rows().size() == 1, "duplicate/stale fill mutated account");
    auto sale = filled(quote_fill(intent(a, 2, 2, SellAll{}), e.open(2), a, {5, 5}));
    auto oversell = sale;
    oversell.quantity = qd::Quantity::from_raw(a.units.raw() + 1);
    rejects(apply_fill(a, ledger, oversell));
    require(a == held && ledger.rows().size() == 1, "oversell not atomic");
    DividendEvent d{a.trial, e.prior(2), {a.source.currency, 1889000000}, a.units};
    take(apply_dividend(a, ledger, d));
    require(a.dividends.amount() == 1889, "10 units x 1.889");
    invariant(a, ledger, d.prior_close);
    const auto credited = a;
    rejects(apply_dividend(a, ledger, d));
    rejects(apply_fill(a, ledger, sale));
    require(a == credited && ledger.rows().size() == 2,
            "duplicate dividend/stale quote not atomic");
    auto current = filled(quote_fill(intent(a, 2, 2, SellAll{}), e.open(2), a, {5, 5}));
    take(apply_fill(a, ledger, current));
    const auto flat = a;
    rejects(mark_account(a, e.prior(2), ValuationPhase::PriorClose));
    rejects(mark_account(a, take(closing_valuation(e.close(1))), ValuationPhase::Close));
    auto duplicate_id = filled(quote_fill(
        intent(a, 1, 3, BuyFixedQuantity{qd::Quantity::from_integer(1)}), e.open(3), a, {5, 5}));
    rejects(apply_fill(a, ledger, duplicate_id));
    require(a == flat && ledger.rows().size() == 3, "reused fill ID not rejected");
    auto negative = a;
    negative.cash = dollars(-1);
    rejects(quote_fill(intent(negative, 3, 3, SellAll{}), e.open(3), negative, {0, 0}));
    auto limited = e.account();
    auto short_ledger = take(Ledger::create(limited, 1));
    auto f =
        filled(quote_fill(intent(limited, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(1)}),
                          e.open(1), limited, {0, 0}));
    take(apply_fill(limited, short_ledger, f));
    const auto before = limited;
    rejects(apply_dividend(
                limited, short_ledger,
                {limited.trial, e.prior(2), {limited.source.currency, 1000000000}, limited.units}),
            ErrorCode::ResourceExceeded);
    require(limited == before && short_ledger.rows().size() == 1,
            "ledger capacity failure not atomic");
    auto rich = e.account(INT64_MAX);
    auto rich_ledger = take(Ledger::create(rich, 4));
    auto rf = filled(quote_fill(intent(rich, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(1)}),
                                e.open(1), rich, {0, 0}));
    take(apply_fill(rich, rich_ledger, rf));
    const auto rich_before = rich;
    rejects(apply_dividend(rich, rich_ledger,
                           {rich.trial, e.prior(2), {rich.source.currency, INT64_MAX}, rich.units}),
            ErrorCode::NumericRange);
    require(rich == rich_before && rich_ledger.rows().size() == 1, "dividend overflow not atomic");
    auto overflow_sale = filled(quote_fill(intent(rich, 2, 2, SellAll{}), e.open(2), rich, {0, 0}));
    rejects(apply_fill(rich, rich_ledger, overflow_sale), ErrorCode::NumericRange);
    require(rich == rich_before && rich_ledger.rows().size() == 1, "sale cash overflow not atomic");
}
void scenario(const Environment &e, std::string name, ExecutionPolicy costs, bool terminal_sale) {
    auto a = e.account();
    auto ledger = take(Ledger::create(a, 32));
    emit(name, costs, "fund", a, ledger, take(closing_valuation(e.close(0))));
    // Freeze before the ex-date: the zero entitlement event changes sequence, not
    // cash revision, so it does not spuriously invalidate the pending cash budget.
    auto order = intent(a, 1, 1, take(freeze_cash_budget(a)));
    DividendEvent no_entitlement{a.trial, e.prior(1), {a.source.currency, 1889000000}, a.units};
    take(apply_dividend(a, ledger, no_entitlement));
    require(a.dividends.amount() == 0, "ex-date buyer received prior holdings' dividend");
    emit(name, costs, "dividend", a, ledger, no_entitlement.prior_close, nullptr, &no_entitlement);
    auto buy = filled(quote_fill(order, e.open(1), a, costs));
    take(apply_fill(a, ledger, buy));
    emit(name, costs, "buy", a, ledger, buy.mark, &buy);
    const auto after_buy = a;
    rejects(
        apply_dividend(a, ledger, {a.trial, e.prior(1), {a.source.currency, 1889000000}, a.units}));
    require(a == after_buy, "post-opening dividend changed account");
    DividendEvent entitled{a.trial, e.prior(2), {a.source.currency, 1000000000}, a.units};
    take(apply_dividend(a, ledger, entitled));
    emit(name, costs, "dividend", a, ledger, entitled.prior_close, nullptr, &entitled);
    auto sale = filled(quote_fill(intent(a, 2, 2, SellAll{}), e.open(2), a, costs));
    take(apply_fill(a, ledger, sale));
    emit(name, costs, "sell", a, ledger, sale.mark, &sale);
    auto reentry =
        filled(quote_fill(intent(a, 3, 3, take(freeze_cash_budget(a))), e.open(3), a, costs));
    take(apply_fill(a, ledger, reentry));
    emit(name, costs, "buy", a, ledger, reentry.mark, &reentry);
    auto close = take(closing_valuation(e.close(3)));
    const auto before = take(mark_account(a, close, ValuationPhase::Close));
    emit(name, costs, "mark", a, ledger, close);
    if (terminal_sale) {
        auto end = filled(
            quote_terminal_liquidation(e.close(3), a, costs, {TerminalMode::SyntheticClose, 4}));
        require(!end.intent && end.synthetic_terminal && end.mark.phase() == ValuationPhase::Close,
                "terminal fill fabricated opening eligibility");
        take(apply_fill(a, ledger, end));
        emit(name, costs, "terminal", a, ledger, end.mark, &end);
        if (costs.commission_bps) {
            require(a.cash.amount() < before.equity.amount(),
                    "terminal costs failed to reduce final equity");
        }
    }
}
void terminal_drawdown(const Environment &e) {
    auto a = e.account();
    auto ledger = take(Ledger::create(a, 4));
    ExecutionPolicy costs{5, 0};
    auto buy = filled(quote_fill(intent(a, 1, 1, BuyFixedQuantity{qd::Quantity::from_integer(95)}),
                                 e.open(1), a, {0, 0}));
    take(apply_fill(a, ledger, buy));
    const auto close = take(closing_valuation(e.close(1)));
    require(take(mark_account(a, close, ValuationPhase::Close)).equity.amount() == 1000000,
            "preterminal flat mark");
    auto end =
        filled(quote_terminal_liquidation(e.close(1), a, costs, {TerminalMode::SyntheticClose, 2}));
    take(apply_fill(a, ledger, end));
    require(a.cash.amount() == 999525, "terminal fee final equity");
    require(1000000 - a.cash.amount() == 475, "terminal fee must set new reported drawdown");
    invariant(a, ledger, close);
}
void arithmetic_vectors() {
    std::ostringstream out;
    out << "price,quantity,side,commission,slippage,execution,gross,fee,dividend\n";
    std::mt19937_64 random{20261018};
    qd::CurrencyCode usd{"USD"};
    for (unsigned n = 0; n < 512; ++n) {
        const std::int64_t edges[]{1, 4999999, 5000000, 15000000, 100000000001, INT64_MAX};
        const auto p =
            n < 36
                ? edges[n % 6]
                : static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(INT64_MAX)) + 1;
        const auto q =
            n < 36
                ? edges[n / 6]
                : static_cast<std::int64_t>(
                      random() % (n % 2 ? 1000000000ULL : static_cast<std::uint64_t>(INT64_MAX)));
        const ExecutionPolicy policy{static_cast<std::int32_t>(n % 4 == 0   ? 0
                                                               : n % 4 == 1 ? 5
                                                               : n % 4 == 2 ? 10
                                                                            : 9999),
                                     static_cast<std::int32_t>(n % 5 == 0 ? 9999
                                                               : n % 2    ? 5
                                                                          : 0)};
        const auto side = n % 2 ? qd::Side::Buy : qd::Side::Sell;
        const auto value = [](const auto &result, auto raw) {
            if (std::holds_alternative<BacktestError>(result)) {
                return std::string{"error"};
            }
            return std::to_string(raw(std::get<0>(result)));
        };
        out << p << ',' << q << ',' << (side == qd::Side::Buy ? "buy" : "sell") << ','
            << policy.commission_bps << ',' << policy.slippage_bps << ','
            << value(execution_price(qd::Price::from_raw(p), side, policy),
                     [](auto v) { return v.raw(); })
            << ','
            << value(settle_gross(qd::Price::from_raw(p), qd::Quantity::from_raw(q), usd),
                     [](auto v) { return v.amount(); })
            << ','
            << value(settle_fee(qd::Price::from_raw(p), qd::Quantity::from_raw(q), usd, policy),
                     [](auto v) { return v.amount(); })
            << ','
            << value(settle_dividend({usd, p}, qd::Quantity::from_raw(q)),
                     [](auto v) { return v.amount(); })
            << '\n';
    }
    quant::storage::files::write_text(root / "arithmetic.csv", out.str());
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected accounting scratch root");
        std::filesystem::create_directories(argv[1]);
        auto pattern = (std::filesystem::path{argv[1]} / "accounting.XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        require(::mkdtemp(bytes.data()) != nullptr, "accounting scratch directory");
        root = bytes.data();
        trace << "scenario,commission,slippage,lot,initial,event,key,phase,order_kind,decision,"
                 "eligible,budget,requested_units,raw_price,per_unit,quantity,executed_price,gross,"
                 "fee,cash,units,basis,realized,dividends,total_fees,position_value,equity,"
                 "unrealized,terminal\n";
        Environment e{fixture()};
        settlement_boundaries();
        hand_trace(e);
        sizing_and_rejections(e);
        atomic_rejections(e);
        terminal_drawdown(e);
        for (const auto bps : {0, 5, 10, 20}) {
            for (const bool terminal : {false, true}) {
                scenario(e, "cost" + std::to_string(bps) + (terminal ? "_liquidate" : "_mark"),
                         {bps, bps}, terminal);
            }
        }
        arithmetic_vectors();
        quant::storage::files::write_text(root / "ledger.csv", trace.str());
        std::cout << "PASS settlement, hand ledger, sizing/rejections, atomicity, "
                     "phase/dividend/endpoint and drawdown checks\nACCOUNTING_EVIDENCE="
                  << root.string() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
