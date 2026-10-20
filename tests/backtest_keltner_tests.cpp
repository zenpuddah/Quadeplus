#include "quant/strategies/policies.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/storage/file_integrity.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
namespace qb = quant::backtest;
namespace qd = quant::data;
namespace qs = quant::strategies;
template <class T, class E> T take(std::variant<T, E> result) {
    if (const auto *e = std::get_if<E>(&result)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(result));
}
void require(bool value, std::string_view text) {
    if (!value) {
        throw std::runtime_error(std::string{text});
    }
}
template <class T> void fails(std::variant<T, qb::BacktestError> result, qb::ErrorCode code) {
    const auto *e = std::get_if<qb::BacktestError>(&result);
    require(e && e->code == code, "missing/wrong fatal guard");
}
qd::Price price(std::int64_t dollars) {
    return qd::Price::from_raw(dollars * 1000000000LL);
}
qd::Money money(std::int64_t cents) {
    return qd::Money::from_raw(qd::CurrencyCode{"USD"}, cents);
}
qs::KeltnerState state() {
    qs::KeltnerState s{};
    s.prefix = "t0.";
    s.costs = {0, 0};
    return s;
}
std::vector<qb::PriceFeature> features(std::uint64_t key, std::int64_t close = 100,
                                       std::int64_t ema = 100, std::int64_t atr = 2,
                                       std::int64_t upper = 104) {
    return {{"t0.close", price(close), key, 1},
            {"t0.ema", price(ema), key, 1},
            {"t0.atr", price(atr), key, 1},
            {"t0.upper", price(upper), key, 1},
            {"t0.tr", price(atr), key, 1}};
}
qb::Action decision(qs::KeltnerState &s, const qb::Account &a, std::uint64_t key,
                    std::int64_t close, std::int64_t ema = 100, std::int64_t atr = 2,
                    std::int64_t upper = 104) {
    return take(
        qs::keltner_decision(s, {key, true, true}, &a, features(key, close, ema, atr, upper)));
}
struct Environment {
    quant::research::DailyDataset data;
    auto open(std::uint64_t key) const {
        return take(data.opening(key, data.rows()[key].bar.interval_start.unix_nanos()));
    }
    auto close(std::uint64_t key) const {
        return take(data.closing(key, data.rows()[key].close_available_ns));
    }
    auto account(std::int64_t cash = 1000000, std::int64_t lot = 1) const {
        return take(
            qb::fund_account(1, open(0).source(), money(cash), qd::Quantity::from_raw(lot)));
    }
};
Environment fixture(const std::filesystem::path &root) {
    namespace qr = quant::research;
    namespace files = quant::storage::files;
    const std::string csv =
        "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n2026-01-02,100,100,100,100,1,100,"
        "0\n2026-01-05,100,110,1,100,1,100,0\n2026-01-06,110,120,100,110,1,110,0\n2026-01-07,80,"
        "100,70,80,1,80,0\n2026-01-08,100,100,100,100,1,100,0\n";
    const std::string grid = "Date\n2026-01-02\n2026-01-05\n2026-01-06\n2026-01-07\n2026-01-08\n";
    files::write_text(root / "prices.csv", csv);
    files::write_text(root / "sessions.csv", grid);
    auto resource = take(::lexongine::ResourceAccount::create(
        {{16777216, 16777216, 16777216, 16777216, 16777216, 16777216, 8388608},
         1024,
         1024,
         8192,
         128}));
    qr::DatasetSpec spec{"keltner-unit-v1",
                         "TEST",
                         "USD",
                         "authored-keltner-fixture",
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
    return {take(qr::prepare_daily_dataset({root / "prices.csv",
                                            root / "sessions.csv",
                                            root / "dataset",
                                            files::sha256(csv),
                                            files::sha256(grid),
                                            spec,
                                            {1048576, 8, 64},
                                            resource}))};
}
void integration_fixture(const std::filesystem::path &root, std::string_view mode, unsigned begin,
                         unsigned origin_index = 0) {
    namespace qr = quant::research;
    namespace files = quant::storage::files;
    require(root.is_absolute() &&
                (mode == "trend" || mode == "flat" || mode == "gap" || mode == "crash" ||
                 mode == "reordered") &&
                (begin == 1 || begin == 60) && origin_index < begin,
            "invalid integration fixture arguments");
    require(std::filesystem::create_directory(root), "fixture already exists");
    std::ostringstream csv, grid;
    csv << "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n";
    grid << "Date\n";
    std::vector<std::string> dates;
    const auto origin = std::chrono::sys_days{std::chrono::year{2004} / 11 / 18};
    for (unsigned key = 0; key < 160; ++key) {
        const auto date = std::chrono::year_month_day{origin + std::chrono::days{key}};
        std::ostringstream text;
        text << static_cast<int>(date.year()) << '-' << std::setfill('0') << std::setw(2)
             << static_cast<unsigned>(date.month()) << '-' << std::setw(2)
             << static_cast<unsigned>(date.day());
        dates.push_back(text.str());
        grid << dates.back() << '\n';
        // Reorder complete price tuples, never dates: valid OHLC and the same
        // multiset remain, but the retained source succession changes.
        const auto price_key = mode == "reordered" ? (key == 25   ? 35U
                                                      : key == 35 ? 25U
                                                                  : key)
                                                   : key;
        const std::int64_t close =
            mode == "flat" ? 100
            : mode == "crash"
                ? (price_key < 30    ? 100
                   : price_key < 45  ? 105 + 100 * static_cast<std::int64_t>(price_key - 30)
                   : price_key < 75  ? 20
                   : price_key < 110 ? 120
                                     : 80)
                : (price_key < 30    ? 100
                   : price_key < 70  ? 120
                   : price_key < 110 ? 80
                                     : 125);
        const auto open = mode == "gap" && price_key == 31 ? 2000 : close;
        const auto high = mode == "flat" ? close : std::max(open, close) + 1,
                   low = mode == "flat" ? close : std::min(open, close) - 1;
        csv << dates.back() << ',' << open << ',' << high << ',' << low << ',' << close << ",100,"
            << close << ',' << (price_key == 45 || price_key == 73 ? "1.889" : "0") << '\n';
    }
    const auto prices = csv.str(), sessions = grid.str();
    files::write_text(root / "prices.csv", prices);
    files::write_text(root / "sessions.csv", sessions);
    auto resource = take(::lexongine::ResourceAccount::create(
        {{16777216, 16777216, 16777216, 16777216, 16777216, 16777216, 8388608},
         1024,
         1024,
         8192,
         128}));
    qr::DatasetSpec spec{"keltner-integration-v1",
                         "TEST",
                         "USD",
                         "authored-keltner-integration",
                         1,
                         1,
                         1,
                         "synthetic_session_grid_v1",
                         std::string{qr::adjustment_profile},
                         std::string{qr::decimal_profile},
                         std::string{qr::volume_profile},
                         std::string{qr::clock_profile},
                         dates[origin_index],
                         dates[begin],
                         dates.back()};
    auto result = take(qr::prepare_daily_dataset({root / "prices.csv",
                                                  root / "sessions.csv",
                                                  root / "dataset",
                                                  files::sha256(prices),
                                                  files::sha256(sessions),
                                                  spec,
                                                  {1048576, 256, 64},
                                                  resource}));
    std::cout << "KELTNER_FIXTURE " << result.manifest_sha256() << '\n';
}
qb::Intent intent(const qb::Account &a, std::uint64_t key,
                  std::variant<qb::BuyCashBudget, qb::BuyFixedQuantity, qb::SellAll> order) {
    return {a.trial, key, a.source, key - 1, key, "unit", std::move(order)};
}
qb::Fill fill(qb::BacktestResult<qb::FillQuote> result) {
    auto value = take(std::move(result));
    require(std::holds_alternative<qb::Fill>(value), "expected fill");
    return std::get<qb::Fill>(std::move(value));
}
void rejected(qb::BacktestResult<qb::FillQuote> result, qb::RejectionReason why) {
    auto value = take(std::move(result));
    const auto *r = std::get_if<qb::OrderRejection>(&value);
    require(r && r->reason == why, "wrong ordinary rejection");
}
void crossing_and_guards(const Environment &e) {
    const auto a = e.account();
    auto s = state();
    require(decision(s, a, 19, 105).reason == "no_crossing",
            "first ready value invented a crossing");
    require(decision(s, a, 20, 106).kind == qb::ActionKind::Hold,
            "persistent above-channel value entered");
    require(decision(s, a, 21, 104).kind == qb::ActionKind::Hold, "channel equality entered");
    auto entry = decision(s, a, 22, 105);
    require(entry.kind == qb::ActionKind::FixedEntry && entry.initial_stop_distance == price(4) &&
                entry.quantity.raw() == 12500000,
            "crossing did not freeze exact risk quantity/distance");
    take(qs::keltner_execution(s, qb::OrderRejection{1, 1, qb::RejectionReason::AllocationCap}, a));
    require(!s.held && !s.threshold && decision(s, a, 23, 106).kind == qb::ActionKind::Hold,
            "rejected crossing retried");
    require(decision(s, a, 24, 100).kind == qb::ActionKind::Hold, "reset crossing bought");
    require(decision(s, a, 25, 105).kind == qb::ActionKind::FixedEntry,
            "new crossing not consumed independently");
    auto warm = state();
    require(take(qs::keltner_decision(warm, {18, false, false}, nullptr, {})).reason == "warming",
            "missing features ready");
    require(
        take(qs::keltner_decision(warm, {19, false, false}, nullptr, features(19, 104))).reason ==
            "warmup",
        "numerical prefix traded");
    require(decision(warm, a, 20, 105).kind == qb::ActionKind::FixedEntry,
            "preceding prefix feature was lost");
    auto zero = state();
    decision(zero, a, 0, 100, 100, 0, 100);
    require(decision(zero, a, 1, 101, 100, 0, 100).reason == "zero_atr", "zero ATR entered");
    auto missing = state();
    decision(missing, a, 0, 100);
    fails(qs::keltner_decision(missing, {1, true, true}, &a, {}), qb::ErrorCode::SourceBinding);
    auto skipped = state();
    decision(skipped, a, 0, 100);
    fails(qs::keltner_decision(skipped, {2, true, true}, &a, features(2)), qb::ErrorCode::Coverage);
    auto future = state();
    fails(qs::keltner_decision(future, {0, true, true}, &a, features(1)),
          qb::ErrorCode::FutureRead);
    auto duplicate = state();
    auto f = features(0);
    f.push_back(f.front());
    fails(qs::keltner_decision(duplicate, {0, true, true}, &a, f), qb::ErrorCode::InvalidConfig);
    auto unknown = state();
    f = features(0);
    f[0].request = "t1.close";
    fails(qs::keltner_decision(unknown, {0, true, true}, &a, f), qb::ErrorCode::InvalidConfig);
    auto unfunded = state();
    fails(qs::keltner_decision(unfunded, {0, true, false}, nullptr, features(0)),
          qb::ErrorCode::Integrity);
    auto mismatch = state();
    mismatch.held = true;
    fails(qs::keltner_decision(mismatch, {0, true, true}, &a, features(0)),
          qb::ErrorCode::Integrity);
    auto negative = state();
    f = features(0);
    f[2].value = price(-1);
    fails(qs::keltner_decision(negative, {0, true, true}, &a, f), qb::ErrorCode::InvalidData);
}
void sizing_and_open_guards(const Environment &e) {
    auto a = e.account();
    require(take(qb::keltner_quantity(a, price(100), price(2), {0, 0})).raw() == 12500000,
            "risk sized units wrong");
    // $1.01 equity: a rounded-to-cents risk budget would erase this position.
    require(take(qb::keltner_quantity(e.account(101), price(1), price(1), {0, 0})).raw() == 2525,
            "risk budget rounded before sizing");
    require(take(qb::keltner_quantity(e.account(101, 1000), price(1), price(1), {0, 0})).raw() ==
                2000,
            "risk lot floor wrong");
    require(take(qb::keltner_quantity(e.account(1, 1000000), price(100), price(2), {5, 5})).raw() ==
                0,
            "below-lot entry nonzero");
    auto capped = take(qb::keltner_quantity(a, price(100), qd::Price::from_raw(1), {0, 0}));
    require(capped.raw() == 95000050, "settled half-cent cash boundary lost");
    for (auto costs :
         {qb::ExecutionPolicy{0, 0}, qb::ExecutionPolicy{5, 5}, qb::ExecutionPolicy{20, 20}}) {
        auto q = take(qb::keltner_quantity(a, price(100), qd::Price::from_raw(1), costs));
        auto p = take(qb::execution_price(price(100), qd::Side::Buy, costs));
        auto cost = [&](qd::Quantity units) {
            return take(qb::settle_gross(p, units, a.source.currency)).amount() +
                   take(qb::settle_fee(p, units, a.source.currency, costs)).amount();
        };
        require(cost(q) <= 950000 && cost(qd::Quantity::from_raw(q.raw() + 1)) > 950000,
                "cash-sized lot is not exact maximum");
    }
    fails(qb::keltner_quantity(a, price(100), price(0), {0, 0}), qb::ErrorCode::InvalidData);
    fails(qb::keltner_quantity(e.account(INT64_MAX), qd::Price::from_raw(1), qd::Price::from_raw(1),
                               {0, 0}),
          qb::ErrorCode::NumericRange);
    const auto original = a;
    rejected(
        qb::quote_fill(intent(a, 2, qb::BuyFixedQuantity{capped, price(1)}), e.open(2), a, {0, 0}),
        qb::RejectionReason::InsufficientCash);
    rejected(
        qb::quote_fill(intent(a, 2, qb::BuyFixedQuantity{qd::Quantity::from_integer(90), price(1)}),
                       e.open(2), a, {0, 0}),
        qb::RejectionReason::AllocationCap);
    rejected(qb::quote_fill(
                 intent(a, 1, qb::BuyFixedQuantity{qd::Quantity::from_integer(1), price(100)}),
                 e.open(1), a, {0, 0}),
             qb::RejectionReason::NonpositiveProtectiveThreshold);
    rejected(qb::quote_fill(intent(a, 1, qb::BuyFixedQuantity{qd::Quantity::from_raw(0), price(4)}),
                            e.open(1), a, {0, 0}),
             qb::RejectionReason::BelowMinimumLot);
    fails(
        qb::quote_fill(intent(a, 1, qb::BuyFixedQuantity{qd::Quantity::from_integer(1), price(0)}),
                       e.open(1), a, {0, 0}),
        qb::ErrorCode::InvalidConfig);
    require(a == original, "quote/rejection mutated account");
}
void stops_and_receipts(const Environment &e) {
    auto a = e.account();
    auto ledger = take(qb::Ledger::create(a, 8));
    auto s = state();
    auto buy = fill(
        qb::quote_fill(intent(a, 1, qb::BuyFixedQuantity{qd::Quantity::from_integer(10), price(4)}),
                       e.open(1), a, {5, 5}));
    take(qb::apply_fill(a, ledger, buy));
    take(qs::keltner_execution(s, buy, a));
    require(s.held && s.threshold->raw() == 96050000000LL && !s.highest_close,
            "initial stop did not use actual slipped fill");
    // This day's low is 1; only the completed close can request an exit.
    require(decision(s, a, 1, 100, 100).kind == qb::ActionKind::Hold && a.units.raw() == 10000000,
            "EMA equality/intrabar touch exited");
    require(decision(s, a, 2, 110).kind == qb::ActionKind::Hold && s.threshold == price(106) &&
                s.highest_close == price(110),
            "stop failed to ratchet");
    require(decision(s, a, 3, 108, 100, 10).kind == qb::ActionKind::Hold &&
                s.threshold == price(106),
            "ATR expansion loosened stop");
    require(decision(s, a, 4, 106, 100, 10).reason == "protective_close" &&
                a.units.raw() == 10000000,
            "equal stop must request exit, not fill immediately");
    auto other = state();
    other.held = true;
    other.threshold = price(90);
    require(decision(other, a, 1, 100, 100).kind == qb::ActionKind::Hold, "EMA equality exited");
    require(decision(other, a, 2, 99, 100).reason == "below_ema",
            "strict below EMA failed to exit");
    // Quote and post a next-opening gap sale; it is not a stop-price execution.
    auto sale = fill(qb::quote_fill(intent(a, 3, qb::SellAll{}), e.open(3), a, {0, 0}));
    require(sale.executed_price == price(80), "stop substituted for actual gap open");
    take(qb::apply_fill(a, ledger, sale));
    take(qs::keltner_execution(s, sale, a));
    require(!s.held && !s.threshold && !s.highest_close, "exit retained position state");
    fails(qs::keltner_execution(s, sale, a), qb::ErrorCode::Integrity);
    auto bad = buy;
    bad.trial = 2;
    fails(qs::keltner_execution(s, bad, a), qb::ErrorCode::Integrity);
    auto held = e.account();
    held.units = qd::Quantity::from_integer(1);
    fails(qs::keltner_execution(s, qb::OrderRejection{1, 1, qb::RejectionReason::AllocationCap},
                                held),
          qb::ErrorCode::Integrity);
}
void risk_latch(const Environment &e) {
    auto a = e.account();
    auto ledger = take(qb::Ledger::create(a, 4));
    auto buy = fill(qb::quote_fill(
        intent(a, 1, qb::BuyFixedQuantity{qd::Quantity::from_integer(95)}), e.open(1), a, {0, 0}));
    take(qb::apply_fill(a, ledger, buy));
    auto risk = take(qb::initialize_risk(a.initial_cash));
    auto independent = take(qb::initialize_risk(a.initial_cash));
    const qb::RiskPolicy policy{2000, {}};
    auto check = [&](std::uint64_t key) {
        auto mark = take(qb::closing_valuation(e.close(key)));
        return take(qb::evaluate_risk(
            risk, a, take(qb::mark_account(a, mark, qb::ValuationPhase::Close)), policy));
    };
    require(!check(1).block_entries && !check(2).block_entries &&
                risk.high_water.amount() == 1095000,
            "risk failed to track appreciation");
    auto trigger = check(3);
    require(trigger.new_trigger && trigger.liquidate && trigger.block_entries &&
                trigger.reason == qb::RiskReason::Drawdown,
            "20% high-water breaker failed");
    auto recovery = check(4);
    require(!recovery.new_trigger && recovery.liquidate && recovery.block_entries &&
                !independent.latched,
            "latch reset or leaked between accounts");
    require(take(qb::loss_at_least(money(1000000), money(800000), 2000)) &&
                !take(qb::loss_at_least(money(1000000), money(800001), 2000)),
            "risk threshold equality wrong");
}
void duplicate_account_independence(const Environment &e) {
    auto a = e.account(), b = e.account(2000000);
    a.trial = 2;
    b.trial = 4;
    auto la = take(qb::Ledger::create(a, 8)), lb = take(qb::Ledger::create(b, 8));
    auto sa = state(), sb = state();
    decision(sa, a, 0, 104);
    decision(sb, b, 0, 104);
    const auto entry_a = decision(sa, a, 1, 105), entry_b = decision(sb, b, 1, 105);
    require(entry_a.kind == qb::ActionKind::FixedEntry &&
                entry_b.kind == qb::ActionKind::FixedEntry && entry_a.quantity.raw() == 12500000 &&
                entry_b.quantity.raw() == 25000000,
            "duplicate feature inputs did not size from independent capital");
    auto fa = fill(qb::quote_fill(
        intent(a, 2, qb::BuyFixedQuantity{entry_a.quantity, entry_a.initial_stop_distance}),
        e.open(2), a, {0, 0}));
    const auto untouched = b;
    take(qb::apply_fill(a, la, fa));
    take(qs::keltner_execution(sa, fa, a));
    require(b == untouched && lb.rows().empty() && !sb.held && !sb.threshold && sb.last_key == 1,
            "first duplicate entry mutated other account/state");
    fails(qb::apply_fill(b, lb, fa), qb::ErrorCode::InvalidData);
    require(b == untouched && lb.rows().empty(), "cross-trial fill changed duplicate ledger");
    auto fb = fill(qb::quote_fill(
        intent(b, 2, qb::BuyFixedQuantity{entry_b.quantity, entry_b.initial_stop_distance}),
        e.open(2), b, {0, 0}));
    take(qb::apply_fill(b, lb, fb));
    take(qs::keltner_execution(sb, fb, b));
    require(la.initial().trial == 2 && lb.initial().trial == 4 && la.rows().size() == 1 &&
                lb.rows().size() == 1 && la.rows()[0].after.trial == 2 &&
                lb.rows()[0].after.trial == 4,
            "duplicate entries lost separate ledger identities");
    require(std::get<qb::Fill>(la.rows()[0].event).trial == 2 &&
                std::get<qb::Fill>(lb.rows()[0].event).trial == 4 &&
                a.units.raw() * 2 == b.units.raw(),
            "duplicate fills pooled units/trial identity");
    decision(sa, a, 2, 110);
    const auto held_b = b;
    auto sale = fill(qb::quote_fill(intent(a, 3, qb::SellAll{}), e.open(3), a, {0, 0}));
    take(qb::apply_fill(a, la, sale));
    take(qs::keltner_execution(sa, sale, a));
    require(!sa.held && !sa.threshold && b == held_b && lb.rows().size() == 1 && sb.held &&
                sb.threshold == price(106) && !sb.highest_close && sb.last_key == 1,
            "duplicate sale leaked into other position/stop/history");
    require(decision(sb, b, 2, 110).reason == "held" && sb.highest_close == price(110) &&
                a.units.raw() == 0 && b.units.raw() == 25000000,
            "remaining duplicate failed to advance independently");
    take(qb::verify_financial_invariants(
        a, la, take(qb::mark_account(a, sale.mark, qb::ValuationPhase::Open))));
    take(qb::verify_financial_invariants(
        b, lb,
        take(qb::mark_account(b, take(qb::closing_valuation(e.close(2))),
                              qb::ValuationPhase::Close))));
}
} // namespace
int main(int argc, char **argv) {
    try {
        if ((argc == 5 || argc == 6) && std::string_view{argv[1]} == "--fixture") {
            integration_fixture(argv[2], argv[3], static_cast<unsigned>(std::stoul(argv[4])),
                                argc == 6 ? static_cast<unsigned>(std::stoul(argv[5])) : 0);
            return 0;
        }
        require(argc == 2, "usage: backtest_keltner_tests SCRATCH_ROOT (or --fixture ABSOLUTE_OUT "
                           "trend|flat|gap|crash|reordered 1|60 [ORIGIN_INDEX])");
        std::filesystem::create_directories(argv[1]);
        auto pattern = (std::filesystem::absolute(argv[1]) / "keltner.XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        require(::mkdtemp(bytes.data()) != nullptr, "cannot create isolated fixture");
        const auto e = fixture(bytes.data());
        crossing_and_guards(e);
        sizing_and_open_guards(e);
        stops_and_receipts(e);
        risk_latch(e);
        duplicate_account_independence(e);
        std::cout << "PASS Keltner crossing/guards, exact sizing/open rejection, execution stops, "
                     "independent risk latch and duplicate-account isolation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
