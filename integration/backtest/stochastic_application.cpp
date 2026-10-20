#include "application.hpp"
#include "report_evidence.hpp"
#include "quant/strategies/stochastic.hpp"
#include "quant/research/daily_source.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/detail.hpp"
#include "file_evidence.hpp"
#include "native_files.hpp"
#include "xml_frontend.hpp"

#include <array>
#include <chrono>
#include <sstream>

namespace quant::backtest::app {
namespace {
namespace lx = ::lexongine;
namespace qs = quant::strategies;
namespace qr = quant::research;
using detail::take;
using detail::fail;
constexpr std::uint64_t mb = 1048576;
constexpr std::string_view spy_pin =
    "28963337b6eba735cad1eb62dc333d497079cff73f02076d17d3439c48e167ae";
constexpr std::array<std::string_view, 3> trial_names{
    qs::stochastic_strategy_id, "cash_usd_no_interest_v1", "buy_hold95_next_open_v1"};

template <class T> T engine(std::variant<T, lx::Diagnostic> result) {
    if (const auto *error = std::get_if<lx::Diagnostic>(&result)) {
        fail(ErrorCode::SourceBinding, error->text());
    }
    return std::get<T>(std::move(result));
}
std::string hex(const lx::Sha256 &digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}
std::string terminal_name(TerminalMode mode) {
    return mode == TerminalMode::Mark ? "mark" : "synthetic_close";
}
std::string side_name(data::Side side) {
    return side == data::Side::Buy ? "Buy" : "Sell";
}
template <class T> std::string number(std::optional<T> value) {
    return value ? std::to_string(*value) : "";
}
lx::RequiredEvidenceSpec evidence_spec(std::string name, std::string format,
                                       const std::filesystem::path &out, std::string file,
                                       std::uint64_t bytes, std::uint64_t records) {
    return {std::move(name),
            std::move(format),
            (out / file).string(),
            "create_new",
            bytes,
            records,
            "on_close",
            "stop"};
}

std::uint64_t validate_scope(const qr::DailyDataset &data, const Config &cfg) {
    const auto n = data.rows().size();
    const auto &ds = data.specification();
    const bool legacy = cfg.profile == "legacy_spy_2026_v1";
    if (cfg.strategy != qs::stochastic_strategy_id ||
        (!legacy && cfg.profile != "fresh_research_v1") || n < 5 || n > 8000 ||
        ds.symbol != cfg.symbol || ds.currency != "USD" || ds.numerical_origin != cfg.origin ||
        ds.evaluation_end != cfg.end || cfg.begin < ds.evaluation_begin ||
        cfg.begin > ds.evaluation_end || data.rows().front().session != cfg.origin ||
        data.rows().back().session != cfg.end || cfg.initial_cash <= 0 || cfg.lot <= 0 ||
        cfg.max_price_raw <= 0 || !cfg.cache_directory.empty() ||
        (cfg.sharing != "exact" && cfg.sharing != "disabled")) {
        fail(ErrorCode::InvalidConfig,
             "S3 config, permitted financial range, or full numerical scope mismatch");
    }
    const auto cost = cfg.costs.commission_bps;
    const bool admitted_cost = cost == 5 || cost == 10 || cost == 20;
    if (cfg.costs.commission_bps != cfg.costs.slippage_bps || !admitted_cost ||
        cfg.terminal != (legacy ? TerminalMode::SyntheticClose : TerminalMode::Mark)) {
        fail(ErrorCode::Unsupported,
             "S3 requires paired 5/10 bps or diagnostic 20, and its declared profile endpoint");
    }
    const auto first = std::find_if(data.rows().begin(), data.rows().end(),
                                    [&](const auto &row) { return row.session == cfg.begin; });
    const auto begin = static_cast<std::uint64_t>(first - data.rows().begin());
    if (begin == 0 || begin >= n) {
        fail(ErrorCode::Coverage,
             "S3 needs an exact evaluation-begin session and preceding funding close");
    }
    if (legacy && (cfg.symbol != "SPY" || cfg.origin != "2004-11-18" || cfg.begin != "2026-08-17" ||
                   cfg.end != "2026-10-15" || data.rows()[begin - 1].session != "2026-08-14" ||
                   n - begin != 43 || n != 5511 || data.prices_sha256() != spy_pin ||
                   ds.schedule != "retained_reference_grid_v1" || cfg.initial_cash != 1000000 ||
                   cfg.lot != 1)) {
        fail(ErrorCode::InvalidConfig, "legacy S3 requires the pinned SPY input, funding/window, "
                                       "USD 10000, and micro-unit lots");
    }
    // The ratio kernel does not take a Price bound. Admit the entire immutable
    // source before funding and bind this separate financial bound in identity.
    for (const auto &row : data.rows()) {
        for (auto value : {row.bar.open, row.bar.high, row.bar.low, row.bar.close}) {
            if (value.raw() <= 0 || value.raw() > cfg.max_price_raw) {
                fail(ErrorCode::NumericRange,
                     "S3 source OHLC exceeds the configured positive Price9 bound");
            }
        }
    }
    return begin;
}

std::string graph_xml(const qr::DailyDataset &data, const Config &cfg,
                      const std::filesystem::path &out, std::string_view run_id,
                      std::uint64_t journal_events) {
    const auto n = data.rows().size();
    const std::string clock{qr::clock_profile};
    std::ostringstream xml;
    xml << "<composition version=\"1\"><run id=\"" << run_id << "\" clock=\"" << clock
        << "\" driver=\"hosted\" execution=\"synchronous\" sharing=\"" << cfg.sharing
        << "\" failure=\"stop\" trace=\"steps\" retention_warning_steps=\"128\" "
           "retention_log_path=\""
        << escape_xml((out / "retention.log").string())
        << "\" retention_log_format=\"key_value_lines_v1\" retention_log_open=\"create_new\" "
           "retention_log_flush=\"on_event\""
        << " retention_log_failure=\"stop\" retention_log_overflow=\"stop\" "
           "retention_log_max_bytes=\"536870912\" retention_log_max_events=\""
        << journal_events << "\"/>"
        << "<limits xml_bytes=\"1048576\" xml_depth=\"32\" xml_elements=\"1024\" "
           "string_bytes=\"4096\" nodes=\"32\" edges=\"64\""
        << " requests=\"16\" source_updates=\"" << n
        << "\" managed_buffer_bytes=\"4294967296\" trace_events=\"" << 4 * n + 64
        << "\" trace_message_bytes=\"256\"/><plugins/><cache mode=\"disabled\"/>"
        << "<sources><source id=\"s\" operation=\"quadeplus.daily_research\" "
           "semantic_version=\"1\" implementation_id=\"sealed-daily-source-v1\""
        << " source_version=\"" << data.manifest_sha256() << "\" clock=\"" << clock
        << "\" begin_step=\"0\" end_step=\"" << n
        << "\" completeness=\"complete_only\" empty=\"reject\" missing=\"fail\" "
           "missing_intervals=\"strict\" availability=\""
        << clock << "\"/></sources>"
        << "<nodes><node id=\"k5\" operation=\"quadeplus.stochastic5\" semantic_version=\"1\" "
           "implementation_id=\"stochastic-v1\""
        << " alignment=\"same_step\" initialization=\"empty_at_step_0\"><param name=\"window\" "
           "value=\"5\"/>"
        << "<param name=\"zero_range\" value=\"1/2\"/><param name=\"coverage\" "
           "value=\"strict_complete_bars\"/>"
        << "<input port=\"bar\" producer=\"s\" output=\"bar\" prior_outputs=\"0\" "
           "missing_intervals=\"strict\"/></node></nodes>"
        << "<observations><observe id=\"k5\" producer=\"k5\" output=\"k\" begin_step=\"4\" "
           "end_step=\""
        << n
        << "\" absence=\"require_value\" prior_outputs=\"0\" "
           "missing_intervals=\"strict\"/></observations></composition>";
    return xml.str();
}

// Empty CSV fields are inapplicable, never numeric zero. Ratio representation is
// exact but need not be reduced; adapters compare numerator/denominator rationally.
enum Column : std::size_t {
    Trial,
    Key,
    Date,
    Kind,
    MaySubmit,
    Funded,
    KNumerator,
    KDenominator,
    KAvailable,
    EpisodeStart,
    EpisodeAction,
    Target,
    EntryConsumed,
    HeldUnits,
    PendingSide,
    PendingDecision,
    ParityFailed,
    DecisionAction,
    Reason,
    OrderId,
    DecisionKey,
    EligibleKey,
    Side,
    Synthetic,
    RejectionCode,
    Quantity,
    Price,
    Gross,
    Fee,
    Cash,
    Units,
    Basis,
    Realized,
    Dividends,
    Fees,
    ColumnCount
};
using TraceRow = std::array<std::string, ColumnCount>;
constexpr std::string_view trace_header =
    "trial,key,date,kind,may_submit,funded,k_numerator,k_denominator,k_available_at_ns,episode_"
    "start,episode_action,target,"
    "entry_consumed,held_units_raw,pending_side,pending_decision_key,parity_failed,action,reason,"
    "order_id,decision_key,"
    "eligible_key,side,synthetic,rejection_code,quantity_raw,price_raw,gross_cents,fee_cents,cash_"
    "cents,units_raw,basis_cents,"
    "realized_cents,dividend_cents,total_fee_cents\n";
struct Trace {
    lx::Reservation charge;
    std::vector<TraceRow> rows;
    std::uint64_t maximum = 0;
    qs::StochasticState strategy;
    std::array<std::optional<std::uint64_t>, 3> submitted{};
    std::shared_ptr<const FinancialRun> financial;
    std::shared_ptr<std::vector<Summary>> summaries = std::make_shared<std::vector<Summary>>();
    void append(TraceRow row) {
        if (rows.size() == maximum) {
            fail(ErrorCode::ResourceExceeded, "S3 state trace exceeds admitted rows");
        }
        rows.push_back(std::move(row));
    }
};
void balances(TraceRow &row, const Account *a) {
    if (!a) {
        return;
    }
    row[Cash] = std::to_string(a->cash.amount());
    row[Units] = std::to_string(a->units.raw());
    row[Basis] = std::to_string(a->basis.amount());
    row[Realized] = std::to_string(a->realized.amount());
    row[Dividends] = std::to_string(a->dividends.amount());
    row[Fees] = std::to_string(a->fees.amount());
}
void state_fields(TraceRow &row, const qs::StochasticState &state) {
    row[EpisodeStart] = number(state.episode_start);
    row[Target] = state.episode_start ? "1" : "0";
    row[EntryConsumed] = state.entry_consumed ? "1" : "0";
    row[HeldUnits] = std::to_string(state.held_units);
    row[PendingSide] = state.pending_side ? side_name(*state.pending_side) : "";
    row[PendingDecision] = number(state.pending_decision);
    row[ParityFailed] = state.parity_failed ? "1" : "0";
}
std::string episode_action(const qs::StochasticState &state, std::optional<std::uint64_t> previous,
                           const DecisionContext &c, std::optional<qs::StochasticValue> k) {
    if (state.profile == qs::StochasticProfile::Fresh && !c.may_submit) {
        return "warmup";
    }
    if (!k) {
        return "warming";
    }
    if (!previous && state.episode_start) {
        return "activate";
    }
    if (previous && !state.episode_start) {
        return take(ratio_greater(k->numerator, k->denominator, 4, 5)) ? "threshold_exit"
                                                                       : "timer_exit";
    }
    return state.episode_start ? "active" : "inactive";
}
std::string serialize_trace(const Trace &trace) {
    std::string out{trace_header};
    for (const auto &row : trace.rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) {
                out += ',';
            }
            // All current text fields are controlled identifiers. Fail rather
            // than silently emitting an ambiguous record if that ever changes.
            if (row[i].find_first_of(",\r\n\"") != std::string::npos) {
                fail(ErrorCode::Integrity, "invalid S3 trace CSV field");
            }
            out += row[i];
        }
        out += '\n';
    }
    return out;
}

