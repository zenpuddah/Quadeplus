#include "report_evidence.hpp"
#include "application.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/backtest/report.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace quant::backtest::app {
namespace {
namespace lx = ::lexongine;
using detail::take;
constexpr std::uint64_t kib = 1024;
constexpr std::array<std::string_view, 3> extensions{"html", "json", "csv"};
constexpr std::string_view historical_cutoff_date = "2025-12-31";
constexpr std::string_view historical_profile = "continuous_account_through_2025_12_31_v1";

[[noreturn]] void fail(ErrorCode code, std::string_view message) {
    throw detail::Failure{error(code, message, Phase::Finalization)};
}
void check(bool condition, std::string_view message, ErrorCode code = ErrorCode::Integrity) {
    if (!condition) {
        fail(code, message);
    }
}
std::uint64_t plus(std::uint64_t a, std::uint64_t b) {
    check(b <= UINT64_MAX - a, "report evidence bound addition overflow", ErrorCode::NumericRange);
    return a + b;
}
std::uint64_t times(std::uint64_t a, std::uint64_t b) {
    check(!a || b <= UINT64_MAX / a, "report evidence bound product overflow",
          ErrorCode::NumericRange);
    return a * b;
}
lx::Reservation reserve(const std::shared_ptr<lx::ResourceAccount> &account, lx::ResourceKind kind,
                        std::uint64_t bytes) {
    auto result = account->reserve(kind, bytes);
    if (const auto *e = std::get_if<lx::Diagnostic>(&result)) {
        fail(ErrorCode::ResourceExceeded, e->text());
    }
    return std::get<lx::Reservation>(std::move(result));
}
void text(std::string_view value, std::size_t limit, bool empty = false) {
    check((empty || !value.empty()) && value.size() <= limit &&
              std::all_of(value.begin(), value.end(),
                          [](unsigned char c) { return c >= 32 && c <= 126; }),
          "report labels/metadata must be bounded printable ASCII", ErrorCode::InvalidConfig);
}
std::string json_string(std::string_view value) {
    std::string out = "\"";
    constexpr std::string_view hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c / 16];
            out += hex[c % 16];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}
std::string html_text(std::string_view value) {
    std::string out;
    for (char c : value) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else if (c == '"') {
            out += "&quot;";
        } else if (c == '\'') {
            out += "&#39;";
        } else {
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
std::string csv_row(std::string_view path, std::string_view type, std::string_view value) {
    return csv_cell(path) + ',' + csv_cell(type) + ',' + csv_cell(value) + "\r\n";
}
std::string hex(const lx::Sha256 &value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (auto byte : value) {
        const auto n = std::to_integer<unsigned>(byte);
        out += digits[n / 16];
        out += digits[n % 16];
    }
    return out;
}
ReportDate date(std::string_view value) {
    check(value.size() == 10 && value[4] == '-' && value[7] == '-',
          "noncanonical report session date", ErrorCode::InvalidData);
    auto parse = [&](std::size_t begin, std::size_t count) {
        unsigned n = 0;
        const auto part = value.substr(begin, count);
        check(std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }),
              "invalid report date digits", ErrorCode::InvalidData);
        const auto result = std::from_chars(part.data(), part.data() + part.size(), n);
        check(result.ec == std::errc{} && result.ptr == part.data() + part.size(),
              "invalid report date", ErrorCode::InvalidData);
        return n;
    };
    const auto y = parse(0, 4), m = parse(5, 2), d = parse(8, 2);
    ReportDate result{std::chrono::year{static_cast<int>(y)}, std::chrono::month{m},
                      std::chrono::day{d}};
    check(y >= 1 && result.ok(), "invalid report calendar date", ErrorCode::InvalidData);
    return result;
}
data::Money money(const data::CurrencyCode &currency, std::int64_t cents) {
    return data::Money::from_raw(currency, cents);
}

ReportEvidenceSpecs specs(const std::filesystem::path &out, std::span<const std::uint32_t> ids,
                          std::uint64_t rows, std::uint64_t bytes) {
    check(out.is_absolute() && !ids.empty() && ids.size() <= 128 && rows && bytes,
          "report specs require absolute output, 1..128 trials and positive bounds",
          ErrorCode::InvalidConfig);
    text(out.string(), 4096);
    for (const auto &component : out) {
        check(component != "." && component != "..", "report output path is not normalized",
              ErrorCode::InvalidConfig);
    }
    std::set<std::uint32_t> unique;
    ReportEvidenceSpecs result{out, rows, bytes, {ids.begin(), ids.end()}, {}};
    result.required.reserve(ids.size() * 3);
    for (auto id : ids) {
        check(unique.insert(id).second, "duplicate report trial ID", ErrorCode::InvalidConfig);
        for (auto extension : extensions) {
            const auto name = "report-" + std::to_string(id) + "-" + std::string{extension};
            const auto path = out / ("report-" + std::to_string(id) + "." + std::string{extension});
            // CSV is a long-form leaf table; JSON/HTML are one logical document.
            const auto records = extension == "csv" ? plus(8192, times(rows, 256)) : 1;
            result.required.push_back(
                {name, "quadeplus_report_evidence_" + std::string{extension} + "_v1", path.string(),
                 "create_new", bytes, records, "on_close", "stop"});
        }
    }
    // Check all subsequent reservation arithmetic before plan preparation.
    static_cast<void>(plus(times(bytes, 8), plus(times(rows, 4096), 64 * kib)));
    return result;
}

struct HistoricalScope {
    std::optional<std::uint64_t> cutoff_key{};
    std::string not_applicable_reason;
};
HistoricalScope historical_scope(const research::DailyDataset &dataset,
                                 const SuiteConfiguration &suite,
                                 const ReportEvidenceLabels &labels) {
    if (labels.profile != "fresh_research_v1") {
        return {{}, "legacy_profile"};
    }
    const auto rows = dataset.rows();
    if (rows[suite.evaluation_begin].session > historical_cutoff_date) {
        return {{}, "financial_scope_starts_after_cutoff"};
    }
    if (rows[suite.evaluation_end - 1].session < historical_cutoff_date) {
        return {{}, "financial_scope_ends_before_cutoff"};
    }
    for (auto key = suite.evaluation_begin; key < suite.evaluation_end; ++key) {
        if (rows[key].session == historical_cutoff_date) {
            return {key, {}};
        }
    }
    fail(ErrorCode::Coverage,
         "fresh financial scope spans 2025-12-31 but the declared cutoff session is missing");
}

struct Context {
    ReportEvidenceSpecs specification;
    std::shared_ptr<const FinancialRun> financial;
    SuiteConfiguration suite;
    research::DailyDataset dataset;
    ReportEvidenceLabels labels;
    lx::Reservation charge;
    // The final string has left TrialSinkState but still lives in make_sink's
    // synchronous finalizer. Keep its lease until the next trial starts (the
    // previous callback and its bytes are then gone), or Context destruction.
    lx::Reservation deferred_trial_artifacts{};
    std::size_t next_report_artifact = 0;
    HistoricalScope historical{};
};

