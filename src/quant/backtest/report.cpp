#include "quant/backtest/report.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <new>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace quant::backtest {
namespace {
[[noreturn]] void fail(ErrorCode code, std::string_view message) {
    throw error(code, message, Phase::Finalization);
}

template <class F> auto guarded(F &&action) noexcept -> decltype(action()) {
    try {
        return action();
    } catch (const BacktestError &e) {
        return e;
    } catch (const std::bad_alloc &) {
        return error(ErrorCode::ResourceExceeded, "report allocation failed", Phase::Finalization);
    } catch (const std::length_error &) {
        return error(ErrorCode::ResourceExceeded, "report size exceeded", Phase::Finalization);
    } catch (const std::overflow_error &) {
        return error(ErrorCode::NumericRange, "report cents aggregate exceeds signed64",
                     Phase::Finalization);
    } catch (const std::exception &e) {
        return error(ErrorCode::InvalidData, e.what(), Phase::Finalization);
    } catch (...) {
        return error(ErrorCode::InvalidData, "unexpected report failure", Phase::Finalization);
    }
}

std::int64_t add(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        fail(ErrorCode::NumericRange, "report cents aggregate exceeds signed64");
    }
    return a + b;
}

std::uint64_t magnitude(std::int64_t n) {
    return n < 0 ? static_cast<std::uint64_t>(-(n + 1)) + 1 : static_cast<std::uint64_t>(n);
}

ReportRatio ratio(std::int64_t numerator, std::int64_t denominator) {
    if (denominator <= 0) {
        fail(ErrorCode::InvalidData, "nonpositive reporting ratio denominator");
    }
    const auto divisor = static_cast<std::int64_t>(
        std::gcd(magnitude(numerator), static_cast<std::uint64_t>(denominator)));
    return {numerator / divisor, denominator / divisor};
}

// Euclidean/continued-fraction ordering; no products, floating comparison,
// compiler intrinsics, or overflow even at the signed64 coefficient boundaries.
int compare_positive(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    int direction = 1;
    for (;;) {
        if (a / b != c / d) {
            return direction * (a / b < c / d ? -1 : 1);
        }
        const auto r = a % b, s = c % d;
        if (!r || !s) {
            return direction * (r == s ? 0 : !r ? -1 : 1);
        }
        a = b;
        b = r;
        c = d;
        d = s;
        direction = -direction;
    }
}
int compare(ReportRatio a, ReportRatio b) {
    if ((a.numerator < 0) != (b.numerator < 0)) {
        return a.numerator < 0 ? -1 : 1;
    }
    const auto order =
        compare_positive(magnitude(a.numerator), static_cast<std::uint64_t>(a.denominator),
                         magnitude(b.numerator), static_cast<std::uint64_t>(b.denominator));
    return a.numerator < 0 ? -order : order;
}
long double number(ReportRatio r) {
    return static_cast<long double>(r.numerator) / static_cast<long double>(r.denominator);
}
ReportMetric undefined(std::string reason) {
    return ReportUndefined{std::move(reason)};
}
ReportMetric approximate(long double value) {
    return std::isfinite(value) ? ReportMetric{value} : undefined("floating_point_range");
}
ReportMetric endpoint_return(std::int64_t end, std::int64_t begin) {
    return begin == 0 ? undefined("zero_previous_equity") : ReportMetric{ratio(end - begin, begin)};
}
std::int64_t calendar_days(ReportDate begin, ReportDate end) {
    return (std::chrono::sys_days{end} - std::chrono::sys_days{begin}).count();
}
void valid_date(ReportDate day) {
    if (!day.ok() || int(day.year()) < 1 || int(day.year()) > 9999) {
        fail(ErrorCode::InvalidData, "report date must be a valid calendar date in years 1..9999");
    }
}
std::string date_text(ReportDate day) {
    auto out = std::ostringstream{};
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(4) << int(day.year()) << '-' << std::setw(2)
        << unsigned(day.month()) << '-' << std::setw(2) << unsigned(day.day());
    return out.str();
}