std::string summary_json(Trace &trace, const Config &cfg, const qr::DailyDataset &data,
                         std::uint64_t begin) {
    const auto &financial = *trace.financial;
    std::ostringstream out;
    out << "{\"format\":\"quadeplus_stochastic_summary_v1\",\"profile\":\"" << cfg.profile
        << "\",\"financial_state\":\"";
    if (!financial.finalized || !financial.exhausted || financial.failure) {
        out << "partial\",\"evaluation_sessions\":null,\"trials\":[]}\n";
        return out.str();
    }
    out << "completed\",\"dataset_sha256\":\"" << data.manifest_sha256() << "\",\"experiment\":\""
        << hex(financial.experiment)
        << "\",\"window_status\":\"previously_inspected_research_not_holdout\",\"terminal\":\""
        << terminal_name(cfg.terminal) << "\",\"dividend_policy\":\"" << dividend_profile
        << "\",\"numerical_origin\":\"" << cfg.origin << "\",\"funding_date\":\""
        << data.rows()[begin - 1].session << "\",\"evaluation_begin\":\"" << cfg.begin
        << "\",\"evaluation_end\":\"" << cfg.end
        << "\",\"evaluation_sessions\":" << data.rows().size() - begin
        << ",\"initial_cash_cents\":" << cfg.initial_cash << ",\"lot_step_raw\":" << cfg.lot
        << ",\"commission_bps\":" << cfg.costs.commission_bps
        << ",\"slippage_bps\":" << cfg.costs.slippage_bps << ",\"trials\":[";
    trace.summaries->clear();
    if (financial.trials.size() != 3) {
        fail(ErrorCode::Integrity, "S3 requires three independent financial trials");
    }
    for (std::size_t i = 0; i < financial.trials.size(); ++i) {
        const auto &t = financial.trials[i];
        if (!t.account || !t.ledger) {
            fail(ErrorCode::Integrity, "completed S3 trial lacks accounting evidence");
        }
        const auto &a = *t.account;
        std::optional<std::int64_t> ending;
        std::uint64_t rejections = 0, expired = 0, trades = 0, close_count = 0;
        for (const auto &event : financial.events) {
            if (event.trial != a.trial) {
                continue;
            }
            if (event.kind == "final" && event.equity) {
                if (ending) {
                    fail(ErrorCode::Integrity, "duplicate S3 final valuation");
                }
                ending = event.equity;
            }
            rejections += event.kind == "rejected";
            expired += event.kind == "expired";
        }
        if (!ending) {
            fail(ErrorCode::Integrity, "S3 final valuation missing");
        }
        for (const auto &row : t.ledger->rows()) {
            if (const auto *fill = std::get_if<Fill>(&row.event);
                fill && fill->side == data::Side::Sell) {
                ++trades;
            }
        }
        std::int64_t peak = cfg.initial_cash, loss = 0, loss_peak = peak;
        for (const auto &event : financial.events) {
            if (event.trial != a.trial || event.kind != "close" || event.key < begin ||
                !event.equity) {
                continue;
            }
            ++close_count;
            // A terminal synthetic sale replaces this session's reported value;
            // its ordinary close remains independently retained in financial.txt.
            const auto equity = event.key == data.rows().size() - 1 ? *ending : *event.equity;
            peak = std::max(peak, equity);
            const auto drawdown = detail::sub(peak, equity);
            if (take(ratio_greater(drawdown, peak, loss, loss_peak))) {
                loss = drawdown;
                loss_peak = peak;
            }
        }
        if (close_count != data.rows().size() - begin) {
            fail(ErrorCode::Coverage, "S3 summary close/session coverage mismatch");
        }
        Summary s{a.trial,
                  std::string{trial_names[i]},
                  a.cash.amount(),
                  a.units.raw(),
                  *ending,
                  detail::sub(*ending, cfg.initial_cash),
                  a.realized.amount(),
                  detail::sub(detail::sub(*ending, a.cash.amount()), a.basis.amount()),
                  a.dividends.amount(),
                  a.fees.amount(),
                  loss,
                  loss_peak};
        if (s.pnl != detail::add(detail::add(s.realized, s.unrealized), s.dividends)) {
            fail(ErrorCode::Integrity, "S3 summary PnL does not reconcile");
        }
        trace.summaries->push_back(s);
        if (i) {
            out << ',';
        }
        out << "{\"id\":" << a.trial << ",\"strategy\":\"" << trial_names[i]
            << "\",\"cash_cents\":" << s.cash << ",\"units_raw\":" << s.units
            << ",\"equity_cents\":" << s.equity << ",\"pnl_cents\":" << s.pnl
            << ",\"realized_cents\":" << s.realized << ",\"unrealized_cents\":" << s.unrealized
            << ",\"dividend_cents\":" << s.dividends << ",\"fee_cents\":" << s.fees
            << ",\"drawdown_loss_cents\":" << s.drawdown_loss
            << ",\"drawdown_peak_cents\":" << s.drawdown_peak
            << ",\"completed_positions\":" << trades << ",\"rejected_orders\":" << rejections
            << ",\"expired_orders\":" << expired << '}';
    }
    out << "]}\n";
    return out.str();
}
} // namespace