ValuationProjection close_mark(const Context &c, std::uint64_t key) {
    check(key < c.dataset.rows().size(), "report close key out of dataset range",
          ErrorCode::Coverage);
    return take(
        closing_valuation(take(c.dataset.closing(key, c.dataset.rows()[key].close_available_ns))));
}
ValuationProjection open_mark(const Context &c, std::uint64_t key) {
    check(key < c.dataset.rows().size(), "report opening key out of dataset range",
          ErrorCode::Coverage);
    return take(opening_valuation(
        take(c.dataset.opening(key, c.dataset.rows()[key].bar.interval_start.unix_nanos()))));
}
ValuationProjection prior_mark(const Context &c, std::uint64_t key) {
    check(key > 0 && key < c.dataset.rows().size(), "report prior-close key out of range",
          ErrorCode::Coverage);
    return take(prior_close_valuation(
        take(c.dataset.closing(key - 1, c.dataset.rows()[key - 1].close_available_ns)),
        take(c.dataset.opening(key, c.dataset.rows()[key].bar.interval_start.unix_nanos()))));
}
void balances(const HostEvent &event, const Account &account) {
    check(event.trial == account.trial && event.cash == account.cash.amount() &&
              event.units == account.units.raw() && event.basis == account.basis.amount() &&
              event.realized == account.realized.amount() &&
              event.dividends == account.dividends.amount() && event.fees == account.fees.amount(),
          "host event balances differ from the corresponding ledger snapshot");
}
EquityPoint marked(const HostEvent &event, const Account &account, const ValuationProjection &mark,
                   bool fill = false) {
    balances(event, account);
    check(event.key == mark.key() && event.phase == mark.phase() && event.equity.has_value(),
          "host event valuation key/phase missing or inconsistent");
    if (!fill) {
        check(event.price == mark.price().raw(), "host event mark differs from dataset price");
    }
    auto point = take(mark_account(account, mark, mark.phase()));
    check(*event.equity == point.equity.amount(), "host equity differs from exact dataset mark");
    return point;
}
void fill_fields(const HostEvent &event, const Fill &fill) {
    check(event.id == fill.id && event.key == fill.mark.key() &&
              event.price == fill.executed_price.raw() && event.quantity == fill.quantity.raw() &&
              event.gross == fill.gross.amount() && event.fee == fill.fee.amount(),
          "host fill evidence differs from ledger fill");
    check(event.reason == fill.reason, "host fill reason differs from ledger fill");
    if (fill.intent) {
        check(event.decision_key == fill.intent->decision_key &&
                  event.eligible_key == fill.intent->earliest_open_key,
              "fill decision/eligibility evidence mismatch");
        std::string_view policy = "sell_all";
        std::int64_t budget = 0;
        if (const auto *b = std::get_if<BuyCashBudget>(&fill.intent->order)) {
            policy = "cash_budget";
            budget = b->budget.amount();
        } else if (std::holds_alternative<BuyFixedQuantity>(fill.intent->order)) {
            policy = "fixed_quantity";
        }
        check(event.quantity_policy == policy && event.budget == budget,
              "fill quantity policy evidence mismatch");
    } else {
        check(!event.decision_key && !event.eligible_key && event.quantity_policy == "none" &&
                  event.budget == 0,
              "terminal fill fabricated opening intent evidence");
    }
}
std::string rejection_name(std::string_view raw) {
    constexpr std::array<std::string_view, 8> names{
        "BelowMinimumLot", "InsufficientCash", "AllocationCap", "PositionAlreadyOpen",
        "NoPosition",      "CashChanged",      "NotEligible",   "NonpositiveProtectiveThreshold"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (raw == std::to_string(i)) {
            return std::string{names[i]};
        }
    }
    fail(ErrorCode::Unsupported, "unknown host rejection reason encoding");
}

struct HistoricalSnapshot {
    std::size_t close_count, trade_count;
    ReportAccountTotals totals;
};
struct Assembled {
    std::vector<ReportClosePoint> closes;
    std::vector<data::Money> trades;
    ReportAccountTotals totals;
    std::optional<data::Money> terminal;
    std::optional<HistoricalSnapshot> historical{};
};