void validate(const ReportInput &input) {
    valid_date(input.funded_date);
    if (input.funded_equity.amount() <= 0 || input.closes.empty()) {
        fail(ErrorCode::InvalidData,
             "report requires positive funding and at least one ordinary close");
    }
    if (input.closes.size() > static_cast<std::uint64_t>(INT64_MAX) / 100) {
        fail(ErrorCode::ResourceExceeded, "report session count exceeds supported ratio range");
    }
    const auto &currency = input.funded_equity.currency();
    const auto money = [&](const data::Money &value, bool nonnegative = true) {
        if (value.currency() != currency) {
            fail(ErrorCode::InvalidData, "report currency mismatch");
        }
        if (nonnegative && value.amount() < 0) {
            fail(ErrorCode::InvalidData, "negative report balance/cost");
        }
    };
    auto previous = input.funded_date;
    for (const auto &row : input.closes) {
        valid_date(row.date);
        if (row.date <= previous) {
            fail(ErrorCode::InvalidData,
                 "ordinary close dates must strictly increase after funding");
        }
        previous = row.date;
        money(row.equity);
        money(row.invested_value);
        if (row.invested_value.amount() > row.equity.amount() ||
            (row.invested_units.is_zero() && row.invested_value.amount() != 0)) {
            fail(ErrorCode::InvalidData, "inconsistent close cash/units/value");
        }
        if (row.legacy_any_session_invested == false && row.invested_units.is_positive()) {
            fail(ErrorCode::InvalidData, "close holdings contradict any-session exposure");
        }
        if (row.overnight_prices && (!row.overnight_prices->previous_close.is_positive() ||
                                     !row.overnight_prices->opening.is_positive())) {
            fail(ErrorCode::InvalidData, "overnight asset prices must be positive");
        }
    }
    const auto &totals = input.totals;
    money(totals.final_cash);
    money(totals.unrealized_pnl, false);
    money(totals.dividends);
    money(totals.fees);
    if (totals.slippage_estimate) {
        money(*totals.slippage_estimate);
    }
    if (totals.cumulative_gross) {
        money(*totals.cumulative_gross);
    }
    for (const auto &trade : input.closed_trade_pnls) {
        money(trade, false);
    }
    const auto &last = input.closes.back();
    auto expected_cash = last.equity.amount() - last.invested_value.amount();
    if (input.terminal_cash) {
        money(*input.terminal_cash);
        expected_cash = input.terminal_cash->amount();
        if (expected_cash > last.equity.amount()) {
            fail(ErrorCode::InvalidData, "costed terminal cash exceeds ordinary close equity");
        }
    }
    if (totals.final_cash.amount() != expected_cash) {
        fail(ErrorCode::Integrity, "final cash does not reconcile with endpoint");
    }
    if ((input.terminal_cash || last.invested_units.is_zero()) &&
        totals.unrealized_pnl.amount() != 0) {
        fail(ErrorCode::Integrity, "flat endpoint has unrealized P&L");
    }
    std::uint64_t rejected = 0;
    for (std::size_t i = 0; i < totals.rejection_reasons.size(); ++i) {
        const auto &entry = totals.rejection_reasons[i];
        if (entry.reason.empty() || entry.reason.size() > 256 || entry.count == 0 ||
            std::any_of(entry.reason.begin(), entry.reason.end(),
                        [](unsigned char c) { return c > 127; })) {
            fail(ErrorCode::InvalidData, "invalid rejection reason/count");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (entry.reason == totals.rejection_reasons[j].reason) {
                fail(ErrorCode::InvalidData, "duplicate rejection reason");
            }
        }
        if (entry.count > UINT64_MAX - rejected) {
            fail(ErrorCode::NumericRange, "rejected-order count overflow");
        }
        rejected += entry.count;
    }
    if (rejected != totals.rejected_orders) {
        fail(ErrorCode::Integrity, "rejection reasons do not reconcile with count");
    }
}

// Compensated floating accumulation is used only for reporting averages/variance.
struct Sum {
    long double total = 0, correction = 0;
    void add(long double value) {
        const auto next = total + value;
        correction +=
            std::abs(total) >= std::abs(value) ? (total - next) + value : (value - next) + total;
        total = next;
    }
    long double value() const {
        return total + correction;
    }
};