BacktestResult<ApplicationRun> run_stochastic(const qr::DailyDataset &data, const Config &cfg,
                                              const std::filesystem::path &out,
                                              const lx::HostBuild &build) noexcept {
    return detail::guard([&]() -> BacktestResult<ApplicationRun> {
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        require_new_output(out);
        const auto begin = validate_scope(data, cfg),
                   n = static_cast<std::uint64_t>(data.rows().size());
        const bool legacy = cfg.profile == "legacy_spy_2026_v1";
        const std::string run_id = legacy ? "stochastic-legacy" : "stochastic-fresh";
        if (!std::filesystem::create_directory(out)) {
            fail(ErrorCode::IO, "new S3 output directory required");
        }

        auto registry = engine(lx::Registry::create());
        const quant::lexongine::ComponentRegistration registration{
            build.identity(), {}, lx::MissingIntervalPolicy::Strict};
        auto types = engine(quant::lexongine::register_components(registry, registration));
        auto ratio = engine(qs::register_stochastic(registry, types, registration));
        // Source preparation must use the dataset's original account, not a
        // replacement resources() account: source ownership is checked by prepare.
        auto resolver = engine(lx::SourceResolver::create(data.account(), 65536, 4));
        auto source = take(
            qr::register_daily_source(registry, resolver, types.bar, data,
                                      {{n, data.owned_bytes(), 32 * mb, mb, 4096},
                                       std::string{qr::clock_profile},
                                       "daily",
                                       "sealed",
                                       {"quadeplus", "daily_research", 1, "sealed-daily-source-v1"},
                                       "USD",
                                       "complete_only",
                                       "reject",
                                       "fail",
                                       std::string{qr::clock_profile},
                                       lx::MissingIntervalPolicy::Strict},
                                      registration));
        engine(registry.freeze());
        engine(resolver.freeze());

        auto trace = std::make_shared<Trace>();
        trace->charge = engine(data.account()->reserve(lx::ResourceKind::Graph, 8 * mb + n * 8192));
        trace->maximum = 5 * n + 32;
        trace->rows.reserve(trace->maximum);
        trace->summaries->reserve(3);
        trace->strategy.profile =
            legacy ? qs::StochasticProfile::Legacy : qs::StochasticProfile::Fresh;
        const HostLimits limits{3, 2 * n + 16, 16 * n + 64, 16 * mb};
        SuiteConfiguration suite{
            begin - 1, begin,  n,
            legacy,    limits, financial_evidence_spec((out / "financial.txt").string(), limits),
            {}};
        const auto cash = data::Money::from_raw(data::CurrencyCode{"USD"}, cfg.initial_cash);
        const auto lot = data::Quantity::from_raw(cfg.lot);
        const std::string rejection{legacy ? qs::stochastic_legacy_policy
                                           : qs::stochastic_fresh_policy};
        const std::string common =
            "profile=" + cfg.profile + ";terminal=" + terminal_name(cfg.terminal) +
            ";max_price_raw=" + std::to_string(cfg.max_price_raw) + ";origin=" + cfg.origin +
            ";funding=" + data.rows()[begin - 1].session + ";evaluation_begin=" + cfg.begin +
            ";evaluation_end=" + cfg.end + ";allocation=95/100;structural_risk_only;";
        suite.trials.push_back(
            {1,
             std::string{trial_names[0]},
             common +
                 "window=5;zero_range=1/2;entry_lt=1/5;exit_gt=4/5;timer=10;rejection=" + rejection,
             cash,
             lot,
             cfg.costs,
             {},
             cfg.terminal,
             {{"k5", 4, n, FeatureKind::Ratio}}});
        suite.trials.push_back({2,
                                std::string{trial_names[1]},
                                common + "cash_only;no_interest",
                                cash,
                                lot,
                                cfg.costs,
                                {},
                                cfg.terminal,
                                {}});
        suite.trials.push_back({3,
                                std::string{trial_names[2]},
                                common + (legacy ? "funding_close" : "first_evaluation_close") +
                                    ";single_entry;no_rebalance",
                                cash,
                                lot,
                                cfg.costs,
                                {},
                                cfg.terminal,
                                {}});
        std::vector<DecisionCallback> decisions;
        std::vector<ExecutionCallback> executions;
        for (std::size_t i = 0; i < 3; ++i) {
            decisions.push_back(
                [trace, data, begin, legacy,
                 i](const DecisionContext &c, const Account *a,
                    std::span<const PriceFeature> prices) -> BacktestResult<Action> {
                    return detail::guard([&]() -> BacktestResult<Action> {
                        if (!prices.empty() || c.key >= data.rows().size()) {
                            fail(ErrorCode::SourceBinding,
                                 "S3 received unexpected Price/key input");
                        }
                        TraceRow row{};
                        row[Trial] = std::to_string(i + 1);
                        row[Key] = std::to_string(c.key);
                        row[Date] = data.rows()[c.key].session;
                        row[Kind] = "decision";
                        row[MaySubmit] = c.may_submit ? "1" : "0";
                        row[Funded] = c.funded ? "1" : "0";
                        Action action{ActionKind::Hold, data::Quantity::from_raw(0),
                                      i == 1 ? "cash_control" : "buy_hold_control"};
                        if (i == 0) {
                            if (c.ratios.size() != (c.key >= 4 ? 1U : 0U)) {
                                fail(ErrorCode::SourceBinding,
                                     "S3 requires exactly its current ready K receipt");
                            }
                            std::optional<qs::StochasticValue> k;
                            if (!c.ratios.empty()) {
                                const auto &f = c.ratios.front();
                                if (f.request != "k5" || f.key != c.key ||
                                    f.available_at_ns > data.rows()[c.key].close_available_ns) {
                                    fail(ErrorCode::FutureRead,
                                         "S3 ratio receipt request/key/availability mismatch");
                                }
                                k = f.value;
                                row[KNumerator] = std::to_string(k->numerator);
                                row[KDenominator] = std::to_string(k->denominator);
                                row[KAvailable] = std::to_string(f.available_at_ns);
                            }
                            const auto previous = trace->strategy.episode_start;
                            action = take(qs::stochastic_decision(trace->strategy, c, a, k));
                            state_fields(row, trace->strategy);
                            row[EpisodeAction] = episode_action(trace->strategy, previous, c, k);
                        } else {
                            if (!c.ratios.empty()) {
                                fail(ErrorCode::SourceBinding,
                                     "S3 control received another trial's ratio");
                            }
                            if (i == 2 && c.may_submit && c.key == (legacy ? begin - 1 : begin)) {
                                action.kind = ActionKind::TargetLong;
                            }
                        }
                        row[DecisionAction] = std::to_string(static_cast<int>(action.kind));
                        row[Reason] = action.reason;
                        row[DecisionKey] = std::to_string(c.key);
                        if (action.kind == ActionKind::TargetLong ||
                            action.kind == ActionKind::TargetCash) {
                            row[EligibleKey] = std::to_string(detail::next(c.key));
                            trace->submitted[i] = c.key;
                        }
                        balances(row, a);
                        trace->append(std::move(row));
                        return action;
                    });
                });
            executions.push_back([trace, data, i](const FillQuote &quote,
                                                  const Account &a) -> BacktestResult<void> {
                return detail::guard([&]() -> BacktestResult<void> {
                    TraceRow row{};
                    row[Trial] = std::to_string(i + 1);
                    row[Funded] = "1";
                    if (const auto *fill = std::get_if<Fill>(&quote)) {
                        row[Key] = std::to_string(fill->mark.key());
                        row[Date] = data.rows()[fill->mark.key()].session;
                        row[Kind] = fill->synthetic_terminal ? "terminal" : "filled";
                        row[OrderId] = std::to_string(fill->id);
                        row[Reason] = fill->reason;
                        row[Side] = side_name(fill->side);
                        row[Synthetic] = fill->synthetic_terminal ? "1" : "0";
                        row[Quantity] = std::to_string(fill->quantity.raw());
                        row[Price] = std::to_string(fill->executed_price.raw());
                        row[Gross] = std::to_string(fill->gross.amount());
                        row[Fee] = std::to_string(fill->fee.amount());
                        if (fill->intent) {
                            row[DecisionKey] = std::to_string(fill->intent->decision_key);
                            row[EligibleKey] = std::to_string(fill->intent->earliest_open_key);
                        }
                    } else {
                        const auto &rejected = std::get<OrderRejection>(quote);
                        if (!a.last_event) {
                            fail(ErrorCode::Integrity,
                                 "S3 rejection lacks opening ledger location");
                        }
                        row[Key] = std::to_string(a.last_event->key);
                        row[Date] = data.rows()[a.last_event->key].session;
                        row[Kind] = "rejected";
                        row[OrderId] = std::to_string(rejected.id);
                        row[Reason] = "order_rejected";
                        row[RejectionCode] = std::to_string(static_cast<int>(rejected.reason));
                        row[DecisionKey] = number(trace->submitted[i]);
                        if (trace->submitted[i]) {
                            row[EligibleKey] = std::to_string(detail::next(*trace->submitted[i]));
                        }
                    }
                    auto notified =
                        i == 0 ? qs::stochastic_execution(trace->strategy, quote, a) : ok();
                    if (i == 0) {
                        state_fields(row, trace->strategy);
                    }
                    balances(row, &a);
                    trace->append(std::move(row));
                    trace->submitted[i].reset();
                    // Preserve the rejected legacy row before propagating its fatal parity error.
                    return notified;
                });
            });
        }

        // Resource journals count more than retention.log
        // lines. Admit an explicit per-source-row budget, above its 524288 baseline
        // for the 5511-row fixture, without discarding events or weakening checks.
        const auto audit_events = std::max<std::uint64_t>(524288, 128 * n + 16384);
        const auto xml = graph_xml(data, cfg, out, run_id, audit_events);
        auto parsed = engine(lx::parse_xml(xml, {mb, 32, 16384, 4096, data.account()}));
        auto composition = engine(lx::resolve_composition(parsed, registry));
        lx::PrepareOptions options{
            run_id,
            take(source.request({std::string{qr::clock_profile}, 0, n})),
            {data.account(),
             {{n, n, 0, n}, 4096, 32 * mb, 32 * mb, mb},
             lx::MissingIntervalPolicy::Strict,
             {data.owned_bytes(), mb, 0, 8192, sizeof(data::Bar) + 512, 64 * mb, 0}},
            {{0, 32 * mb, 16 * mb, 256 * mb, 256 * mb, 256 * mb,
              audit_events * sizeof(lx::RetentionEvent) + 32 * mb},
             131072,
             16384,
             audit_events,
             128},
            {mb, 32 * mb, 16 * mb, 16 * mb, 32 * mb, 4 * mb, mb},
            8 * mb,
            16 * mb,
            [](std::span<const std::byte> bytes) { return lx::files::digest(bytes); },
            {}};
        options.evidence = {
            suite.financial_evidence,
            evidence_spec("stochastic", "stochastic_state_csv_v1", out, "stochastic.csv", 32 * mb,
                          trace->maximum),
            evidence_spec("summary", "stochastic_summary_json_v1", out, "summary.json", mb, 3),
            evidence_spec("config", "domain_xml_v1", out, "effective-config.xml", 65536, 1),
            evidence_spec("graph", "composition_xml_v1", out, "graph.xml", mb, 1),
            evidence_spec("producers", "producer_counts_csv_v1", out, "producers.csv", mb, 32),
            evidence_spec("identities", "computation_ids_csv_v1", out, "identities.csv", mb, 32),
            evidence_spec("input_identity", "stochastic_input_identity_v1", out,
                          "input-identity.txt", 65536, 1)};
        options.evidence_account = data.account();
        options.evidence_resources = {0, 0, 2048 * mb, 64 * mb, 0, 128 * mb, 32 * mb};
        const std::array<std::uint32_t, 3> report_ids{1, 2, 3};
        auto reports = take(report_specs(out, report_ids, n - begin, 16 * mb));
        options.evidence.insert(options.evidence.end(), reports.required.begin(),
                                reports.required.end());
        auto report_suite = suite;
        auto prepared = engine(lx::prepare(composition, registry, resolver, {}, options));
        if (!prepared.plan || !prepared.rejected.empty()) {
            fail(ErrorCode::SourceBinding, "S3 required observations were not fully admitted");
        }
        auto plan = prepared.plan;
        auto host = take(HostedSimulation::create(data, plan, build, std::move(suite),
                                                  std::move(decisions), types.price,
                                                  std::move(executions), std::optional{ratio}));
        trace->financial = host.financial();
        std::vector<lx::RequiredEvidenceSink> extra;
        extra.push_back(make_sink(options.evidence[1], data.account(),
                                  [trace] { return serialize_trace(*trace); }));
        extra.push_back(make_sink(options.evidence[2], data.account(), [trace, cfg, data, begin] {
            return summary_json(*trace, cfg, data, begin);
        }));
        extra.push_back(
            make_sink(options.evidence[3], data.account(), [text = cfg.xml] { return text; }));
        extra.push_back(make_sink(options.evidence[4], data.account(), [xml] { return xml; }));
        extra.push_back(
            make_report_sink(options.evidence[5], data.account(), [](const lx::RunReport &report) {
                std::ostringstream out;
                out << "id,physical,advances,publications,aliases,waiting,cached_deliveries\n";
                for (const auto &p : report.producers) {
                    out << p.id << ',' << p.physical << ',' << p.advances << ',' << p.publications
                        << ',' << p.aliases << ',' << p.waiting << ',' << p.cached_deliveries
                        << '\n';
                }
                return out.str();
            }));
        extra.push_back(make_sink(options.evidence[6], data.account(), [plan] {
            std::ostringstream out;
            out << "id,digest\n";
            for (std::uint32_t i = 0; i < plan->nodes().size(); ++i) {
                out << plan->nodes()[i].spec.id << ',' << hex(plan->identity(i)) << '\n';
            }
            return out.str();
        }));
        extra.push_back(
            make_sink(options.evidence[7], data.account(), [data, cfg, begin, rejection] {
                std::ostringstream out;
                out << "quadeplus_stochastic_input_v1\nmanifest=" << data.manifest_sha256()
                    << "\nprices=" << data.prices_sha256()
                    << "\nsessions=" << data.sessions_sha256() << "\nprofile=" << cfg.profile
                    << "\nrejection_policy=" << rejection << "\nmax_price_raw=" << cfg.max_price_raw
                    << "\nnumerical_begin=0\nnumerical_end=" << data.rows().size()
                    << "\nobserver_begin=4\nobserver_end=" << data.rows().size()
                    << "\nfunding_key=" << begin - 1 << "\nevaluation_begin=" << begin
                    << "\nevaluation_end=" << data.rows().size()
                    << "\nterminal=" << terminal_name(cfg.terminal)
                    << "\ndividend_policy=" << dividend_profile << '\n';
                return out.str();
            }));
        auto report_sinks = take(
            bind_report_sinks(reports, host.financial(), std::move(report_suite), data,
                              {run_id, cfg.profile, "previously_inspected_research_not_holdout",
                               "native S3; dataset=" + std::string{data.manifest_sha256()}}));
        for (auto &report : report_sinks) {
            extra.push_back(std::move(report));
        }
        std::vector<std::string> producers;
        for (const auto &node : plan->nodes()) {
            producers.push_back(node.spec.id);
        }
        auto retention = engine(lx::FileEvidence::create(
            plan->evidence_specification(), run_id, std::string{qr::clock_profile},
            std::move(producers), data.account(), {65536, 8192, 2048 * mb}));
        auto instance = engine(
            lx::instantiate(plan, take(host.services(retention.callbacks(), std::move(extra)))));
        const auto advance_start = Clock::now();
        auto work = detail::guard([&]() -> BacktestResult<void> {
            for (std::uint64_t key = 0;; ++key) {
                if (key < n) {
                    take(host.before_open(key));
                }
                const auto receipt = engine(lx::advance_one(instance));
                take(host.after_step(receipt));
                if (receipt.status == lx::StepStatus::Exhausted) {
                    break;
                }
            }
            return ok();
        });
        const auto finish_start = Clock::now();
        if (std::holds_alternative<std::monostate>(work)) {
            work = host.finalize_financial();
        }
        if (const auto *error = std::get_if<BacktestError>(&work)) {
            engine(lx::fail_host(
                instance, lx::diagnostic(lx::FailureCode::CallbackException, error->text())));
        }
        auto report = engine(lx::finish(instance, host.completion()));
        const auto ended = Clock::now();
        const auto us = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };
        // Required files are finalized by finish, before its acceptance marker.
        return ApplicationRun{{host.financial(), std::move(report)},
                              trace->summaries,
                              us(started, advance_start),
                              us(advance_start, finish_start),
                              us(finish_start, ended),
                              us(started, ended)};
    });
}
} // namespace quant::backtest::app
