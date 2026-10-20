#pragma once

#include "quant/backtest/result.hpp"
#include "quant/data/value_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace quant::backtest {

// Independent reporting DTOs. No engine, event loop, filesystem or acceptance
// authority. The caller supplies a validated financial scope with no external
// cash flows after funding. Calendar dates must be valid years 1..9999.
using ReportDate = std::chrono::year_month_day;

struct ReportOvernightPrices {
    data::Price previous_close;
    data::Price opening;
};

struct ReportClosePoint {
    ReportDate date;
    data::Money equity;
    data::Quantity invested_units;
    data::Money invested_value;
    // Carried into this session OR acquired at its open; not inferred from
    // close holdings, because an opening sale also counts in this definition.
    std::optional<bool> legacy_any_session_invested{};
    std::optional<ReportOvernightPrices> overnight_prices{};
};

struct ReportRejectionCount {
    std::string reason;
    std::uint64_t count;
};

struct ReportAccountTotals {
    data::Money final_cash;
    data::Money unrealized_pnl;
    data::Money dividends;
    data::Money fees;
    std::optional<data::Money> slippage_estimate{};
    std::optional<data::Money> cumulative_gross{};
    std::uint64_t rejected_orders = 0;
    std::uint64_t expired_orders = 0;
    // Unique, nonempty ASCII reason labels (<=256 bytes). Renderers escape JSON/
    // HTML text and quote CSV fields. Counts must sum to rejected.
    std::vector<ReportRejectionCount> rejection_reasons{};
};

struct ReportInput {
    ReportDate funded_date;
    data::Money funded_equity;
    std::span<const ReportClosePoint> closes;
    ReportAccountTotals totals;
    // Completed full-sale trading P&L AFTER fees, EXCLUDING dividends. Include
    // a synthetic terminal trade if present. All totals describe that endpoint.
    std::span<const data::Money> closed_trade_pnls;
    // Replaces the last reporting equity; never an extra session. Exposure still
    // uses the ordinary preterminal close. No retroactive risk action is emitted.
    std::optional<data::Money> terminal_cash{};
};

struct ReportRatio {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    friend bool operator==(const ReportRatio &, const ReportRatio &) = default;
};
struct ReportUndefined {
    std::string reason;
};
// Ratios are exact and reduced. Floating alternatives are reporting statistics
// only. Undefined is explicit, never NaN/infinity or a silently omitted sample.
// Floating statistics: volatility, Sharpe, CAGR, mean capital fraction and the
// average of the two central rolling returns for an even-sized median sample.
using ReportMetric = std::variant<ReportUndefined, ReportRatio, long double>;

struct ReportDrawdownEpisode {
    ReportDate peak_date, trough_date;
    std::int64_t peak_equity_cents = 0, trough_equity_cents = 0;
    ReportRatio depth;
    std::optional<ReportDate> recovery_date{};
    bool still_underwater = true;
    // Peak -> first recovery at >=peak, or peak -> last close if unrecovered.
    // Funding index is zero; equal peaks use the latest touch. Equal maximum
    // depths/troughs retain the first occurrence. Calendar days include weekends.
    std::size_t duration_sessions = 0;
    std::int64_t duration_calendar_days = 0;
};

struct ReportDailyPoint {
    ReportDate date;
    std::int64_t ordinary_equity_cents = 0, reporting_equity_cents = 0;
    std::int64_t invested_units_raw = 0, invested_value_cents = 0;
    std::optional<bool> legacy_any_session_invested{};
    ReportMetric daily_return, close_capital_fraction, overnight_asset_gap;
    ReportRatio drawdown;
};
struct ReportPeriod {
    std::string period;
    std::size_t sessions = 0;
    ReportMetric total_return;
};
struct ReportWindow {
    ReportDate start_date, end_date;
    ReportMetric total_return;
};
struct ReportRolling43 {
    std::vector<ReportWindow> windows;
    ReportMetric median, worst, percentage_positive, percentage_at_least_2pct;
};

// Encoded money fields below are signed64 cents in the single named currency;
// unit fields are Quantity6 coefficients. This is a report value, not a ledger.
struct ReportMetrics {
    std::string currency;
    ReportDate funded_date, final_date;
    std::int64_t funded_equity_cents = 0, final_equity_cents = 0;
    std::int64_t ordinary_final_equity_cents = 0, final_cash_cents = 0, final_units_raw = 0;
    std::optional<std::int64_t> terminal_cash_cents{};
    std::int64_t net_pnl_cents = 0, realized_trading_pnl_cents = 0, unrealized_pnl_cents = 0;
    std::int64_t dividends_cents = 0, fees_cents = 0;
    std::optional<std::int64_t> slippage_estimate_cents{}, cumulative_gross_cents{};
    std::int64_t elapsed_calendar_days = 0;
    std::size_t sessions = 0, completed_round_trips = 0, wins = 0, losses = 0, breakeven_trades = 0;
    bool open_position = false, still_underwater = false;
    std::uint64_t rejected_orders = 0, expired_orders = 0;
    std::vector<ReportRejectionCount> rejection_reasons;
    std::size_t observed_overnight_gap_sessions = 0;
    ReportMetric total_return, profit_factor, gross_turnover;
    ReportMetric worst_daily_return, worst_overnight_asset_gap;
    ReportMetric annualized_volatility, sharpe_zero_risk_free, cagr;
    ReportMetric close_invested_fraction, mean_close_capital_fraction, legacy_any_session_exposure;
    ReportRatio maximum_drawdown;
    std::optional<std::size_t> maximum_drawdown_episode{};
    std::vector<ReportDrawdownEpisode> drawdown_episodes;
    std::vector<ReportDailyPoint> daily;
    std::vector<ReportPeriod> monthly_returns, yearly_returns;
    ReportRolling43 rolling_43;
};

struct ReportArtifacts {
    std::string json;
    std::string html;
    // Long-form CSV contains the ENTIRE JSON payload as deterministic leaf
    // paths, types and values (including empty containers and explicit nulls).
    std::string csv;
};
struct PreparedReport {
    ReportMetrics metrics;
    ReportArtifacts artifacts;
};

[[nodiscard]] BacktestResult<ReportMetrics> calculate_report_metrics(const ReportInput &) noexcept;
// Validates and computes before rendering. Returning all strings together lets
// the caller register/write required sinks before engine finish/acceptance.
// No standalone renderer accepts an unvalidated/mutated ReportMetrics object.
// Monetary aggregates (including intermediate sums and separate positive/negative
// trade subtotals) must fit signed64; unsupported ranges return NumericRange.
// Period/window ratios use final/prior-period equity, not a product of rounded
// daily returns. An interior zero does not erase a defined endpoint return.
[[nodiscard]] BacktestResult<PreparedReport> prepare_report(const ReportInput &) noexcept;

} // namespace quant::backtest
