#include "quant/backtest/report.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace quant::backtest;
namespace qd = quant::data;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
template <class T> T take(std::variant<T, BacktestError> result) {
    if (const auto *e = std::get_if<BacktestError>(&result)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(result));
}
template <class T> void rejects(std::variant<T, BacktestError> result, ErrorCode code) {
    const auto *e = std::get_if<BacktestError>(&result);
    require(e && e->code == code && e->phase == Phase::Finalization,
            "wrong/missing report rejection");
}
qd::Money money(std::int64_t cents) {
    return qd::Money::from_raw(qd::CurrencyCode{"USD"}, cents);
}
ReportDate day(int year, unsigned month, unsigned date) {
    return {std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{date}};
}
ReportDate offset(ReportDate date, std::int64_t days) {
    return ReportDate{std::chrono::sys_days{date} + std::chrono::days{days}};
}
ReportRatio exact(const ReportMetric &metric) {
    return std::get<ReportRatio>(metric);
}
long double value(const ReportMetric &metric) {
    if (const auto *r = std::get_if<ReportRatio>(&metric)) {
        return static_cast<long double>(r->numerator) / r->denominator;
    }
    return std::get<long double>(metric);
}
void ratio_is(const ReportMetric &metric, std::int64_t n, std::int64_t d = 1) {
    require(exact(metric) == ReportRatio{n, d}, "wrong exact ratio");
}
void undefined_is(const ReportMetric &metric, std::string_view reason) {
    require(std::get<ReportUndefined>(metric).reason == reason, "wrong undefined reason");
}
void near(long double actual, long double expected) {
    require(std::abs(actual - expected) <= 1e-12L * std::max(1.L, std::abs(expected)),
            "report statistic mismatch");
}

struct Fixture {
    ReportDate funding = day(2025, 1, 1);
    qd::Money initial = money(10000);
    std::vector<ReportClosePoint> rows;
    ReportAccountTotals totals{money(10000), money(0), money(0), money(0)};
    std::vector<qd::Money> trades;
    std::optional<qd::Money> terminal{};
    ReportInput input() const {
        return {funding, initial, rows, totals, trades, terminal};
    }
    ReportMetrics metrics() const {
        return take(calculate_report_metrics(input()));
    }
    PreparedReport report() const {
        return take(prepare_report(input()));
    }
};
Fixture history(const std::vector<std::int64_t> &equities, ReportDate funding = day(2025, 1, 1),
                std::int64_t initial = 10000) {
    Fixture f;
    f.funding = funding;
    f.initial = money(initial);
    for (std::size_t i = 0; i < equities.size(); ++i) {
        f.rows.push_back({offset(funding, static_cast<std::int64_t>(i) + 1), money(equities[i]),
                          qd::Quantity::from_raw(0), money(0)});
    }
    if (!equities.empty()) {
        f.totals.final_cash = money(equities.back());
        f.trades.push_back(money(equities.back() - initial));
    }
    return f;
}
Fixture hand() {
    auto f = history({1000000, 1104500}, day(2025, 1, 1), 1000000);
    f.rows[0].invested_units = qd::Quantity::from_integer(95);
    f.rows[0].invested_value = money(950000);
    f.rows[0].legacy_any_session_invested = f.rows[1].legacy_any_session_invested = true;
    f.totals.dividends = money(9500);
    f.totals.fees = money(100);
    f.totals.slippage_estimate = money(25);
    f.totals.cumulative_gross = money(1995000);
    f.trades = {money(95000)};
    f.rows[0].overnight_prices =
        ReportOvernightPrices{qd::Price::from_integer(100), qd::Price::from_integer(95)};
    f.rows[1].overnight_prices =
        ReportOvernightPrices{qd::Price::from_integer(100), qd::Price::from_integer(102)};
    f.totals.rejected_orders = 3;
    f.totals.expired_orders = 1;
    f.totals.rejection_reasons = {{"gap", 2}, {"<reason>,\"quoted\"\n&", 1}};
    return f;
}
Fixture terminal() {
    auto f = history({11000, 10500});
    for (auto &row : f.rows) {
        row.invested_units = qd::Quantity::from_integer(1);
        row.legacy_any_session_invested = true;
        row.invested_value = money(row.equity.amount() - 1000);
    }
    f.terminal = money(9900);
    f.totals.final_cash = money(9900);
    f.totals.fees = money(600);
    f.trades = {money(-100)};
    return f;
}
Fixture continuous() {
    std::vector<std::int64_t> equities;
    for (int i = 1; i <= 44; ++i) {
        equities.push_back(10000 + 100 * i);
    }
    return history(equities, day(2024, 12, 1));
}
Fixture calendar() {
    auto f = history({12000, 18000, 9000, 9900, 10890}, day(2024, 12, 30));
    const std::vector<ReportDate> dates{day(2024, 12, 31), day(2025, 1, 2), day(2025, 1, 31),
                                        day(2025, 2, 3), day(2026, 1, 2)};
    for (std::size_t i = 0; i < dates.size(); ++i) {
        f.rows[i].date = dates[i];
    }
    return f;
}
std::vector<std::pair<std::string, Fixture>> oracle_cases() {
    return {{"hand", hand()},
            {"terminal", terminal()},
            {"continuous", continuous()},
            {"calendar", calendar()},
            {"flat", history({10000, 10000, 10000})},
            {"zero", history(std::vector<std::int64_t>(44, 0))}};
}