Assembled assemble(const Context &c, std::size_t index) {
    const auto &config = c.suite.trials[index];
    const auto &trial = c.financial->trials[index];
    check(trial.account && trial.ledger && !trial.pending,
          "completed trial lacks account/ledger or retains pending intent");
    const auto &ledger = *trial.ledger;
    check(ledger.rows().size() <= c.suite.limits.max_ledger_events,
          "trial ledger exceeds admitted report scope", ErrorCode::ResourceExceeded);
    const auto &initial = ledger.initial();
    detail::account(initial);
    detail::account(*trial.account);
    const auto baseline = close_mark(c, c.suite.funding_key);
    check(initial.trial == config.id && initial.source == baseline.source() &&
              initial.initial_cash == config.initial_cash && initial.cash == config.initial_cash &&
              initial.lot_step == config.lot_step && !initial.revision && !initial.cash_revision &&
              !initial.last_event && !initial.units.raw() && !initial.basis.amount() &&
              !initial.realized.amount() && !initial.dividends.amount() && !initial.fees.amount(),
          "ledger funding identity/configuration mismatch");
    const auto &currency = initial.source.currency;
    auto zero = money(currency, 0);
    Assembled result{
        {},
        {},
        {trial.account->cash, zero, trial.account->dividends, trial.account->fees, zero, zero},
        {}};
    result.closes.reserve(
        static_cast<std::size_t>(c.suite.evaluation_end - c.suite.evaluation_begin));
    const Account *current = &initial;
    std::size_t cursor = 0;
    std::uint64_t next_close = c.suite.evaluation_begin, previous_key = c.suite.funding_key;
    std::uint64_t dividends_seen = 0, terminals = 0;
    bool funded = false, funding_close = false, finalized = false;
    bool session_dividend = false, any_exposure = false;
    std::optional<std::uint64_t> pending, accepted;
    const HostEvent *pending_event = nullptr;
    std::set<std::uint64_t> order_ids;
    std::map<std::string, std::uint64_t> rejected;
    auto fees = zero, dividends = zero, gross = zero, slippage = zero, realized = zero;

    auto next_ledger = [&]() -> const LedgerRow & {
        check(cursor < ledger.rows().size(), "host posting has no matching ledger row");
        const auto &row = ledger.rows()[cursor];
        check(row.sequence == cursor + 1 && row.after.revision == current->revision + 1,
              "ledger revision/sequence mismatch");
        return row;
    };
    auto commit_snapshot = [&](const LedgerRow &row, Account expected) {
        expected.revision = detail::next(current->revision);
        expected.cash_revision = current->cash_revision + (expected.cash != current->cash ? 1 : 0);
        detail::account(expected);
        check(row.after == expected,
              "ledger posting differs from reconstructed exact account transition");
        current = &row.after;
        ++cursor;
    };
    auto consume_fill = [&](const HostEvent &event, bool terminal) {
        const auto &row = next_ledger();
        const auto *fill = std::get_if<Fill>(&row.event);
        check(fill != nullptr, "fill event does not match ledger kind");
        const auto mark = terminal ? close_mark(c, event.key) : open_mark(c, event.key);
        check(fill->mark == mark && fill->synthetic_terminal == terminal &&
                  fill->policy == config.execution && fill->intent.has_value() != terminal,
              "fill mark/policy/source mismatch");
        take(validate_fill(*fill, *current));
        fill_fields(event, *fill);
        auto expected = *current;
        if (fill->side == data::Side::Buy) {
            check(!terminal && !current->units.raw(), "terminal buy or pyramiding in report input");
            expected.units = fill->quantity;
            expected.basis = fill->gross + fill->fee;
            expected.cash = current->cash - expected.basis;
            any_exposure = true;
        } else {
            check(fill->side == data::Side::Sell && fill->quantity == current->units &&
                      current->units.raw() > 0,
                  "closed trade must be a full sale");
            const auto proceeds = fill->gross - fill->fee;
            const auto pnl = row.after.realized - current->realized;
            check(pnl == proceeds - current->basis,
                  "full-sale realized delta differs from settled trading P&L");
            expected.cash = current->cash + proceeds;
            expected.realized = current->realized + pnl;
            expected.units = data::Quantity::from_raw(0);
            expected.basis = zero;
            result.trades.push_back(pnl);
            realized = realized + pnl;
        }
        expected.fees = current->fees + fill->fee;
        expected.last_event = Account::EventLocation{event.key, mark.phase()};
        const auto unshifted = take(settle_gross(mark.price(), fill->quantity, currency));
        const auto impact =
            fill->side == data::Side::Buy ? fill->gross - unshifted : unshifted - fill->gross;
        check(impact.amount() >= 0, "adverse slippage diagnostic became negative");
        gross = gross + fill->gross;
        fees = fees + fill->fee;
        slippage = slippage + impact;
        commit_snapshot(row, std::move(expected));
        marked(event, *current, mark, true);
    };

    for (const auto &event : c.financial->events) {
        if (event.trial != config.id) {
            continue;
        }
        check(!finalized && event.key >= previous_key && event.key >= c.suite.funding_key &&
                  event.key < c.suite.evaluation_end,
              "host event regresses/outside financial scope or follows final endpoint",
              ErrorCode::Coverage);
        previous_key = event.key;
        if (event.kind == "submitted" || event.kind == "rejected" || event.kind == "expired" ||
            event.kind == "suppressed") {
            check(!event.equity && !event.phase && !event.price && !event.gross && !event.fee &&
                      event.quantity >= 0 && event.budget >= 0,
                  "unmarked order event contains fabricated valuation/settlement fields");
        } else if (event.kind == "funded" || event.kind == "dividend" || event.kind == "close" ||
                   event.kind == "risk" || event.kind == "final") {
            check(!event.id && !event.quantity && !event.gross && !event.fee &&
                      !event.decision_key && !event.eligible_key &&
                      event.quantity_policy == "none" && !event.budget,
                  "nonfill valuation contains fabricated order/settlement fields");
        }
        if (event.kind == "funded") {
            check(!funded && event.key == c.suite.funding_key && cursor == 0,
                  "duplicate or misplaced funding event");
            check(marked(event, initial, baseline).equity == config.initial_cash,
                  "funding baseline equity mismatch");
            funded = true;
            continue;
        }
        check(funded, "financial event precedes funding");
        if (event.kind == "dividend") {
            check(funding_close && event.key == next_close && !session_dividend && !accepted,
                  "missing/duplicate/out-of-order session dividend", ErrorCode::Coverage);
            const auto &row = next_ledger();
            const auto *dividend = std::get_if<DividendEvent>(&row.event);
            const auto prior = prior_mark(c, event.key);
            check(dividend && dividend->trial == config.id && dividend->prior_close == prior &&
                      dividend->entitled_units == current->units &&
                      dividend->per_unit == c.dataset.rows()[event.key].dividend,
                  "dividend entitlement/source differs from prior holdings/dataset");
            auto expected = *current;
            const auto payment =
                take(settle_dividend(dividend->per_unit, dividend->entitled_units));
            expected.cash = current->cash + payment;
            expected.dividends = current->dividends + payment;
            expected.last_event = Account::EventLocation{event.key, ValuationPhase::PriorClose};
            any_exposure = current->units.is_positive();
            dividends = dividends + payment;
            commit_snapshot(row, std::move(expected));
            marked(event, *current, prior);
            session_dividend = true;
            ++dividends_seen;
        } else if (event.kind == "accepted") {
            check(session_dividend && event.key == next_close && pending == event.id && !accepted,
                  "accepted order has no pending opening intent");
            const auto &row = next_ledger();
            const auto *fill = std::get_if<Fill>(&row.event);
            check(fill && !fill->synthetic_terminal, "accepted order has no next ledger fill");
            check(pending_event && fill->intent &&
                      fill->intent->decision_key == pending_event->key &&
                      fill->intent->earliest_open_key == pending_event->eligible_key &&
                      fill->reason == pending_event->reason &&
                      event.quantity_policy == pending_event->quantity_policy &&
                      event.budget == pending_event->budget &&
                      (event.quantity_policy != "fixed_quantity" ||
                       event.quantity == pending_event->quantity),
                  "accepted fill differs from the submitted intent trace");
            fill_fields(event, *fill);
            marked(event, *current, open_mark(c, event.key), true);
            accepted = event.id;
        } else if (event.kind == "filled") {
            check(session_dividend && event.key == next_close && pending == event.id &&
                      accepted == event.id,
                  "filled order lacks accepted opening intent");
            consume_fill(event, false);
            pending.reset();
            accepted.reset();
            pending_event = nullptr;
        } else if (event.kind == "terminal") {
            check(config.terminal == TerminalMode::SyntheticClose &&
                      event.key == c.suite.evaluation_end - 1 &&
                      next_close == c.suite.evaluation_end && !pending && !accepted && !terminals &&
                      order_ids.insert(event.id).second,
                  "unexpected/duplicate terminal liquidation");
            consume_fill(event, true);
            ++terminals;
        } else if (event.kind == "close") {
            check(event.reason == "ordinary" && !accepted,
                  "nonordinary close or uncompleted accepted order");
            const auto point = marked(event, *current, close_mark(c, event.key));
            if (event.key == c.suite.funding_key) {
                check(!funding_close && cursor == 0, "duplicate/nonflat funding close");
                funding_close = true;
            } else {
                check(funding_close && event.key == next_close && session_dividend,
                      "completed run has missing/duplicate ordinary close", ErrorCode::Coverage);
                const auto &row = c.dataset.rows()[event.key];
                result.closes.push_back(
                    {date(row.session), point.equity, current->units, point.position_value,
                     any_exposure,
                     ReportOvernightPrices{c.dataset.rows()[event.key - 1].bar.close,
                                           row.bar.open}});
                if (c.historical.cutoff_key == event.key) {
                    check(!result.historical && current->realized == realized &&
                              current->fees == fees && current->dividends == dividends,
                          "historical cutoff account/trade/cost snapshot is inconsistent");
                    ReportAccountTotals prefix{current->cash,
                                               point.unrealized,
                                               current->dividends,
                                               current->fees,
                                               slippage,
                                               gross,
                                               result.totals.rejected_orders,
                                               result.totals.expired_orders,
                                               {}};
                    for (const auto &[reason, count] : rejected) {
                        prefix.rejection_reasons.push_back({reason, count});
                    }
                    result.historical = HistoricalSnapshot{result.closes.size(),
                                                           result.trades.size(), std::move(prefix)};
                }
                ++next_close;
                session_dividend = false;
            }
        } else if (event.kind == "submitted") {
            balances(event, *current);
            const bool funding_intent =
                c.suite.legacy_funding_close && event.key == c.suite.funding_key && funding_close;
            check((funding_intent ||
                   (event.key >= c.suite.evaluation_begin && event.key + 1 == next_close)) &&
                      !pending && !accepted && event.id && order_ids.insert(event.id).second &&
                      event.decision_key == event.key && event.eligible_key == event.key + 1,
                  "submitted order timing/identity mismatch");
            if (event.quantity_policy == "cash_budget") {
                check(event.budget == take(freeze_cash_budget(*current)).budget.amount() &&
                          !event.quantity,
                      "submitted cash budget differs from exact frozen policy");
            } else if (event.quantity_policy == "fixed_quantity") {
                check(!event.budget, "fixed-quantity submission carries a cash budget");
            } else {
                check(event.quantity_policy == "sell_all" && !event.budget && !event.quantity,
                      "unknown submitted quantity policy");
            }
            pending = event.id;
            pending_event = &event;
        } else if (event.kind == "rejected") {
            balances(event, *current);
            check(session_dividend && event.key == next_close && pending == event.id && !accepted,
                  "rejection has no pending opening order");
            check(pending_event && event.quantity_policy == pending_event->quantity_policy &&
                      event.budget == pending_event->budget &&
                      event.quantity == pending_event->quantity &&
                      event.decision_key == pending_event->decision_key &&
                      event.eligible_key == pending_event->eligible_key,
                  "rejection differs from submitted intent trace");
            ++rejected[rejection_name(event.reason)];
            ++result.totals.rejected_orders;
            pending.reset();
            pending_event = nullptr;
        } else if (event.kind == "expired") {
            balances(event, *current);
            check(event.key == c.suite.evaluation_end - 1 && next_close == c.suite.evaluation_end &&
                      pending == event.id && !accepted && event.reason == "end_of_scope",
                  "expiration has no final pending order");
            check(pending_event && event.quantity_policy == pending_event->quantity_policy &&
                      event.budget == pending_event->budget &&
                      event.quantity == pending_event->quantity &&
                      event.decision_key == pending_event->decision_key &&
                      event.eligible_key == pending_event->eligible_key,
                  "expiration differs from submitted intent trace");
            ++result.totals.expired_orders;
            pending.reset();
            pending_event = nullptr;
        } else if (event.kind == "risk") {
            check(event.key >= c.suite.evaluation_begin && event.key + 1 == next_close,
                  "risk event not after ordinary close");
            marked(event, *current, close_mark(c, event.key));
        } else if (event.kind == "suppressed") {
            check(event.key >= c.suite.evaluation_begin && event.key + 1 == next_close &&
                      event.reason == "risk_latched",
                  "suppression not after ordinary close");
            balances(event, *current);
        } else if (event.kind == "final") {
            check(next_close == c.suite.evaluation_end && event.key == c.suite.evaluation_end - 1 &&
                      !pending && !accepted && cursor == ledger.rows().size(),
                  "final event precedes complete scope/ledger", ErrorCode::Coverage);
            const auto endpoint = marked(event, *current, close_mark(c, event.key));
            result.totals.unrealized_pnl = endpoint.unrealized;
            finalized = true;
        } else {
            fail(ErrorCode::Unsupported, "unknown host event kind in required report");
        }
    }
    check(funded && funding_close && finalized && next_close == c.suite.evaluation_end &&
              result.closes.size() == c.suite.evaluation_end - c.suite.evaluation_begin &&
              dividends_seen == result.closes.size() && cursor == ledger.rows().size(),
          "completed report lacks exact funding/evaluation/endpoint coverage", ErrorCode::Coverage);
    check(*current == *trial.account && current->fees == fees && current->dividends == dividends &&
              current->realized == realized,
          "final account, trade P&L, fee or dividend aggregate does not reconcile");
    check(result.historical.has_value() == c.historical.cutoff_key.has_value(),
          "validated run lacks its required historical cutoff", ErrorCode::Coverage);
    if (config.terminal == TerminalMode::SyntheticClose) {
        check(!current->units.raw() &&
                  terminals == (result.closes.back().invested_units.is_positive() ? 1U : 0U),
              "synthetic endpoint omitted or fabricated liquidation");
        result.terminal = current->cash;
    } else {
        check(!terminals, "marked endpoint contains synthetic liquidation");
    }
    result.totals.cumulative_gross = gross;
    result.totals.slippage_estimate = slippage;
    for (const auto &[reason, count] : rejected) {
        result.totals.rejection_reasons.push_back({reason, count});
    }
    return result;
}