void daily_statistics(ReportMetrics &out) {
    Sum sum;
    bool equal = true;
    const auto *first = std::get_if<ReportRatio>(&out.daily.front().daily_return);
    for (const auto &row : out.daily) {
        const auto *r = std::get_if<ReportRatio>(&row.daily_return);
        if (!r) {
            out.annualized_volatility = out.sharpe_zero_risk_free =
                undefined("undefined_daily_return");
            return;
        }
        sum.add(number(*r));
        equal = equal && first && compare(*r, *first) == 0;
    }
    if (out.daily.size() < 2 || equal) {
        out.annualized_volatility = out.sharpe_zero_risk_free =
            undefined(out.daily.size() < 2 ? "fewer_than_two_returns" : "zero_return_variance");
        return;
    }
    const auto mean = sum.value() / static_cast<long double>(out.daily.size());
    Sum squared;
    for (const auto &row : out.daily) {
        const auto centered = number(std::get<ReportRatio>(row.daily_return)) - mean;
        squared.add(centered * centered);
    }
    const auto stddev = std::sqrt(squared.value() / static_cast<long double>(out.daily.size() - 1));
    if (stddev == 0 || !std::isfinite(stddev)) {
        out.annualized_volatility = out.sharpe_zero_risk_free = undefined("floating_point_range");
        return;
    }
    out.annualized_volatility = approximate(stddev * std::sqrt(252.L));
    out.sharpe_zero_risk_free = approximate(mean / stddev * std::sqrt(252.L));
}

void drawdowns(ReportMetrics &out) {
    auto peak = out.funded_equity_cents;
    auto peak_date = out.funded_date;
    std::size_t peak_index = 0, active_peak_index = 0;
    std::optional<std::size_t> active;
    for (std::size_t i = 0; i < out.daily.size(); ++i) {
        auto &row = out.daily[i];
        row.drawdown = ratio(std::max<std::int64_t>(0, peak - row.reporting_equity_cents), peak);
        if (row.reporting_equity_cents >= peak) {
            if (active) {
                auto &episode = out.drawdown_episodes[*active];
                episode.recovery_date = row.date;
                episode.still_underwater = false;
                episode.duration_sessions = i + 1 - active_peak_index;
                episode.duration_calendar_days = calendar_days(episode.peak_date, row.date);
                active.reset();
            }
            peak = row.reporting_equity_cents;
            peak_date = row.date;
            peak_index = i + 1;
        } else {
            if (!active) {
                active = out.drawdown_episodes.size();
                active_peak_index = peak_index;
                out.drawdown_episodes.push_back(
                    {peak_date, row.date, peak, row.reporting_equity_cents, row.drawdown});
            }
            auto &episode = out.drawdown_episodes[*active];
            if (compare(row.drawdown, episode.depth) > 0) {
                episode.depth = row.drawdown;
                episode.trough_date = row.date;
                episode.trough_equity_cents = row.reporting_equity_cents;
            }
            if (compare(row.drawdown, out.maximum_drawdown) > 0) {
                out.maximum_drawdown = row.drawdown;
                out.maximum_drawdown_episode = active;
            }
        }
    }
    out.still_underwater = active.has_value();
    if (active) {
        auto &episode = out.drawdown_episodes[*active];
        episode.duration_sessions = out.daily.size() - active_peak_index;
        episode.duration_calendar_days = calendar_days(episode.peak_date, out.final_date);
    }
}

std::vector<ReportPeriod> period_returns(const ReportMetrics &out, bool yearly) {
    std::vector<ReportPeriod> periods;
    for (std::size_t begin = 0; begin < out.daily.size();) {
        const auto key = date_text(out.daily[begin].date).substr(0, yearly ? 4 : 7);
        auto end = begin + 1;
        while (end < out.daily.size() &&
               date_text(out.daily[end].date).substr(0, yearly ? 4 : 7) == key) {
            ++end;
        }
        const auto baseline =
            begin ? out.daily[begin - 1].reporting_equity_cents : out.funded_equity_cents;
        periods.push_back({key, end - begin,
                           endpoint_return(out.daily[end - 1].reporting_equity_cents, baseline)});
        begin = end;
    }
    return periods;
}