void pnl_exposure_and_diagnostics() {
    const auto m = hand().metrics();
    require(m.net_pnl_cents == 104500 && m.realized_trading_pnl_cents == 95000 &&
                m.dividends_cents == 9500,
            "hand P&L decomposition");
    ratio_is(m.total_return, 209, 2000);
    ratio_is(m.gross_turnover, 399, 200);
    ratio_is(m.close_invested_fraction, 1, 2);
    near(value(m.mean_close_capital_fraction), 19.L / 40);
    ratio_is(m.legacy_any_session_exposure, 1);
    ratio_is(m.worst_overnight_asset_gap, -1, 20);
    require(m.observed_overnight_gap_sessions == 2 && m.fees_cents == 100 &&
                m.slippage_estimate_cents == 25,
            "diagnostics");
    require(m.rejected_orders == 3 && m.expired_orders == 1 &&
                m.rejection_reasons.front().reason.front() == '<',
            "sorted rejection counts");
    require(m.sessions == 2 && m.daily.size() == 2 && m.completed_round_trips == 1 &&
                !m.open_position,
            "funding denominator/count");
    auto partial = hand();
    partial.rows[1].overnight_prices.reset();
    require(partial.metrics().observed_overnight_gap_sessions == 1, "observed gap coverage");
    auto unknown = history({10000, 10000}).metrics();
    undefined_is(unknown.worst_overnight_asset_gap, "overnight_prices_not_supplied");
    undefined_is(unknown.legacy_any_session_exposure, "missing_legacy_session_exposure");
    auto missing = hand();
    missing.rows[1].legacy_any_session_invested.reset();
    undefined_is(missing.metrics().legacy_any_session_exposure, "missing_legacy_session_exposure");
}

void undefined_and_daily_statistics() {
    const auto flat = history({10000, 10000, 10000}).metrics();
    ratio_is(flat.total_return, 0);
    require(flat.maximum_drawdown == ReportRatio{} && !flat.maximum_drawdown_episode &&
                !flat.still_underwater,
            "flat drawdown");
    undefined_is(flat.annualized_volatility, "zero_return_variance");
    undefined_is(flat.sharpe_zero_risk_free, "zero_return_variance");
    undefined_is(flat.profit_factor, "no_losing_trade_pnl");
    undefined_is(flat.rolling_43.median, "fewer_than_43_sessions");
    undefined_is(flat.cagr, "fewer_than_365_calendar_days");
    undefined_is(history({11000}).metrics().annualized_volatility, "fewer_than_two_returns");
    const auto varying = history({11000, 13200}).metrics();
    near(value(varying.annualized_volatility), std::sqrt(1.26L));
    near(value(varying.sharpe_zero_risk_free), .15L / std::sqrt(.005L) * std::sqrt(252.L));
    ratio_is(varying.worst_daily_return, 1, 10);
    const auto symmetric = history({11000, 9900}).metrics();
    near(value(symmetric.annualized_volatility), std::sqrt(5.04L));
    near(value(symmetric.sharpe_zero_risk_free), 0);
    undefined_is(history({11000, 12100, 13310}).metrics().annualized_volatility,
                 "zero_return_variance");
}