struct RiskProjection {
    std::string json, outcome;
};
RiskProjection project_risk(const Context &c, std::size_t index) {
    const auto &trial = c.suite.trials[index];
    const bool complete = c.financial->finalized && c.financial->exhausted && !c.financial->failure;
    const HostEvent *trigger = nullptr, *expected_trigger = nullptr, *last_close = nullptr,
                    *last = nullptr, *synthetic = nullptr;
    RiskReason expected_reason = RiskReason::None;
    std::uint32_t threshold = 0;
    auto high = trial.initial_cash, previous = trial.initial_cash, baseline = trial.initial_cash;
    struct Order {
        const HostEvent *queued;
        const HostEvent *resolution = nullptr;
        const HostEvent *fill = nullptr;
        std::string status = "queued";
    };
    std::vector<Order> orders;
    const auto &currency = trial.initial_cash.currency();
    const auto same_snapshot = [](const HostEvent &a, const HostEvent &b) {
        return a.key == b.key && a.cash == b.cash && a.units == b.units && a.basis == b.basis &&
               a.realized == b.realized && a.dividends == b.dividends && a.fees == b.fees &&
               a.equity == b.equity && a.phase == b.phase && a.price == b.price;
    };
    const auto checked_sale = [&](const HostEvent &event, bool terminal) {
        check(trigger && event.quantity == trigger->units && event.quantity > 0 && event.units == 0,
              "risk sale does not close the trigger-held quantity");
        const auto price =
            terminal ? c.dataset.rows()[event.key].bar.close : c.dataset.rows()[event.key].bar.open;
        const auto executed = take(execution_price(price, data::Side::Sell, trial.execution));
        const auto quantity = data::Quantity::from_raw(event.quantity);
        check(event.phase == (terminal ? ValuationPhase::Close : ValuationPhase::Open) &&
                  event.price == executed.raw() &&
                  event.gross == take(settle_gross(executed, quantity, currency)).amount() &&
                  event.fee ==
                      take(settle_fee(executed, quantity, currency, trial.execution)).amount(),
              "risk sale settlement/phase does not match dataset and costs");
    };
    for (const auto &event : c.financial->events) {
        if (event.trial != trial.id) {
            continue;
        }
        check(event.key >= c.suite.funding_key && event.key < c.suite.evaluation_end &&
                  (!last || event.key >= last->key) && event.units >= 0,
              "risk projection has invalid scope/succession/units", ErrorCode::Coverage);
        last = &event;
        if (event.kind == "close" && event.key >= c.suite.evaluation_begin) {
            check(event.equity && event.cash >= 0 && *event.equity >= 0 &&
                      event.phase == ValuationPhase::Close &&
                      event.price == c.dataset.rows()[event.key].bar.close.raw() &&
                      *event.equity ==
                          detail::add(
                              event.cash,
                              take(settle_gross(data::Price::from_raw(event.price),
                                                data::Quantity::from_raw(event.units), currency))
                                  .amount()),
                  "risk projection ordinary close is inconsistent");
            const auto equity = money(currency, *event.equity);
            if (equity.amount() > high.amount()) {
                high = equity;
            }
            if (!expected_trigger) {
                if (trial.risk.drawdown_bps &&
                    take(loss_at_least(high, equity, *trial.risk.drawdown_bps))) {
                    expected_trigger = &event;
                    expected_reason = RiskReason::Drawdown;
                    threshold = *trial.risk.drawdown_bps;
                    baseline = high;
                } else if (trial.risk.daily_loss_bps &&
                           take(loss_at_least(previous, equity, *trial.risk.daily_loss_bps))) {
                    expected_trigger = &event;
                    expected_reason = RiskReason::DailyLoss;
                    threshold = *trial.risk.daily_loss_bps;
                    baseline = previous;
                }
            }
            previous = equity;
            last_close = &event;
        } else if (event.kind == "risk") {
            check(!trigger && expected_trigger && last_close && same_snapshot(event, *last_close) &&
                      event.key == expected_trigger->key &&
                      event.reason == std::to_string(static_cast<int>(expected_reason)),
                  "latched risk trigger missing, duplicated or inconsistent with ordinary-close "
                  "threshold");
            trigger = &event;
        } else if (event.kind == "submitted" && event.reason == "risk_exit") {
            check(trigger && trigger->units > 0 && event.units == trigger->units &&
                      event.key >= trigger->key && event.id > 0 &&
                      event.quantity_policy == "sell_all" && event.decision_key == event.key &&
                      event.eligible_key == event.key + 1 && last_close &&
                      last_close->key == event.key &&
                      (orders.empty() || orders.back().status == "rejected"),
                  "risk exit submission has no matching latched trigger");
            check(std::none_of(orders.begin(), orders.end(),
                               [&](const auto &o) { return o.queued->id == event.id; }),
                  "duplicate risk order ID");
            orders.push_back({&event});
        } else if (event.kind == "terminal" && trigger) {
            check(!synthetic && trial.terminal == TerminalMode::SyntheticClose &&
                      event.key == c.suite.evaluation_end - 1 && !event.decision_key &&
                      !event.eligible_key && event.quantity_policy == "none" &&
                      (orders.empty() || orders.back().status == "expired"),
                  "synthetic endpoint incorrectly presented as executable risk exit");
            checked_sale(event, true);
            synthetic = &event;
        } else if (trigger && (event.kind == "accepted" || event.kind == "filled" ||
                               event.kind == "expired" || event.kind == "rejected")) {
            auto order = std::find_if(orders.begin(), orders.end(),
                                      [&](const auto &o) { return o.queued->id == event.id; });
            check(order != orders.end(), "post-trigger lifecycle event has no risk order");
            check(event.decision_key == order->queued->decision_key &&
                      event.eligible_key == order->queued->eligible_key &&
                      event.quantity_policy == "sell_all",
                  "risk order eligibility/policy drift");
            if (event.kind == "accepted") {
                check(order->status == "queued" && event.key >= *event.eligible_key &&
                          event.reason == "risk_exit",
                      "risk acceptance timing/state mismatch");
                order->status = "accepted";
            } else {
                check(!order->resolution, "risk order resolved more than once");
                if (event.kind == "filled") {
                    check(order->status == "accepted" && event.key >= *event.eligible_key &&
                              event.reason == "risk_exit",
                          "risk fill lacks eligible opening acceptance");
                    checked_sale(event, false);
                    order->fill = &event;
                    order->status = "filled";
                } else if (event.kind == "expired") {
                    check(order->status == "queued" && event.key == c.suite.evaluation_end - 1 &&
                              event.reason == "end_of_scope",
                          "risk expiry is not an unexecuted final intent");
                    order->status = "expired";
                } else {
                    check(order->status == "queued" && event.key >= *event.eligible_key,
                          "risk rejection timing/state mismatch");
                    static_cast<void>(rejection_name(event.reason));
                    order->status = "rejected";
                }
                order->resolution = &event;
            }
        } else if (trigger && event.kind == "submitted") {
            fail(ErrorCode::Integrity, "non-risk order submitted after the risk latch");
        }
    }
    if (complete) {
        check((expected_trigger != nullptr) == (trigger != nullptr),
              "completed report omitted the first required risk trigger");
    }
    if (!trigger) {
        return {"[]", complete ? "No latched risk trigger occurred."
                               : "Partial trace: no latched risk trigger recorded."};
    }
    check(last != nullptr, "risk trigger has no observed endpoint");
    const bool filled =
        std::any_of(orders.begin(), orders.end(), [](const auto &o) { return o.fill != nullptr; });
    const bool pending =
        !orders.empty() && (orders.back().status == "queued" || orders.back().status == "accepted");
    if (complete) {
        check(last->kind == "final" && !pending && (trigger->units == 0 || !orders.empty()),
              "completed risk outcome is unresolved");
        check(!(filled || trigger->units == 0) || (!last->units && !synthetic),
              "risk latch acquired holdings after exit/flat trigger");
    }
    const std::string outcome =
        trigger->units == 0 ? "flat_trigger_no_order"
        : filled            ? "risk_exit_filled"
        : orders.empty()    ? "trigger_recorded_order_not_recorded"
        : pending           ? "risk_exit_pending"
        : orders.back().status == "expired"
            ? (synthetic ? "risk_exit_expired_synthetic_endpoint" : "risk_exit_expired_still_held")
            : "risk_exit_rejected";
    const std::map<std::string, std::string> explanations{
        {"flat_trigger_no_order", "Risk latched while flat; no liquidation order was required."},
        {"risk_exit_filled", "Risk exit filled at an eligible opening; gap and execution drag are "
                             "diagnostics already included in equity."},
        {"trigger_recorded_order_not_recorded",
         "Risk triggered while held, but no queued risk order is recorded in this partial trace."},
        {"risk_exit_pending", "Risk exit is queued or accepted without a recorded fill in this "
                              "partial trace; no execution is inferred."},
        {"risk_exit_expired_synthetic_endpoint",
         "Risk order expired unexecuted; a separate synthetic endpoint sale flattened holdings and "
         "is not an executable risk exit."},
        {"risk_exit_expired_still_held",
         "Risk order expired unexecuted; holdings remain at the observed endpoint."},
        {"risk_exit_rejected", "Risk exit was rejected; no fill or gap execution is inferred."}};
    const auto held = data::Quantity::from_raw(trigger->units);
    const auto trigger_value =
        take(settle_gross(c.dataset.rows()[trigger->key].bar.close, held, currency)).amount();
    const auto location = [&](const HostEvent &e) {
        return "\"date\":" + json_string(c.dataset.rows()[e.key].session) +
               ",\"key\":" + std::to_string(e.key);
    };
    std::string json =
        "[{\"trigger\":{" + location(*trigger) + ",\"reason\":" +
        json_string(expected_reason == RiskReason::Drawdown ? "Drawdown" : "DailyLoss") +
        ",\"threshold_bps\":" + std::to_string(threshold) +
        ",\"baseline_equity_cents\":" + std::to_string(baseline.amount()) +
        ",\"equity_cents\":" + std::to_string(*trigger->equity) +
        ",\"close_price_raw\":" + std::to_string(trigger->price) +
        ",\"held_units_raw\":" + std::to_string(trigger->units) +
        ",\"held_close_value_cents\":" + std::to_string(trigger_value) + "},\"orders\":[";
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const auto &o = orders[i];
        const auto &q = *o.queued;
        if (i) {
            json += ',';
        }
        json += "{\"id\":" + std::to_string(q.id) +
                ",\"queued_date\":" + json_string(c.dataset.rows()[q.key].session) +
                ",\"queued_key\":" + std::to_string(q.key) +
                ",\"eligible_key\":" + std::to_string(*q.eligible_key) + ",\"eligible_date\":" +
                (*q.eligible_key < c.suite.evaluation_end
                     ? json_string(c.dataset.rows()[*q.eligible_key].session)
                     : "null") +
                ",\"status\":" + json_string(o.status) + ",\"resolution\":";
        json += o.resolution ? "{" + location(*o.resolution) +
                                   ",\"kind\":" + json_string(o.resolution->kind) +
                                   ",\"reason\":" + json_string(o.resolution->reason) + "}"
                             : "null";
        json += ",\"fill\":";
        if (o.fill) {
            const auto &f = *o.fill;
            const auto raw = c.dataset.rows()[f.key].bar.open;
            const auto raw_value = take(settle_gross(raw, held, currency)).amount();
            const auto gap = detail::sub(raw_value, trigger_value),
                       slippage = detail::sub(raw_value, f.gross),
                       drag = detail::add(slippage, f.fee);
            check(slippage >= 0 && f.fee >= 0, "negative risk execution drag");
            json +=
                "{" + location(f) + ",\"quantity_raw\":" + std::to_string(f.quantity) +
                ",\"raw_open_price_raw\":" + std::to_string(raw.raw()) +
                ",\"executed_price_raw\":" + std::to_string(f.price) +
                ",\"raw_open_value_cents\":" + std::to_string(raw_value) +
                ",\"executed_gross_cents\":" + std::to_string(f.gross) +
                ",\"fee_cents\":" + std::to_string(f.fee) +
                ",\"raw_open_gap_pnl_cents\":" + std::to_string(gap) +
                ",\"raw_open_gap_loss_cents\":" + std::to_string(std::max<std::int64_t>(0, -gap)) +
                ",\"slippage_drag_cents\":" + std::to_string(slippage) +
                ",\"execution_drag_cents\":" + std::to_string(drag) +
                ",\"net_exit_vs_trigger_close_cents\":" + std::to_string(detail::sub(gap, drag)) +
                "}";
        } else {
            json += "null";
        }
        json += '}';
    }
    json += "],\"synthetic_terminal\":";
    json += synthetic ? "{" + location(*synthetic) + ",\"id\":" + std::to_string(synthetic->id) +
                            ",\"reason\":" + json_string(synthetic->reason) +
                            ",\"quantity_raw\":" + std::to_string(synthetic->quantity) +
                            ",\"executed_price_raw\":" + std::to_string(synthetic->price) +
                            ",\"gross_cents\":" + std::to_string(synthetic->gross) +
                            ",\"fee_cents\":" + std::to_string(synthetic->fee) + "}"
                      : "null";
    json +=
        ",\"endpoint\":{\"financial_state\":" + json_string(complete ? "completed" : "partial") +
        ",\"observed_date\":" + json_string(c.dataset.rows()[last->key].session) +
        ",\"observed_key\":" + std::to_string(last->key) +
        ",\"observed_units_raw\":" + std::to_string(last->units) +
        ",\"still_held\":" + (last->units ? "true" : "false") +
        ",\"risk_exit_executed\":" + (filled ? "true" : "false") +
        ",\"pending_order_id\":" + (pending ? std::to_string(orders.back().queued->id) : "null") +
        "},\"outcome\":" + json_string(outcome) + "}]";
    return {std::move(json), (complete ? "" : "Partial trace: ") + explanations.at(outcome)};
}