void rolling_returns(ReportMetrics &out) {
    auto &rolling = out.rolling_43;
    std::vector<ReportRatio> values;
    for (std::size_t end = 42; end < out.daily.size(); ++end) {
        const auto baseline =
            end == 42 ? out.funded_equity_cents : out.daily[end - 43].reporting_equity_cents;
        auto result = endpoint_return(out.daily[end].reporting_equity_cents, baseline);
        if (const auto *r = std::get_if<ReportRatio>(&result)) {
            values.push_back(*r);
        }
        rolling.windows.push_back(
            {out.daily[end - 42].date, out.daily[end].date, std::move(result)});
    }
    if (values.size() != rolling.windows.size() || values.empty()) {
        rolling.median = rolling.worst = rolling.percentage_positive =
            rolling.percentage_at_least_2pct = undefined(
                rolling.windows.empty() ? "fewer_than_43_sessions" : "undefined_rolling_window");
        return;
    }
    std::sort(values.begin(), values.end(), [](auto a, auto b) { return compare(a, b) < 0; });
    const auto count = static_cast<std::int64_t>(values.size());
    const auto mid = values.size() / 2;
    rolling.median = values.size() % 2
                         ? ReportMetric{values[mid]}
                         : approximate((number(values[mid - 1]) + number(values[mid])) / 2);
    rolling.worst = values.front();
    const auto positive =
        std::count_if(values.begin(), values.end(), [](auto r) { return r.numerator > 0; });
    const auto target = std::count_if(values.begin(), values.end(),
                                      [](auto r) { return compare(r, {1, 50}) >= 0; });
    rolling.percentage_positive = ratio(100 * positive, count);
    rolling.percentage_at_least_2pct = ratio(100 * target, count);
}