void terminal_replacement() {
    auto f = terminal();
    const auto m = f.metrics();
    require(m.sessions == 2 && m.daily.size() == 2 && m.final_units_raw == 0 && !m.open_position,
            "terminal invented a session/holding");
    require(m.final_equity_cents == 9900 && m.net_pnl_cents == -100 &&
                m.ordinary_final_equity_cents == 10500,
            "terminal endpoint");
    ratio_is(m.total_return, -1, 100);
    ratio_is(m.daily.back().daily_return, -1, 10);
    ratio_is(m.maximum_drawdown, 1, 10);
    ratio_is(m.monthly_returns.front().total_return, -1, 100);
    ratio_is(m.close_invested_fraction, 1);
    near(value(m.mean_close_capital_fraction), 419.L / 462);
    require(f.rows.back().equity.amount() == 10500 && f.rows.back().invested_units.raw() == 1000000,
            "mutated input");
    f.terminal.reset();
    f.trades.clear();
    f.totals.final_cash = money(1000);
    f.totals.unrealized_pnl = money(500);
    const auto mark = f.metrics();
    ratio_is(mark.maximum_drawdown, 1, 22);
    require(mark.open_position, "marked open position");
    near(value(mark.mean_close_capital_fraction), value(m.mean_close_capital_fraction));
    auto one = history({10000});
    one.rows[0].invested_units = qd::Quantity::from_integer(1);
    one.rows[0].invested_value = money(9500);
    one.terminal = money(9900);
    one.totals.final_cash = money(9900);
    one.trades = {money(-100)};
    one.totals.fees = money(100);
    const auto last = one.metrics();
    ratio_is(last.maximum_drawdown, 1, 100);
    ratio_is(last.close_invested_fraction, 1);
    require(last.drawdown_episodes.front().duration_sessions == 1, "funding peak duration");
    // Ordinary final peak must not survive replacement in the reporting series.
    auto peak = history({10000, 12000});
    peak.terminal = money(11000);
    peak.totals.final_cash = money(11000);
    peak.trades = {money(1000)};
    ratio_is(peak.metrics().maximum_drawdown, 0);
}

void drawdown_recovery_and_exact_order() {
    auto f = history({12000, 9600, 10800, 12000});
    f.rows[0].date = day(2025, 1, 3);
    f.rows[1].date = day(2025, 1, 6);
    f.rows[2].date = day(2025, 1, 7);
    f.rows[3].date = day(2025, 1, 8);
    const auto m = f.metrics();
    const auto &e = m.drawdown_episodes[*m.maximum_drawdown_episode];
    ratio_is(m.maximum_drawdown, 1, 5);
    require(e.peak_date == day(2025, 1, 3) && e.trough_date == day(2025, 1, 6) &&
                e.recovery_date == day(2025, 1, 8),
            "recovery equality/date");
    require(e.duration_sessions == 3 && e.duration_calendar_days == 5 && !e.still_underwater,
            "recovery duration/weekend");
    const auto later = history({12000, 9600, 12000, 11900}).metrics();
    require(later.still_underwater &&
                !later.drawdown_episodes[*later.maximum_drawdown_episode].still_underwater,
            "maximum versus current episode");
    const auto deeper = history({12000, 9600, 12000, 9000, 9100}).metrics();
    ratio_is(deeper.maximum_drawdown, 1, 4);
    require(deeper.drawdown_episodes.back().duration_sessions == 2, "open duration");
    const auto tied = history({12000, 12000, 9600, 9600, 12000, 9600}).metrics();
    require(*tied.maximum_drawdown_episode == 0 &&
                tied.drawdown_episodes[0].peak_date == day(2025, 1, 3) &&
                tied.drawdown_episodes[0].trough_date == day(2025, 1, 4),
            "declared peak/trough tie rule");
    // These depths differ by about 1e-38 and round equal as floating values.
    const auto huge = history({1, INT64_MAX, 1}, day(2025, 1, 1), INT64_MAX - 1).metrics();
    require(*huge.maximum_drawdown_episode == 1,
            "drawdown compared using floating/cross-product overflow");
    ratio_is(huge.maximum_drawdown, INT64_MAX - 1, INT64_MAX);
    ratio_is(huge.worst_daily_return, -(INT64_MAX - 1), INT64_MAX);
    const auto exact_cent =
        history({(std::int64_t{1} << 60) + 1}, day(2025, 1, 1), std::int64_t{1} << 60).metrics();
    ratio_is(exact_cent.total_return, 1, std::int64_t{1} << 60);
}