using Metadata = std::vector<std::pair<std::string, std::string>>;
Metadata metadata(const Context &c, std::size_t index) {
    const auto &t = c.suite.trials[index];
    const auto &d = c.dataset.specification();
    check(d.adjustment == research::adjustment_profile,
          "unsupported report dataset adjustment policy", ErrorCode::Unsupported);
    const auto risk = project_risk(c, index);
    Metadata values{
        {"run_label", c.labels.run_label},
        {"profile", c.labels.profile},
        {"window_status", c.labels.window_status},
        {"provenance", c.labels.provenance},
        {"trial_id", std::to_string(t.id)},
        {"strategy_id", t.strategy_id},
        {"strategy_configuration", t.strategy_configuration},
        {"experiment_sha256", hex(c.financial->experiment)},
        {"dataset_schema", std::string{research::daily_format}},
        {"dataset_id", d.version},
        {"dataset_provenance", d.provenance},
        {"adjustment_policy", d.adjustment},
        {"adjustment_interpretation",
         "Split-adjusted OHLC; separate dividends on the same share basis; AdjClose is retained "
         "but not traded; no additional split postings."},
        {"dataset_manifest_sha256", std::string{c.dataset.manifest_sha256()}},
        {"prices_sha256", std::string{c.dataset.prices_sha256()}},
        {"sessions_sha256", std::string{c.dataset.sessions_sha256()}},
        {"symbol", d.symbol},
        {"currency", d.currency},
        {"instrument_id", std::to_string(d.instrument)},
        {"venue_id", std::to_string(d.venue)},
        {"source_id", std::to_string(d.source)},
        {"numerical_origin", d.numerical_origin},
        {"numerical_end", c.dataset.rows().back().session},
        {"numerical_source_rows", std::to_string(c.dataset.rows().size())},
        {"manifest_permitted_evaluation_begin", d.evaluation_begin},
        {"manifest_permitted_evaluation_end", d.evaluation_end},
        {"funding_date", c.dataset.rows()[c.suite.funding_key].session},
        {"funding_key", std::to_string(c.suite.funding_key)},
        {"evaluation_begin", c.dataset.rows()[c.suite.evaluation_begin].session},
        {"evaluation_end", c.dataset.rows()[c.suite.evaluation_end - 1].session},
        {"evaluation_begin_key", std::to_string(c.suite.evaluation_begin)},
        {"evaluation_end_key_exclusive", std::to_string(c.suite.evaluation_end)},
        {"declared_evaluation_sessions",
         std::to_string(c.suite.evaluation_end - c.suite.evaluation_begin)},
        {"execution_profile", std::string{execution_profile}},
        {"dividend_profile", std::string{dividend_profile}},
        {"commission_bps", std::to_string(t.execution.commission_bps)},
        {"slippage_bps", std::to_string(t.execution.slippage_bps)},
        {"terminal_policy", t.terminal == TerminalMode::Mark ? "mark" : "synthetic_close"},
        {"initial_cash_cents", std::to_string(t.initial_cash.amount())},
        {"lot_step_raw", std::to_string(t.lot_step.raw())},
        {"drawdown_breaker_bps",
         t.risk.drawdown_bps ? std::to_string(*t.risk.drawdown_bps) : "disabled"},
        {"daily_loss_breaker_bps",
         t.risk.daily_loss_bps ? std::to_string(*t.risk.daily_loss_bps) : "disabled"},
        {"risk_report_profile", "trigger_order_gap_v1"},
        {"risk_events_json", risk.json},
        {"risk_outcome", risk.outcome},
        {"risk_gap_interpretation",
         "Difference of separately ties-even settled holding values: actual raw open minus trigger "
         "close, using trigger-held units; loss=max(0,-gap PnL); execution drag=raw open "
         "value-executed gross+fee; dividends excluded; diagnostics already included in equity, "
         "never subtracted again."},
        {"slippage_diagnostic", "settled_gross_difference_v1;buy_executed_minus_raw;sell_raw_minus_"
                                "executed;settle_each_gross_ties_even;already_embedded_in_equity"},
        {"control_timing", c.suite.legacy_funding_close
                               ? "funding_close_intent_may_fill_first_evaluation_open"
                               : "first_evaluation_close_intent_may_fill_next_open"},
        {"acceptance_authority",
         "engine_required_evidence_and_existing_marker;report_is_not_acceptance"}};
    for (const auto &[name, value] : values) {
        static_cast<void>(name);
        text(value, 8192);
    }
    return values;
}
std::string metadata_json(const Metadata &values) {
    std::string out = "{";
    bool first = true;
    for (const auto &[name, value] : values) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += json_string(name) + ':' + json_string(value);
    }
    return out + '}';
}
std::string metadata_csv(const Metadata &values) {
    std::string out;
    for (const auto &[name, value] : values) {
        out += csv_row("/metadata/" + name, "string", value);
    }
    return out;
}