ReportMetrics compute(const ReportInput &input) {
    validate(input);
    ReportMetrics out;
    out.currency = input.funded_equity.currency().value();
    out.funded_date = input.funded_date;
    out.funded_equity_cents = input.funded_equity.amount();
    out.final_date = input.closes.back().date;
    out.sessions = input.closes.size();
    out.elapsed_calendar_days = calendar_days(out.funded_date, out.final_date);
    out.ordinary_final_equity_cents = input.closes.back().equity.amount();
    out.final_equity_cents =
        input.terminal_cash ? input.terminal_cash->amount() : out.ordinary_final_equity_cents;
    if (input.terminal_cash) {
        out.terminal_cash_cents = input.terminal_cash->amount();
    }
    out.final_cash_cents = input.totals.final_cash.amount();
    out.final_units_raw = input.terminal_cash ? 0 : input.closes.back().invested_units.raw();
    out.open_position = out.final_units_raw > 0;
    out.net_pnl_cents = out.final_equity_cents - out.funded_equity_cents;
    out.total_return = ratio(out.net_pnl_cents, out.funded_equity_cents);
    out.unrealized_pnl_cents = input.totals.unrealized_pnl.amount();
    out.dividends_cents = input.totals.dividends.amount();
    out.fees_cents = input.totals.fees.amount();
    if (input.totals.slippage_estimate) {
        out.slippage_estimate_cents = input.totals.slippage_estimate->amount();
    }
    if (input.totals.cumulative_gross) {
        out.cumulative_gross_cents = input.totals.cumulative_gross->amount();
    }
    out.gross_turnover =
        out.cumulative_gross_cents
            ? ReportMetric{ratio(*out.cumulative_gross_cents, out.funded_equity_cents)}
            : undefined("gross_not_supplied");
    out.rejected_orders = input.totals.rejected_orders;
    out.expired_orders = input.totals.expired_orders;
    out.rejection_reasons = input.totals.rejection_reasons;
    std::sort(out.rejection_reasons.begin(), out.rejection_reasons.end(),
              [](const auto &a, const auto &b) { return a.reason < b.reason; });
    std::int64_t profits = 0, losses = 0;
    for (const auto &trade : input.closed_trade_pnls) {
        const auto pnl = trade.amount();
        if (pnl == INT64_MIN) {
            fail(ErrorCode::NumericRange, "absolute trade loss exceeds signed64");
        }
        out.realized_trading_pnl_cents = add(out.realized_trading_pnl_cents, pnl);
        if (pnl > 0) {
            ++out.wins;
            profits = add(profits, pnl);
        } else if (pnl < 0) {
            ++out.losses;
            losses = add(losses, -pnl);
        } else {
            ++out.breakeven_trades;
        }
    }
    if (add(add(out.realized_trading_pnl_cents, out.unrealized_pnl_cents), out.dividends_cents) !=
        out.net_pnl_cents) {
        fail(ErrorCode::Integrity, "net P&L must equal realized trading + unrealized + dividends");
    }
    out.completed_round_trips = input.closed_trade_pnls.size();
    out.profit_factor =
        losses ? ReportMetric{ratio(profits, losses)} : undefined("no_losing_trade_pnl");
    std::int64_t previous = out.funded_equity_cents, invested = 0, legacy = 0;
    bool known_legacy = true, known_capital = true, known_returns = true;
    std::optional<ReportRatio> worst_return, worst_gap;
    Sum capital;
    out.daily.reserve(input.closes.size());
    for (std::size_t i = 0; i < input.closes.size(); ++i) {
        const auto &source = input.closes[i];
        ReportDailyPoint row;
        row.date = source.date;
        row.ordinary_equity_cents = source.equity.amount();
        row.reporting_equity_cents =
            i + 1 == out.sessions ? out.final_equity_cents : source.equity.amount();
        row.invested_units_raw = source.invested_units.raw();
        row.invested_value_cents = source.invested_value.amount();
        row.legacy_any_session_invested = source.legacy_any_session_invested;
        row.daily_return = endpoint_return(row.reporting_equity_cents, previous);
        previous = row.reporting_equity_cents;
        if (const auto *r = std::get_if<ReportRatio>(&row.daily_return)) {
            if (!worst_return || compare(*r, *worst_return) < 0) {
                worst_return = *r;
            }
        } else {
            known_returns = false;
        }
        row.close_capital_fraction =
            source.equity.amount()
                ? ReportMetric{ratio(source.invested_value.amount(), source.equity.amount())}
                : undefined("zero_close_equity");
        if (const auto *r = std::get_if<ReportRatio>(&row.close_capital_fraction)) {
            capital.add(number(*r));
        } else {
            known_capital = false;
        }
        invested += source.invested_units.is_positive() ? 1 : 0;
        legacy += source.legacy_any_session_invested.value_or(false) ? 1 : 0;
        known_legacy = known_legacy && source.legacy_any_session_invested.has_value();
        row.overnight_asset_gap = undefined("overnight_prices_not_supplied");
        if (source.overnight_prices) {
            const auto &prices = *source.overnight_prices;
            const auto gap = ratio(prices.opening.raw() - prices.previous_close.raw(),
                                   prices.previous_close.raw());
            row.overnight_asset_gap = gap;
            ++out.observed_overnight_gap_sessions;
            if (!worst_gap || compare(gap, *worst_gap) < 0) {
                worst_gap = gap;
            }
        }
        out.daily.push_back(std::move(row));
    }
    out.worst_daily_return =
        known_returns ? ReportMetric{*worst_return} : undefined("undefined_daily_return");
    out.worst_overnight_asset_gap =
        worst_gap ? ReportMetric{*worst_gap} : undefined("overnight_prices_not_supplied");
    out.close_invested_fraction = ratio(invested, static_cast<std::int64_t>(out.sessions));
    out.legacy_any_session_exposure =
        known_legacy ? ReportMetric{ratio(legacy, static_cast<std::int64_t>(out.sessions))}
                     : undefined("missing_legacy_session_exposure");
    out.mean_close_capital_fraction =
        known_capital ? approximate(capital.value() / static_cast<long double>(out.sessions))
                      : undefined("zero_close_equity");
    daily_statistics(out);
    drawdowns(out);
    out.cagr = undefined("fewer_than_365_calendar_days");
    if (out.elapsed_calendar_days >= 365) {
        if (out.final_equity_cents == 0) {
            out.cagr = -1.L;
        } else {
            const auto total = number(std::get<ReportRatio>(out.total_return));
            const auto log_growth =
                total > -0.5L ? std::log1p(total)
                              : std::log(static_cast<long double>(out.final_equity_cents) /
                                         static_cast<long double>(out.funded_equity_cents));
            out.cagr = approximate(std::expm1(log_growth * 365.25L /
                                              static_cast<long double>(out.elapsed_calendar_days)));
        }
    }
    out.monthly_returns = period_returns(out, false);
    out.yearly_returns = period_returns(out, true);
    rolling_returns(out);
    return out;
}