void cagr_and_continuous_periods() {
    for (const int elapsed : {364, 365, 366, 731}) {
        auto f = history({12100}, day(2024, 1, 1));
        f.rows.back().date = offset(f.funding, elapsed);
        const auto m = f.metrics();
        if (elapsed < 365) {
            undefined_is(m.cagr, "fewer_than_365_calendar_days");
        } else {
            near(value(m.cagr), std::pow(1.21L, 365.25L / elapsed) - 1);
        }
    }
    auto bankrupt = history({0}, day(2024, 1, 1));
    bankrupt.rows[0].date = offset(bankrupt.funding, 365);
    near(value(bankrupt.metrics().cagr), -1);
    const auto m = calendar().metrics();
    require(m.sessions == 5 && m.monthly_returns.size() == 4 && m.yearly_returns.size() == 3,
            "fabricated sessions/periods");
    ratio_is(m.monthly_returns[0].total_return, 1, 5);
    ratio_is(m.monthly_returns[1].total_return, -1, 4);
    ratio_is(m.monthly_returns[2].total_return, 1, 10);
    ratio_is(m.monthly_returns[3].total_return, 1, 10);
    ratio_is(m.yearly_returns[1].total_return, -7, 40);
    ratio_is(m.yearly_returns[2].total_return, 1, 10);
    const auto rolling = continuous().metrics().rolling_43;
    require(rolling.windows.size() == 2, "rolling count/funding exclusion");
    ratio_is(rolling.windows[0].total_return, 43, 100);
    ratio_is(rolling.windows[1].total_return, 43, 101);
    near(value(rolling.median), (43.L / 100 + 43.L / 101) / 2);
    ratio_is(rolling.worst, 43, 101);
    ratio_is(rolling.percentage_positive, 100);
    ratio_is(rolling.percentage_at_least_2pct, 100);
    auto threshold = history(std::vector<std::int64_t>(44, 10000));
    threshold.rows.back().equity = money(10200);
    threshold.totals.final_cash = money(10200);
    threshold.trades = {money(200)};
    const auto summary = threshold.metrics().rolling_43;
    near(value(summary.median), .01L);
    ratio_is(summary.worst, 0);
    ratio_is(summary.percentage_positive, 50);
    ratio_is(summary.percentage_at_least_2pct, 50);
    const auto one = history(std::vector<std::int64_t>(43, 10000)).metrics().rolling_43;
    require(one.windows.size() == 1, "single complete rolling window");
    ratio_is(one.median, 0);
}

void trades_and_zero_denominators() {
    auto f = history({10900});
    f.rows[0].invested_units = qd::Quantity::from_integer(1);
    f.rows[0].invested_value = money(1000);
    f.totals.final_cash = money(9900);
    f.totals.unrealized_pnl = money(-400);
    f.totals.dividends = money(1000);
    f.trades = {money(500), money(-200), money(0)};
    const auto m = f.metrics();
    require(m.completed_round_trips == 3 && m.wins == 1 && m.losses == 1 && m.breakeven_trades == 1,
            "closed trade classifications");
    ratio_is(m.profit_factor, 5, 2);
    require(m.net_pnl_cents == 900 && m.realized_trading_pnl_cents == 300,
            "dividends/open P&L misclassified");
    ratio_is(history({9000}).metrics().profit_factor, 0);
    auto zero = history(std::vector<std::int64_t>(44, 0)).metrics();
    ratio_is(zero.total_return, -1);
    ratio_is(zero.daily.front().daily_return, -1);
    ratio_is(zero.maximum_drawdown, 1);
    undefined_is(zero.daily[1].daily_return, "zero_previous_equity");
    undefined_is(zero.mean_close_capital_fraction, "zero_close_equity");
    undefined_is(zero.annualized_volatility, "undefined_daily_return");
    undefined_is(zero.worst_daily_return, "undefined_daily_return");
    // Endpoint ratios stay meaningful when their own baseline is positive, even
    // if an interior zero made an individual daily return undefined.
    ratio_is(zero.monthly_returns[0].total_return, -1);
    ratio_is(zero.rolling_43.windows[0].total_return, -1);
    undefined_is(zero.monthly_returns[1].total_return, "zero_previous_equity");
    undefined_is(zero.rolling_43.median, "undefined_rolling_window");
}