// Preserve RFC4180 fields, including quoted embedded newlines. Only the first
// field (the native renderer's JSON-pointer path) gets the /metrics prefix.
std::string prefixed_csv(std::string_view csv) {
    constexpr std::string_view header = "path,type,value\r\n";
    check(csv.starts_with(header), "unexpected native report CSV header");
    csv.remove_prefix(header.size());
    std::string out;
    bool quoted = false, start = true;
    for (std::size_t i = 0; i < csv.size(); ++i) {
        const auto c = csv[i];
        if (start) {
            check(c == '"', "unexpected native CSV path encoding");
            out += "\"/metrics";
            quoted = true;
            start = false;
            continue;
        }
        out += c;
        if (c == '"') {
            if (quoted && i + 1 < csv.size() && csv[i + 1] == '"') {
                out += csv[++i];
            } else {
                quoted = !quoted;
            }
        } else if (c == '\n' && !quoted) {
            start = true;
        }
    }
    check(!quoted && start, "unterminated native report CSV row");
    return out;
}
std::uint64_t csv_records(std::string_view csv) {
    bool quoted = false;
    std::uint64_t records = 0;
    for (std::size_t i = 0; i < csv.size(); ++i) {
        if (csv[i] == '"') {
            if (quoted && i + 1 < csv.size() && csv[i + 1] == '"') {
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (csv[i] == '\n' && !quoted) {
            ++records;
        }
    }
    check(!quoted && records > 0, "invalid required report CSV");
    return records - 1;
}

struct HistoricalSection {
    std::string json, summary, html;
};
HistoricalSection historical_unavailable(std::string_view status, std::string_view reason) {
    const std::map<std::string, std::string> summaries{
        {"legacy_profile", "Not applicable: legacy initialization profile; no fresh "
                           "continuous-account historical section."},
        {"financial_scope_starts_after_cutoff",
         "Not applicable: the financial evaluation starts after 2025-12-31."},
        {"financial_scope_ends_before_cutoff",
         "Not applicable: the financial evaluation ends before 2025-12-31."},
        {"partial_financial_run", "Historical section unavailable: the financial run is partial; "
                                  "no cutoff metrics are asserted."}};
    const auto summary = summaries.at(std::string{reason});
    return {"{\"status\":" + json_string(status) + ",\"reason\":" + json_string(reason) +
                ",\"cutoff_date\":\"2025-12-31\",\"metrics\":null}",
            summary,
            "<section id=\"historical-section\"><h2>Historical continuous account through "
            "2025-12-31</h2><p id=\"historical-summary-text\">" +
                html_text(summary) + "</p></section>"};
}
HistoricalSection historical_section(const Context &c, std::size_t index,
                                     const Assembled &assembled) {
    if (!c.historical.cutoff_key) {
        return historical_unavailable("not_applicable", c.historical.not_applicable_reason);
    }
    check(assembled.historical.has_value(), "historical cutoff snapshot was not captured");
    const auto &prefix = *assembled.historical;
    // Views over the validated continuous trace: no synthetic final event, no
    // intent expiry at the cutoff, no replay and no fresh account/cohort reset.
    auto prepared = take(prepare_report(
        {date(c.dataset.rows()[c.suite.funding_key].session), c.suite.trials[index].initial_cash,
         std::span<const ReportClosePoint>{assembled.closes}.first(prefix.close_count),
         prefix.totals, std::span<const data::Money>{assembled.trades}.first(prefix.trade_count),
         std::nullopt}));
    const auto &m = prepared.metrics;
    check(m.sessions == *c.historical.cutoff_key - c.suite.evaluation_begin + 1 &&
              m.daily.size() == m.sessions && m.final_date == date(historical_cutoff_date) &&
              !m.terminal_cash_cents &&
              m.funded_date == date(c.dataset.rows()[c.suite.funding_key].session),
          "historical native metrics changed the continuous funding/scope/mark contract");
    const auto summary = "Continuous account from funding " +
                         c.dataset.rows()[c.suite.funding_key].session + " through 2025-12-31; " +
                         std::to_string(m.sessions) +
                         " ordinary closes; marked holdings; no capital reset or synthetic sale.";
    const Metadata table{
        {"Funding date", c.dataset.rows()[c.suite.funding_key].session},
        {"Cutoff date", "2025-12-31"},
        {"Sessions", std::to_string(m.sessions)},
        {"Currency", m.currency},
        {"Initial equity (cents)", std::to_string(m.funded_equity_cents)},
        {"Equity (cents)", std::to_string(m.final_equity_cents)},
        {"Net P&L (cents)", std::to_string(m.net_pnl_cents)},
        {"Cash (cents)", std::to_string(m.final_cash_cents)},
        {"Units (Quantity6 raw)", std::to_string(m.final_units_raw)},
        {"Cost basis (cents)",
         std::to_string(detail::sub(m.daily.back().invested_value_cents, m.unrealized_pnl_cents))},
        {"Realized trading P&L (cents)", std::to_string(m.realized_trading_pnl_cents)},
        {"Unrealized P&L (cents)", std::to_string(m.unrealized_pnl_cents)},
        {"Dividends (cents)", std::to_string(m.dividends_cents)},
        {"Fees (cents)", std::to_string(m.fees_cents)},
        {"Closed trades", std::to_string(m.completed_round_trips)},
        {"Maximum drawdown (fraction)", std::to_string(m.maximum_drawdown.numerator) + "/" +
                                            std::to_string(m.maximum_drawdown.denominator)},
        {"Endpoint", "Mark (no synthetic sale)"}};
    std::string html = "<section id=\"historical-section\"><h2>Historical continuous account "
                       "through 2025-12-31</h2>"
                       "<p id=\"historical-summary-text\">" +
                       html_text(summary) + "</p><table id=\"historical-summary\"><tbody>";
    for (const auto &[name, value] : table) {
        html += "<tr><th>" + html_text(name) + "</th><td>" + html_text(value) + "</td></tr>";
    }
    html += "</tbody></table><p>All historical metrics, daily observations and period/window "
            "returns are retained in "
            "<a href=\"#historical-json\">historical_section_json</a>.</p></section>";
    // Discard historical HTML/CSV and metric vectors before building the full
    // report. Only the historical JSON string and this compact table survive.
    return {std::move(prepared.artifacts.json), summary, std::move(html)};
}
void add_historical_metadata(Metadata &labels, HistoricalSection &historical,
                             std::uint64_t max_bytes) {
    check(historical.json.size() <= max_bytes, "historical JSON exceeds report artifact bound",
          ErrorCode::ResourceExceeded);
    labels.emplace_back("historical_section_profile", historical_profile);
    // This potentially large native JSON is already validated by prepare_report;
    // its byte ceiling replaces the ordinary 8192-byte printable-label limit.
    labels.emplace_back("historical_section_json", std::move(historical.json));
    labels.emplace_back("historical_section_summary", historical.summary);
}

ReportArtifacts completed(const Context &c, std::size_t index, Metadata labels) {
    auto assembled = assemble(c, index);
    auto historical = historical_section(c, index, assembled);
    add_historical_metadata(labels, historical, c.specification.max_artifact_bytes);
    const auto &t = c.suite.trials[index];
    auto prepared = take(prepare_report(
        {date(c.dataset.rows()[c.suite.funding_key].session), t.initial_cash, assembled.closes,
         std::move(assembled.totals), assembled.trades, assembled.terminal}));
    check(prepared.metrics.sessions == c.suite.evaluation_end - c.suite.evaluation_begin &&
              prepared.metrics.daily.size() == prepared.metrics.sessions &&
              prepared.metrics.funded_date == date(c.dataset.rows()[c.suite.funding_key].session) &&
              prepared.metrics.daily.front().date ==
                  date(c.dataset.rows()[c.suite.evaluation_begin].session) &&
              prepared.metrics.final_date ==
                  date(c.dataset.rows()[c.suite.evaluation_end - 1].session),
          "native report scope differs from actual financial suite keys", ErrorCode::Coverage);
    auto out = std::move(prepared.artifacts);
    out.json = "{\"schema\":\"quadeplus-report-evidence-v1\",\"financial_state\":\"completed\","
               "\"metadata\":" +
               metadata_json(labels) + ",\"metrics\":" + out.json + "}\n";
    out.csv = "path,type,value\r\n" + csv_row("/schema", "string", "quadeplus-report-evidence-v1") +
              csv_row("/financial_state", "string", "completed") + metadata_csv(labels) +
              prefixed_csv(out.csv);
    const auto at = out.html.find("<pre>");
    check(at != std::string::npos, "native report HTML has no payload section");
    std::string section = "<h2>Financial evidence context</h2><p>Financial state: completed. "
                          "Engine acceptance is separate.</p><dl>";
    for (const auto &[name, value] : labels) {
        section += "<dt>" + html_text(name) + "</dt>";
        section += name == "historical_section_json"
                       ? "<dd id=\"historical-json\" "
                         "style=\"white-space:pre-wrap;max-height:30em;overflow:auto\">"
                       : "<dd>";
        section += html_text(value) + "</dd>";
    }
    out.html.insert(at, historical.html + section + "</dl><h2>Full financial window</h2>");
    return out;
}
ReportArtifacts partial(const Context &c, std::size_t index, Metadata labels) {
    auto historical = historical_unavailable("unavailable", "partial_financial_run");
    add_historical_metadata(labels, historical, c.specification.max_artifact_bytes);
    const auto id = c.suite.trials[index].id;
    std::uint64_t events = 0, closes = 0;
    for (const auto &event : c.financial->events) {
        if (event.trial == id) {
            ++events;
            if (event.kind == "close" && event.key >= c.suite.evaluation_begin &&
                event.key < c.suite.evaluation_end) {
                ++closes;
            }
        }
    }
    const auto reason = c.financial->failure ? std::string{c.financial->failure->text()}
                                             : std::string{"financial completion not established"};
    const auto diagnostic = "{\"observed_trial_events\":" + std::to_string(events) +
                            ",\"observed_evaluation_close_events\":" + std::to_string(closes) +
                            ",\"reason\":" + json_string(reason) +
                            ",\"exhausted\":" + (c.financial->exhausted ? "true" : "false") +
                            ",\"finalized\":" + (c.financial->finalized ? "true" : "false") + "}";
    ReportArtifacts out;
    out.json = "{\"schema\":\"quadeplus-report-evidence-v1\",\"financial_state\":\"partial\","
               "\"metadata\":" +
               metadata_json(labels) + ",\"diagnostic\":" + diagnostic + ",\"metrics\":null}\n";
    out.csv =
        "path,type,value\r\n" + csv_row("/schema", "string", "quadeplus-report-evidence-v1") +
        csv_row("/financial_state", "string", "partial") + metadata_csv(labels) +
        csv_row("/metrics", "null", "null") +
        csv_row("/diagnostic/observed_trial_events", "number", std::to_string(events)) +
        csv_row("/diagnostic/observed_evaluation_close_events", "number", std::to_string(closes)) +
        csv_row("/diagnostic/reason", "string", reason) +
        csv_row("/diagnostic/exhausted", "boolean", c.financial->exhausted ? "true" : "false") +
        csv_row("/diagnostic/finalized", "boolean", c.financial->finalized ? "true" : "false");
    out.html = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>Partial "
               "financial diagnostic</title></head>"
               "<body><h1>Partial financial diagnostic</h1><p>Coverage and final metrics are not "
               "established. "
               "Observed event counts are diagnostic only; they do not certify complete sessions "
               "or acceptance.</p>" +
               historical.html + "<pre>" + html_text(out.json) + "</pre></body></html>\n";
    return out;
}

struct TrialSinkState {
    std::shared_ptr<Context> context;
    std::size_t index;
    // Declare before artifacts so cached strings are destroyed before their
    // reservation even if finalization stops with undelivered artifacts.
    lx::Reservation retained;
    std::optional<ReportArtifacts> artifacts{};
    std::array<bool, 3> delivered{};
    std::exception_ptr serialization_failure{};
    std::string output(std::size_t format) {
        check(format < 3 && context->next_report_artifact == index * 3 + format,
              "required reports must finalize in trial-major HTML/JSON/CSV order");
        ++context->next_report_artifact;
        if (format == 0) {
            // No prior report callback is active in the synchronous host. Release
            // its deferred charge BEFORE reserving this trial's artifact budget.
            context->deferred_trial_artifacts = lx::Reservation{};
        }
        if (serialization_failure) {
            std::rethrow_exception(serialization_failure);
        }
        check(!delivered[format], "required report artifact finalized more than once");
        const auto &c = *context;
        const auto &f = *c.financial;
        try {
            if (!artifacts) {
                check(f.events.size() <= c.suite.limits.max_trace_records,
                      "report host trace exceeds admitted records", ErrorCode::ResourceExceeded);
                check(f.trials.size() == c.suite.trials.size(), "report trial inventory changed");
                if (f.finalized && !f.exhausted) {
                    fail(ErrorCode::Integrity, "finalized financial run lacks exhaustion");
                }
                for (const auto &event : f.events) {
                    check(std::find(c.specification.trial_ids.begin(),
                                    c.specification.trial_ids.end(),
                                    event.trial) != c.specification.trial_ids.end(),
                          "report trace contains an undeclared trial");
                }
                retained = reserve(c.dataset.account(), lx::ResourceKind::Evidence,
                                   times(c.specification.max_artifact_bytes, 3));
                const auto transient =
                    plus(times(c.specification.max_artifact_bytes, 8),
                         plus(times(c.specification.max_rows, 4096),
                              plus(times(c.suite.limits.max_ledger_events, 512), 64 * kib)));
                auto scratch = reserve(c.dataset.account(), lx::ResourceKind::Scratch, transient);
                auto labels = metadata(c, index);
                artifacts = f.finalized && f.exhausted && !f.failure
                                ? completed(c, index, std::move(labels))
                                : partial(c, index, std::move(labels));
                for (const auto *value : {&artifacts->html, &artifacts->json, &artifacts->csv}) {
                    check(value->size() <= c.specification.max_artifact_bytes,
                          "required report exceeds declared byte ceiling",
                          ErrorCode::ResourceExceeded);
                }
                check(csv_records(artifacts->csv) <=
                          c.specification.required[index * 3 + 2].max_records,
                      "required report CSV exceeds declared record ceiling",
                      ErrorCode::ResourceExceeded);
            }
        } catch (...) {
            serialization_failure = std::current_exception();
            artifacts.reset();
            retained = lx::Reservation{};
            throw;
        }
        delivered[format] = true;
        auto result = std::move(format == 0   ? artifacts->html
                                : format == 1 ? artifacts->json
                                              : artifacts->csv);
        if (std::all_of(delivered.begin(), delivered.end(), [](bool value) { return value; })) {
            artifacts.reset();
            context->deferred_trial_artifacts = std::move(retained);
        }
        return result;
    }
};

} // namespace

BacktestResult<ReportEvidenceSpecs> report_specs(const std::filesystem::path &out,
                                                 std::span<const std::uint32_t> ids,
                                                 std::uint64_t rows, std::uint64_t bytes) noexcept {
    return detail::guard(
        [&]() -> BacktestResult<ReportEvidenceSpecs> { return specs(out, ids, rows, bytes); });
}

BacktestResult<std::vector<lx::RequiredEvidenceSink>>
bind_report_sinks(const ReportEvidenceSpecs &specification,
                  std::shared_ptr<const FinancialRun> financial, SuiteConfiguration suite,
                  research::DailyDataset dataset, ReportEvidenceLabels labels) noexcept {
    return detail::guard([&]() -> BacktestResult<std::vector<lx::RequiredEvidenceSink>> {
        const auto expected = specs(specification.output_directory, specification.trial_ids,
                                    specification.max_rows, specification.max_artifact_bytes);
        check(expected.required == specification.required,
              "report evidence declarations changed after preparation", ErrorCode::InvalidConfig);
        check(financial && !financial->finalized && !financial->exhausted && !financial->failure &&
                  financial->events.empty(),
              "report sinks must bind to a fresh HostedSimulation before execution",
              ErrorCode::InvalidConfig);
        check(financial->experiment != lx::Sha256{} && !financial->effective_configuration.empty(),
              "report binding lacks the native financial experiment identity",
              ErrorCode::InvalidConfig);
        check(financial->trials.size() == suite.trials.size() &&
                  suite.trials.size() == specification.trial_ids.size(),
              "report trial inventory mismatch", ErrorCode::InvalidConfig);
        check(suite.funding_key != UINT64_MAX && suite.funding_key + 1 == suite.evaluation_begin &&
                  suite.evaluation_begin < suite.evaluation_end &&
                  suite.evaluation_end == dataset.rows().size() &&
                  suite.evaluation_end - suite.evaluation_begin <= specification.max_rows,
              "report suite/data/funding scope mismatch", ErrorCode::Coverage);
        const auto &ds = dataset.specification();
        check(ds.adjustment == research::adjustment_profile,
              "unsupported report dataset adjustment policy", ErrorCode::Unsupported);
        const auto permitted_begin = date(ds.evaluation_begin),
                   permitted_end = date(ds.evaluation_end);
        const auto actual_begin = date(dataset.rows()[suite.evaluation_begin].session);
        const auto actual_end = date(dataset.rows()[suite.evaluation_end - 1].session);
        check(permitted_begin <= permitted_end && actual_begin >= permitted_begin &&
                  actual_begin <= permitted_end && actual_end >= actual_begin &&
                  actual_end <= permitted_end,
              "actual report financial scope is outside the manifest's permitted range",
              ErrorCode::Coverage);
        check(dataset.rows().back().session == ds.evaluation_end &&
                  dataset.rows().front().session == ds.numerical_origin,
              "report numerical origin/end differs from prepared dataset manifest",
              ErrorCode::Coverage);
        for (std::size_t i = 0; i < suite.trials.size(); ++i) {
            check(suite.trials[i].id == specification.trial_ids[i] &&
                      suite.trials[i].initial_cash.currency().value() == ds.currency,
                  "report suite IDs/currency differ from declared trials",
                  ErrorCode::InvalidConfig);
            detail::policy(suite.trials[i].execution);
            check(suite.trials[i].terminal == TerminalMode::Mark ||
                      suite.trials[i].terminal == TerminalMode::SyntheticClose,
                  "unsupported report terminal policy", ErrorCode::Unsupported);
        }
        check(labels.profile ==
                  (suite.legacy_funding_close ? "legacy_spy_2026_v1" : "fresh_research_v1"),
              "report profile label contradicts financial initialization",
              ErrorCode::InvalidConfig);
        text(labels.run_label, 256);
        text(labels.profile, 128);
        text(labels.window_status, 256);
        text(labels.provenance, 8192);
        // Validate dates once while binding; rows retain declared source succession.
        auto previous = date(dataset.rows()[suite.funding_key].session);
        for (auto key = suite.evaluation_begin; key < suite.evaluation_end; ++key) {
            const auto current = date(dataset.rows()[key].session);
            check(current > previous, "report session dates do not increase", ErrorCode::Coverage);
            previous = current;
        }
        const auto historical = historical_scope(dataset, suite, labels);
        auto charge = reserve(dataset.account(), lx::ResourceKind::Graph,
                              times(64 * kib, suite.trials.size() + 1));
        auto context = std::make_shared<Context>(Context{specification, std::move(financial),
                                                         std::move(suite), std::move(dataset),
                                                         std::move(labels), std::move(charge)});
        context->historical = historical;
        std::vector<lx::RequiredEvidenceSink> result;
        result.reserve(specification.required.size());
        for (std::size_t i = 0; i < context->suite.trials.size(); ++i) {
            auto state = std::make_shared<TrialSinkState>(TrialSinkState{context, i, {}, {}, {}});
            for (std::size_t format = 0; format < 3; ++format) {
                result.push_back(make_sink(specification.required[i * 3 + format],
                                           context->dataset.account(),
                                           [state, format] { return state->output(format); }));
            }
        }
        return result;
    });
}

} // namespace quant::backtest::app