// A small private rendering value tree ensures HTML/JSON/CSV contain exactly
// the same metrics. It is not a parser, persistence format or dependency layer.
struct Json {
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;
    std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, long double, std::string, Array,
                 Object>
        value;
    Json(std::nullptr_t = nullptr) : value(nullptr) {}
    Json(bool v) : value(v) {}
    Json(std::int64_t v) : value(v) {}
    Json(std::uint64_t v) : value(v) {}
    Json(long double v) : value(v) {}
    Json(std::string v) : value(std::move(v)) {}
    Json(const char *v) : value(std::string{v}) {}
    Json(Array v) : value(std::move(v)) {}
    Json(Object v) : value(std::move(v)) {}
};
Json object(std::initializer_list<std::pair<std::string, Json>> entries) {
    return Json::Object{entries};
}
Json encoded_ratio(ReportRatio r) {
    return object({{"numerator", r.numerator}, {"denominator", r.denominator}});
}
Json metric(const ReportMetric &value) {
    if (const auto *u = std::get_if<ReportUndefined>(&value)) {
        return object({{"value", nullptr}, {"undefined_reason", u->reason}});
    }
    if (const auto *r = std::get_if<ReportRatio>(&value)) {
        return object({{"value", encoded_ratio(*r)}, {"undefined_reason", nullptr}});
    }
    return object({{"value", std::get<long double>(value)}, {"undefined_reason", nullptr}});
}
Json optional_money(std::optional<std::int64_t> value) {
    return value ? Json{*value} : Json{};
}
Json episode_json(const ReportDrawdownEpisode &e) {
    return object({{"peak_date", date_text(e.peak_date)},
                   {"trough_date", date_text(e.trough_date)},
                   {"peak_equity_cents", e.peak_equity_cents},
                   {"trough_equity_cents", e.trough_equity_cents},
                   {"depth", metric(e.depth)},
                   {"recovery_date", e.recovery_date ? Json{date_text(*e.recovery_date)} : Json{}},
                   {"still_underwater", e.still_underwater},
                   {"duration_sessions", static_cast<std::uint64_t>(e.duration_sessions)},
                   {"duration_calendar_days", e.duration_calendar_days}});
}
Json periods_json(const std::vector<ReportPeriod> &periods) {
    Json::Array result;
    for (const auto &row : periods) {
        result.push_back(object({{"period", row.period},
                                 {"sessions", static_cast<std::uint64_t>(row.sessions)},
                                 {"total_return", metric(row.total_return)}}));
    }
    return result;
}