void validation_and_reconciliation() {
    auto f = history({10000});
    f.rows.clear();
    rejects(calculate_report_metrics(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.initial = money(0);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows[0].date = f.funding;
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows[0].date = day(2025, 2, 30);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows.push_back(f.rows[0]);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows[0].equity = money(-1);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows[0].invested_value = money(1);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows[0].invested_units = qd::Quantity::from_raw(1);
    f.rows[0].legacy_any_session_invested = false;
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.rows[0].invested_value = qd::Money::from_raw(qd::CurrencyCode{"EUR"}, 0);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.totals.fees = money(-1);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.totals.final_cash = money(9999);
    rejects(prepare_report(f.input()), ErrorCode::Integrity);
    f = history({10000});
    f.trades = {money(1)};
    rejects(prepare_report(f.input()), ErrorCode::Integrity);
    f = history({10000});
    f.totals.unrealized_pnl = money(1);
    rejects(prepare_report(f.input()), ErrorCode::Integrity);
    f = terminal();
    f.terminal = money(10501);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = terminal();
    f.totals.unrealized_pnl = money(1);
    rejects(prepare_report(f.input()), ErrorCode::Integrity);
    f = hand();
    f.totals.rejected_orders = 2;
    rejects(prepare_report(f.input()), ErrorCode::Integrity);
    f = hand();
    f.totals.rejection_reasons.push_back({"gap", 1});
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.trades = {money(INT64_MAX), money(1)};
    rejects(prepare_report(f.input()), ErrorCode::NumericRange);
    f = history({10000});
    f.trades = {money(INT64_MIN)};
    rejects(prepare_report(f.input()), ErrorCode::NumericRange);
    f = hand();
    f.rows[0].overnight_prices->previous_close = qd::Price::from_raw(0);
    rejects(prepare_report(f.input()), ErrorCode::InvalidData);
    f = history({10000});
    f.totals.cumulative_gross = money(0);
    ratio_is(f.metrics().gross_turnover, 0);
}

struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override {
        return ',';
    }
};
void rendering_and_locale() {
    auto f = hand();
    const auto a = f.report().artifacts;
    const auto old = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    const auto b = f.report().artifacts;
    std::locale::global(old);
    require(a.json == b.json && a.csv == b.csv && a.html == b.html,
            "rendering depends on global locale");
    require(a.json.find("\"net_pnl_cents\": 104500") != std::string::npos, "JSON cents");
    require(a.json.find("<reason>,\\\"quoted\\\"\\n&") != std::string::npos, "JSON escaping");
    require(a.html.find("<reason>") == std::string::npos &&
                a.html.find("&lt;reason&gt;") != std::string::npos,
            "HTML escaping");
    require(a.csv.starts_with("path,type,value\r\n"), "CSV header");
    require(a.csv.find("\"/net_pnl_cents\",\"number\",\"104500\"") != std::string::npos,
            "CSV exact integer");
    require(a.csv.find("\"<reason>,\"\"quoted\"\"\n&\"") != std::string::npos, "CSV quoting");
    require(a.json.find("\"value\": null") != std::string::npos &&
                a.csv.find("\"null\",\"null\"") != std::string::npos,
            "undefined encoding");
    require(a.json.find("NaN") == std::string::npos && a.json.find("Infinity") == std::string::npos,
            "nonfinite output");
}

} // namespace

int main(int argc, char **argv) {
    try {
        // Optional stdout-only fixture export for the independent Python oracle.
        if (argc == 2 && std::string_view{argv[1]} == "--oracle-json") {
            std::cout << "{";
            bool first = true;
            for (const auto &[name, fixture] : oracle_cases()) {
                if (!first) {
                    std::cout << ',';
                }
                first = false;
                std::cout << '"' << name << "\":" << fixture.report().artifacts.json;
            }
            std::cout << "}\n";
            return 0;
        }
        if (argc == 2 && (std::string_view{argv[1]} == "--oracle-csv" ||
                          std::string_view{argv[1]} == "--oracle-html")) {
            const auto report = hand().report();
            std::cout << (std::string_view{argv[1]} == "--oracle-csv" ? report.artifacts.csv
                                                                      : report.artifacts.html);
            return 0;
        }
        require(argc == 1, "unknown test option");
        const std::vector<std::pair<std::string_view, void (*)()>> tests{
            {"pnl_exposure_and_diagnostics", pnl_exposure_and_diagnostics},
            {"undefined_and_daily_statistics", undefined_and_daily_statistics},
            {"terminal_replacement", terminal_replacement},
            {"drawdown_recovery_and_exact_order", drawdown_recovery_and_exact_order},
            {"cagr_and_continuous_periods", cagr_and_continuous_periods},
            {"trades_and_zero_denominators", trades_and_zero_denominators},
            {"validation_and_reconciliation", validation_and_reconciliation},
            {"rendering_and_locale", rendering_and_locale}};
        for (const auto &[name, test] : tests) {
            test();
            std::cout << "PASS " << name << '\n';
        }
        std::cout << tests.size() << " reporting groups passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