Json payload(const ReportMetrics &m) {
    Json::Array daily, episodes, windows, rejections;
    for (const auto &row : m.daily) {
        daily.push_back(object(
            {{"date", date_text(row.date)},
             {"ordinary_equity_cents", row.ordinary_equity_cents},
             {"reporting_equity_cents", row.reporting_equity_cents},
             {"invested_units_raw", row.invested_units_raw},
             {"invested_value_cents", row.invested_value_cents},
             {"legacy_any_session_invested",
              row.legacy_any_session_invested ? Json{*row.legacy_any_session_invested} : Json{}},
             {"daily_return", metric(row.daily_return)},
             {"drawdown", metric(row.drawdown)},
             {"close_capital_fraction", metric(row.close_capital_fraction)},
             {"overnight_asset_gap", metric(row.overnight_asset_gap)}}));
    }
    for (const auto &episode : m.drawdown_episodes) {
        episodes.push_back(episode_json(episode));
    }
    for (const auto &row : m.rolling_43.windows) {
        windows.push_back(object({{"start_date", date_text(row.start_date)},
                                  {"end_date", date_text(row.end_date)},
                                  {"total_return", metric(row.total_return)}}));
    }
    for (const auto &rejection : m.rejection_reasons) {
        rejections.push_back(object({{"reason", rejection.reason}, {"count", rejection.count}}));
    }
    return object(
        {{"schema", "quadeplus-native-report-preparatory-v1"},
         {"currency", m.currency},
         {"funded_date", date_text(m.funded_date)},
         {"funded_equity_cents", m.funded_equity_cents},
         {"final_date", date_text(m.final_date)},
         {"elapsed_calendar_days", m.elapsed_calendar_days},
         {"sessions", static_cast<std::uint64_t>(m.sessions)},
         {"terminal_policy", m.terminal_cash_cents ? "synthetic_cash_replacement" : "mark"},
         {"terminal_cash_cents", optional_money(m.terminal_cash_cents)},
         {"ordinary_final_equity_cents", m.ordinary_final_equity_cents},
         {"final_equity_cents", m.final_equity_cents},
         {"net_pnl_cents", m.net_pnl_cents},
         {"total_return", metric(m.total_return)},
         {"realized_trading_pnl_cents", m.realized_trading_pnl_cents},
         {"unrealized_pnl_cents", m.unrealized_pnl_cents},
         {"dividends_cents", m.dividends_cents},
         {"final_cash_cents", m.final_cash_cents},
         {"final_units_raw", m.final_units_raw},
         {"open_position", m.open_position},
         {"fees_cents", m.fees_cents},
         {"slippage_estimate_cents", optional_money(m.slippage_estimate_cents)},
         {"cumulative_gross_cents", optional_money(m.cumulative_gross_cents)},
         {"gross_turnover", metric(m.gross_turnover)},
         {"rejected_orders", m.rejected_orders},
         {"expired_orders", m.expired_orders},
         {"rejection_reasons", std::move(rejections)},
         {"completed_round_trips", static_cast<std::uint64_t>(m.completed_round_trips)},
         {"wins", static_cast<std::uint64_t>(m.wins)},
         {"losses", static_cast<std::uint64_t>(m.losses)},
         {"breakeven_trades", static_cast<std::uint64_t>(m.breakeven_trades)},
         {"profit_factor", metric(m.profit_factor)},
         {"drawdown", object({{"maximum", metric(m.maximum_drawdown)},
                              {"maximum_episode",
                               m.maximum_drawdown_episode
                                   ? episode_json(m.drawdown_episodes[*m.maximum_drawdown_episode])
                                   : Json{}},
                              {"episodes", std::move(episodes)},
                              {"still_underwater", m.still_underwater}})},
         {"worst_daily_return", metric(m.worst_daily_return)},
         {"worst_overnight_asset_gap", metric(m.worst_overnight_asset_gap)},
         {"observed_overnight_gap_sessions",
          static_cast<std::uint64_t>(m.observed_overnight_gap_sessions)},
         {"annualized_volatility", metric(m.annualized_volatility)},
         {"sharpe_zero_risk_free", metric(m.sharpe_zero_risk_free)},
         {"cagr", metric(m.cagr)},
         {"annualization_sessions", std::int64_t{252}},
         {"cagr_year_days", 365.25L},
         {"close_invested_fraction", metric(m.close_invested_fraction)},
         {"mean_close_capital_fraction", metric(m.mean_close_capital_fraction)},
         {"legacy_any_session_exposure", metric(m.legacy_any_session_exposure)},
         {"daily", std::move(daily)},
         {"monthly_returns", periods_json(m.monthly_returns)},
         {"yearly_returns", periods_json(m.yearly_returns)},
         {"rolling_43",
          object({{"window_sessions", std::int64_t{43}},
                  {"count", static_cast<std::uint64_t>(m.rolling_43.windows.size())},
                  {"windows", std::move(windows)},
                  {"median", metric(m.rolling_43.median)},
                  {"worst", metric(m.rolling_43.worst)},
                  {"percentage_positive", metric(m.rolling_43.percentage_positive)},
                  {"percentage_at_least_2pct", metric(m.rolling_43.percentage_at_least_2pct)}})}});
}

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    constexpr std::string_view hex = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 32) {
                out += "\\u00";
                out += hex[c / 16];
                out += hex[c % 16];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + '"';
}
std::string scalar(const Json &node) {
    if (std::holds_alternative<std::nullptr_t>(node.value)) {
        return "null";
    }
    if (const auto *v = std::get_if<bool>(&node.value)) {
        return *v ? "true" : "false";
    }
    if (const auto *v = std::get_if<std::int64_t>(&node.value)) {
        return std::to_string(*v);
    }
    if (const auto *v = std::get_if<std::uint64_t>(&node.value)) {
        return std::to_string(*v);
    }
    if (const auto *v = std::get_if<std::string>(&node.value)) {
        return json_quote(*v);
    }
    const auto value = std::get<long double>(node.value);
    if (!std::isfinite(value)) {
        fail(ErrorCode::NumericRange, "nonfinite reporting statistic");
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<long double>::max_digits10) << value;
    return out.str();
}
void write_json(std::string &out, const Json &node, std::size_t depth = 0) {
    const auto indent = [&](std::size_t level) { out.append(level * 2, ' '); };
    if (const auto *object = std::get_if<Json::Object>(&node.value)) {
        out += '{';
        for (std::size_t i = 0; i < object->size(); ++i) {
            out += '\n';
            indent(depth + 1);
            out += json_quote((*object)[i].first);
            out += ": ";
            write_json(out, (*object)[i].second, depth + 1);
            if (i + 1 < object->size()) {
                out += ',';
            }
        }
        if (!object->empty()) {
            out += '\n';
            indent(depth);
        }
        out += '}';
    } else if (const auto *array = std::get_if<Json::Array>(&node.value)) {
        out += '[';
        for (std::size_t i = 0; i < array->size(); ++i) {
            out += '\n';
            indent(depth + 1);
            write_json(out, (*array)[i], depth + 1);
            if (i + 1 < array->size()) {
                out += ',';
            }
        }
        if (!array->empty()) {
            out += '\n';
            indent(depth);
        }
        out += ']';
    } else {
        out += scalar(node);
    }
}
std::string html_escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
        }
    }
    return out;
}
std::string csv_cell(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    return out + '"';
}
void write_csv(std::string &out, const Json &node, const std::string &path) {
    if (const auto *object = std::get_if<Json::Object>(&node.value); object && !object->empty()) {
        for (const auto &[key, value] : *object) {
            std::string escaped;
            for (char c : key) {
                if (c == '~') {
                    escaped += "~0";
                } else if (c == '/') {
                    escaped += "~1";
                } else {
                    escaped += c;
                }
            }
            write_csv(out, value, path + '/' + escaped);
        }
        return;
    }
    if (const auto *array = std::get_if<Json::Array>(&node.value); array && !array->empty()) {
        for (std::size_t i = 0; i < array->size(); ++i) {
            write_csv(out, (*array)[i], path + '/' + std::to_string(i));
        }
        return;
    }
    std::string type, value;
    if (const auto *s = std::get_if<std::string>(&node.value)) {
        type = "string";
        value = *s;
    } else if (std::holds_alternative<Json::Array>(node.value)) {
        type = "array";
        value = "[]";
    } else if (std::holds_alternative<Json::Object>(node.value)) {
        type = "object";
        value = "{}";
    } else {
        type = std::holds_alternative<std::nullptr_t>(node.value) ? "null"
               : std::holds_alternative<bool>(node.value)         ? "boolean"
                                                                  : "number";
        value = scalar(node);
    }
    out += csv_cell(path) + ',' + csv_cell(type) + ',' + csv_cell(value) + "\r\n";
}

ReportArtifacts render(const ReportMetrics &metrics) {
    const auto tree = payload(metrics);
    ReportArtifacts artifacts;
    write_json(artifacts.json, tree);
    artifacts.json += '\n';
    artifacts.csv = "path,type,value\r\n";
    write_csv(artifacts.csv, tree, "");
    artifacts.html =
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>Backtest "
        "report</title>"
        "</head><body><h1>Backtest report</h1><p>Pure reporting output; run acceptance is owned by "
        "the host.</p>"
        "<p>Money is integer cents in the named currency; units use Quantity6. Ratios are "
        "fractions of one, "
        "except explicitly named percentages. Exact values retain numerator/denominator pairs. "
        "Undefined values are null with reasons. Volatility and Sharpe use sample deviation and "
        "252 sessions, "
        "zero risk-free rate. CAGR uses elapsed days / 365.25 and requires at least 365 days.</p>"
        "<p>Funding is the baseline, not a session. Terminal cash replaces the final reporting "
        "equity; "
        "exposure uses ordinary preterminal closes. Periods and 43-session windows use continuous "
        "account "
        "endpoint ratios, never annual resets. Drawdown durations run from the peak to recovery or "
        "the final close. "
        "Worst overnight gap covers only the supplied observations. Fees and slippage are already "
        "embedded in equity.</p><pre>";
    artifacts.html += html_escape(artifacts.json);
    artifacts.html += "</pre></body></html>\n";
    return artifacts;
}
} // namespace

BacktestResult<ReportMetrics> calculate_report_metrics(const ReportInput &input) noexcept {
    return guarded([&]() -> BacktestResult<ReportMetrics> { return compute(input); });
}
BacktestResult<PreparedReport> prepare_report(const ReportInput &input) noexcept {
    return guarded([&]() -> BacktestResult<PreparedReport> {
        auto metrics = compute(input);
        auto artifacts = render(metrics);
        return PreparedReport{std::move(metrics), std::move(artifacts)};
    });
}
} // namespace quant::backtest
