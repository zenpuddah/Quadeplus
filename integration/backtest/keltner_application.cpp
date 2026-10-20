#include "application.hpp"
#include "report_evidence.hpp"
#include "quant/strategies/features.hpp"
#include "quant/strategies/policies.hpp"
#include "quant/research/daily_source.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "file_evidence.hpp"
#include "file_result_store.hpp"
#include "native_files.hpp"
#include "xml_frontend.hpp"
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
template <class T> T engine(std::variant<T, lx::Diagnostic> v) {
    if (auto *e = std::get_if<lx::Diagnostic>(&v)) {
        fail(ErrorCode::SourceBinding, e->text());
    }
    return std::get<T>(std::move(v));
}
constexpr std::pair<unsigned, unsigned> widths[]{{3, 2}, {2, 1}, {5, 2}, {2, 1}};
lx::MetadataLimits metadata() {
    return {{64 * mb, 1048576, 64, 4096, 512 * mb}, 128 * mb};
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
std::string graph_xml(const qr::DailyDataset &data, const Config &config,
                      const std::filesystem::path &out, const lx::LocalCacheConfiguration *cache) {
    const auto n = data.rows().size();
    const std::string clock{qr::clock_profile};
    std::ostringstream x;
    x << "<composition version=\"1\"><run id=\"keltner-fresh\" clock=\"" << clock
      << "\" driver=\"hosted\" execution=\"synchronous\" sharing=\"" << config.sharing
      << "\" failure=\"stop\" trace=\"steps\" retention_warning_steps=\"128\" retention_log_path=\""
      << escape_xml((out / "retention.log").string())
      << "\" retention_log_format=\"key_value_lines_v1\" retention_log_open=\"create_new\" "
         "retention_log_flush=\"on_event\" retention_log_failure=\"stop\" "
         "retention_log_overflow=\"stop\" retention_log_max_bytes=\"536870912\" "
         "retention_log_max_events=\"8388608\"/>"
      << "<limits xml_bytes=\"1048576\" xml_depth=\"32\" xml_elements=\"4096\" "
         "string_bytes=\"4096\" nodes=\"64\" edges=\"128\" requests=\"64\" source_updates=\""
      << n
      << "\" managed_buffer_bytes=\"8589934592\" trace_events=\"65536\" "
         "trace_message_bytes=\"256\"/><plugins/>";
    if (cache) {
        x << "<cache mode=\"automatic_read_write\" backend=\"local_files\" directory=\""
          << escape_xml(cache->directory)
          << "\" matching=\"exact_full_scope\" trust=\"same_build_local_writer\" "
             "integrity=\"sha256\" writer=\"single_process_lock\" corruption=\"fail_preparation\" "
             "capacity=\"bypass_new_entries\" cleanup=\"explicit_only\" max_entries=\"128\" "
             "max_store_bytes=\"268435456\" max_entry_bytes=\"16777216\" "
             "max_decoded_bytes=\"67108864\"/>";
    } else {
        x << "<cache mode=\"disabled\"/>";
    }
    x << "<sources><source id=\"s\" operation=\"quadeplus.daily_research\" semantic_version=\"1\" "
         "implementation_id=\"sealed-daily-source-v1\" source_version=\""
      << data.manifest_sha256() << "\" clock=\"" << clock << "\" begin_step=\"0\" end_step=\"" << n
      << "\" completeness=\"complete_only\" empty=\"reject\" missing=\"fail\" "
         "missing_intervals=\"strict\" availability=\""
      << clock
      << "\"/></sources><nodes><node id=\"close\" operation=\"qaedalus.bar_close\" "
         "semantic_version=\"1\" implementation_id=\"builtin-v1\" alignment=\"same_step\" "
         "initialization=\"stateless\"><input port=\"bar\" producer=\"s\" output=\"bar\" "
         "prior_outputs=\"0\" missing_intervals=\"strict\"/></node>";
    const auto input = [&](std::string port, std::string producer, std::string output,
                           unsigned history) {
        x << "<input port=\"" << port << "\" producer=\"" << producer << "\" output=\"" << output
          << "\" prior_outputs=\"" << history << "\" missing_intervals=\"strict\"/>";
    };
    for (unsigned i = 0; i < 4; ++i) {
        auto p = "t" + std::to_string(i) + "_";
        for (const auto kind : {"ema20", "true_range", "atr10", "upper_channel"}) {
            const bool stateless = std::string_view{kind} == "upper_channel";
            x << "<node id=\"" << p << kind << "\" operation=\"quadeplus." << kind
              << "\" semantic_version=\"1\" implementation_id=\"keltner-v1\" "
                 "alignment=\"same_step\" initialization=\""
              << (stateless ? "stateless" : "empty_at_step_0")
              << "\"><param name=\"max_input_raw\" value=\"" << config.max_price_raw
              << "\"/><param name=\"rounding\" value=\"nearest_ties_even\"/><param "
                 "name=\"overflow\" value=\"fail\"/>";
            if (std::string_view{kind} == "ema20") {
                x << "<param name=\"seed\" value=\"mean_first_20\"/><param name=\"recurrence\" "
                     "value=\"2_current_19_previous_over_21\"/>";
                input("price", "close", "price", 0);
            } else if (std::string_view{kind} == "true_range") {
                x << "<param name=\"seed\" value=\"first_high_low\"/>";
                input("bar", "s", "bar", 1);
            } else if (std::string_view{kind} == "atr10") {
                x << "<param name=\"seed\" value=\"mean_first_10\"/><param name=\"recurrence\" "
                     "value=\"9_previous_current_over_10\"/>";
                input("price", p + "true_range", "price", 0);
            } else {
                x << "<param name=\"numerator\" value=\"" << widths[i].first
                  << "\"/><param name=\"denominator\" value=\"" << widths[i].second << "\"/>";
                input("ema", p + "ema20", "price", 0);
                input("atr", p + "atr10", "price", 0);
            }
            x << "</node>";
        }
    }
    x << "</nodes><observations>";
    for (unsigned i = 0; i < 4; ++i) {
        const auto prefix = "t" + std::to_string(i);
        for (const auto name : {"close", "tr", "ema", "atr", "upper"}) {
            const std::string s = name;
            const auto producer = s == "close" ? "close"
                                               : prefix + "_" +
                                                     (s == "tr"    ? "true_range"
                                                      : s == "ema" ? "ema20"
                                                      : s == "atr" ? "atr10"
                                                                   : "upper_channel");
            const auto begin = s == "ema" || s == "upper" ? 19 : s == "atr" ? 9 : 0;
            x << "<observe id=\"" << prefix << '.' << name << "\" producer=\"" << producer
              << "\" output=\"price\" begin_step=\"" << begin << "\" end_step=\"" << n
              << "\" absence=\"require_value\" prior_outputs=\"0\" missing_intervals=\"strict\"/>";
        }
    }
    x << "</observations></composition>";
    return x.str();
}
struct Trace {
    lx::Reservation charge;
    std::vector<std::string> rows;
    std::uint64_t max_rows;
    std::array<qs::KeltnerState, 4> states;
    std::array<std::optional<Action>, 4> last_entry;
    std::shared_ptr<const FinancialRun> financial;
    std::shared_ptr<std::vector<Summary>> summaries = std::make_shared<std::vector<Summary>>();
    void append(std::string row) {
        if (rows.size() >= max_rows) {
            fail(ErrorCode::ResourceExceeded, "Keltner trace record limit");
        }
        rows.push_back(std::move(row));
    }
};
std::string raw(std::optional<data::Price> v) {
    return v ? std::to_string(v->raw()) : "";
}
std::string summaries(Trace &t, const Config &cfg, const qr::DailyDataset &data,
                      std::uint64_t begin) {
    if (!t.financial->finalized) {
        return "{\"financial_state\":\"partial\",\"trials\":[]}\n";
    }
    std::ostringstream out;
    out << "{\"format\":\"quadeplus_keltner_summary_v1\",\"financial_state\":\"completed\","
           "\"dataset_sha256\":\""
        << data.manifest_sha256() << "\",\"evaluation_sessions\":" << data.rows().size() - begin
        << ",\"trials\":[";
    for (std::size_t i = 0; i < t.financial->trials.size(); ++i) {
        const auto &trial = t.financial->trials[i];
        const auto &a = *trial.account;
        std::int64_t peak = cfg.initial_cash, loss = 0, loss_peak = peak, equity = cfg.initial_cash;
        for (const auto &e : t.financial->events) {
            if (e.trial != a.trial) {
                continue;
            }
            if (e.kind == "final" && e.equity) {
                equity = *e.equity;
            }
            if (e.kind == "close" && e.key >= begin && e.equity) {
                peak = std::max(peak, *e.equity);
                if (take(ratio_greater(peak - *e.equity, peak, loss, loss_peak))) {
                    loss = peak - *e.equity;
                    loss_peak = peak;
                }
            }
        }
        const auto name = i < 4
                              ? "keltner_" + std::to_string(widths[i].first) + "_" +
                                    std::to_string(widths[i].second) + (i == 3 ? "_duplicate" : "")
                          : i == 4 ? "cash_usd_no_interest_v1"
                                   : "buy_hold95_next_open_v1";
        Summary s{a.trial,
                  name,
                  a.cash.amount(),
                  a.units.raw(),
                  equity,
                  detail::sub(equity, cfg.initial_cash),
                  a.realized.amount(),
                  detail::sub(detail::sub(equity, a.cash.amount()), a.basis.amount()),
                  a.dividends.amount(),
                  a.fees.amount(),
                  loss,
                  loss_peak};
        t.summaries->push_back(s);
        if (i) {
            out << ',';
        }
        out << "{\"id\":" << a.trial << ",\"strategy\":\"" << name << "\",\"cash_cents\":" << s.cash
            << ",\"units_raw\":" << s.units << ",\"equity_cents\":" << s.equity
            << ",\"pnl_cents\":" << s.pnl << ",\"realized_cents\":" << s.realized
            << ",\"unrealized_cents\":" << s.unrealized << ",\"dividend_cents\":" << s.dividends
            << ",\"fee_cents\":" << s.fees << ",\"drawdown_loss_cents\":" << loss
            << ",\"drawdown_peak_cents\":" << loss_peak
            << ",\"risk_latched\":" << (trial.risk->latched ? "true" : "false") << '}';
    }
    out << "]}\n";
    return out.str();
}
} // namespace
BacktestResult<ApplicationRun> run_keltner(const qr::DailyDataset &data, const Config &cfg,
                                           const std::filesystem::path &out,
                                           const lx::HostBuild &build) noexcept {
    return detail::guard([&]() -> BacktestResult<ApplicationRun> {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        require_new_output(out);
        const auto n = data.rows().size();
        const auto &ds = data.specification();
        if (n < 20 || n > 8000 || ds.symbol != cfg.symbol || ds.numerical_origin != cfg.origin ||
            ds.evaluation_begin != cfg.begin || ds.evaluation_end != cfg.end) {
            fail(ErrorCode::InvalidConfig, "Keltner config/dataset scope mismatch");
        }
        const auto pos = std::find_if(data.rows().begin(), data.rows().end(),
                                      [&](const auto &r) { return r.session == cfg.begin; });
        const auto begin = static_cast<std::uint64_t>(pos - data.rows().begin());
        if (!begin || begin >= n) {
            fail(ErrorCode::Coverage, "Keltner requires a preceding funding close");
        }
        if (!cfg.cache_directory.empty() && n > 512) {
            fail(ErrorCode::ResourceExceeded,
                 "Native snapshot/reuse profile admits at most 512 source rows");
        }
        if (!std::filesystem::create_directory(out)) {
            fail(ErrorCode::IO, "new Keltner output directory required");
        }
        auto registry = engine(lx::Registry::create());
        auto types = engine(quant::lexongine::register_components(
            registry, {build.identity(), {}, lx::MissingIntervalPolicy::Strict}));
        engine(qs::register_keltner_features(
            registry, types, {build.identity(), {}, lx::MissingIntervalPolicy::Strict}));
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
                                      {build.identity(), {}, lx::MissingIntervalPolicy::Strict}));
        engine(registry.freeze());
        engine(resolver.freeze());
        auto charge = engine(data.account()->reserve(lx::ResourceKind::Graph, 64 * mb + n * 16384));
        auto trace = std::make_shared<Trace>();
        trace->charge = std::move(charge);
        trace->max_rows = 8 * n + 32;
        trace->rows.reserve(trace->max_rows);
        trace->summaries->reserve(6);
        const HostLimits limits{6, 2 * n + 16, 32 * n + 64, 32 * mb};
        SuiteConfiguration suite{
            begin - 1, begin,  n,
            false,     limits, financial_evidence_spec((out / "financial.txt").string(), limits),
            {}};
        auto cash = data::Money::from_raw(data::CurrencyCode{"USD"}, cfg.initial_cash);
        auto lot = data::Quantity::from_raw(cfg.lot);
        std::vector<DecisionCallback> decisions;
        std::vector<ExecutionCallback> executions;
        for (unsigned i = 0; i < 4; ++i) {
            const auto prefix = "t" + std::to_string(i) + ".";
            trace->states[i].prefix = prefix;
            trace->states[i].costs = cfg.costs;
            suite.trials.push_back({i + 1,
                                    "keltner_atr_risk_v1",
                                    "width=" + std::to_string(widths[i].first) + "/" +
                                        std::to_string(widths[i].second) +
                                        ";risk=1/200;stop=2atr;entry_guard=v1",
                                    cash,
                                    lot,
                                    cfg.costs,
                                    {{2000}, {}},
                                    TerminalMode::Mark,
                                    {{prefix + "close", 0, n},
                                     {prefix + "tr", 0, n},
                                     {prefix + "ema", 19, n},
                                     {prefix + "atr", 9, n},
                                     {prefix + "upper", 19, n}}});
            decisions.push_back(
                [trace, i,
                 prefix](const DecisionContext &context, const Account *account,
                         std::span<const PriceFeature> features) -> BacktestResult<Action> {
                    return detail::guard([&]() -> BacktestResult<Action> {
                        auto action = take(
                            qs::keltner_decision(trace->states[i], context, account, features));
                        if (action.kind == ActionKind::FixedEntry) {
                            trace->last_entry[i] = action;
                        }
                        std::array<std::optional<data::Price>, 5> values;
                        const std::string names[]{"close", "tr", "ema", "atr", "upper"};
                        for (const auto &f : features) {
                            for (unsigned j = 0; j < 5; ++j) {
                                if (f.request == prefix + names[j]) {
                                    values[j] = f.value;
                                }
                            }
                        }
                        const auto &s = trace->states[i];
                        std::ostringstream row;
                        row << i + 1 << ',' << context.key << ",decision," << context.may_submit
                            << ',' << context.funded;
                        for (auto v : values) {
                            row << ',' << raw(v);
                        }
                        row << ',' << static_cast<int>(action.kind) << ',' << action.reason << ','
                            << action.quantity.raw() << ',' << raw(action.initial_stop_distance)
                            << ',' << s.held << ',' << raw(s.threshold) << ','
                            << raw(s.highest_close) << '\n';
                        trace->append(row.str());
                        return action;
                    });
                });
            executions.push_back(
                [trace, i](const FillQuote &quote, const Account &account) -> BacktestResult<void> {
                    return detail::guard([&]() -> BacktestResult<void> {
                        take(qs::keltner_execution(trace->states[i], quote, account));
                        std::uint64_t key = account.last_event->key;
                        std::int64_t quantity = 0;
                        std::optional<data::Price> distance;
                        std::string reason;
                        if (const auto *fill = std::get_if<Fill>(&quote)) {
                            key = fill->mark.key();
                            quantity = fill->quantity.raw();
                            reason = fill->reason;
                            if (fill->intent) {
                                if (const auto *fixed =
                                        std::get_if<BuyFixedQuantity>(&fill->intent->order)) {
                                    distance = fixed->initial_stop_distance;
                                }
                            }
                        } else {
                            reason = std::to_string(
                                static_cast<int>(std::get<OrderRejection>(quote).reason));
                            if (trace->last_entry[i]) {
                                quantity = trace->last_entry[i]->quantity.raw();
                                distance = trace->last_entry[i]->initial_stop_distance;
                            }
                        }
                        const auto &s = trace->states[i];
                        std::ostringstream row;
                        row << i + 1 << ',' << key << ",execution,,,,,,,,," << reason << ','
                            << quantity << ',' << raw(distance) << ',' << s.held << ','
                            << raw(s.threshold) << ',' << raw(s.highest_close) << '\n';
                        trace->append(row.str());
                        trace->last_entry[i].reset();
                        return ok();
                    });
                });
        }
        for (unsigned i = 4; i < 6; ++i) {
            suite.trials.push_back({i + 1,
                                    i == 4 ? "cash_usd_no_interest_v1" : "buy_hold95_next_open_v1",
                                    i == 4 ? "cash_only" : "first_evaluation_close;single_entry",
                                    cash,
                                    lot,
                                    cfg.costs,
                                    {},
                                    TerminalMode::Mark,
                                    {}});
            decisions.push_back([i, begin](const DecisionContext &c, const Account *,
                                           auto) -> BacktestResult<Action> {
                return Action{i == 5 && c.may_submit && c.key == begin ? ActionKind::TargetLong
                                                                       : ActionKind::Hold,
                              data::Quantity::from_raw(0),
                              i == 4 ? "cash_control" : "buy_hold_control"};
            });
            executions.emplace_back();
        }
        std::optional<lx::FileResultStore> store;
        std::optional<lx::LocalCacheConfiguration> cache;
        if (!cfg.cache_directory.empty()) {
            cache = lx::LocalCacheConfiguration{"automatic_read_write",
                                                "local_files",
                                                cfg.cache_directory.string(),
                                                "exact_full_scope",
                                                "same_build_local_writer",
                                                "sha256",
                                                "single_process_lock",
                                                "fail_preparation",
                                                "bypass_new_entries",
                                                "explicit_only",
                                                128,
                                                256 * mb,
                                                16 * mb,
                                                64 * mb};
            lx::FileStoreLimits bounds{{{4 * mb, 4096, mb, 16 * mb}, mb, 65536, 65536, 8192, 64},
                                       {{4 * mb, 262144, 64, 4096, 32 * mb}, 16 * mb},
                                       {16 * mb, 1048576, 4096, 8192, 64, 64 * mb, 128 * mb},
                                       2 * mb,
                                       1024,
                                       64 * mb,
                                       metadata()};
            store = engine(
                lx::FileResultStore::create(*cache, types.price, build, resources(true), bounds));
        }
        auto xml = graph_xml(data, cfg, out, cache ? &*cache : nullptr);
        auto parsed = engine(lx::parse_xml(xml, {mb, 32, 16384, 4096, data.account()}));
        auto composition = engine(lx::resolve_composition(parsed, registry));
        const auto events = 512 * n + 16384;
        lx::PrepareOptions options{
            "keltner-fresh",
            take(source.request({std::string{qr::clock_profile}, 0, n})),
            {data.account(),
             {{n, n, 0, n}, 4096, 32 * mb, 32 * mb, mb},
             lx::MissingIntervalPolicy::Strict,
             {data.owned_bytes(), mb, 0, 8192, sizeof(data::Bar) + 512, 64 * mb, 0}},
            {{0, 32 * mb, 16 * mb, 128 * mb, 64 * mb, 64 * mb,
              events * sizeof(lx::RetentionEvent) + 32 * mb},
             16384,
             8192,
             events,
             128},
            {mb, 32 * mb, 16 * mb, 16 * mb, 32 * mb, 65536, mb},
            8 * mb,
            16 * mb,
            [](std::span<const std::byte> bytes) { return lx::files::digest(bytes); },
            {}};
        options.evidence = {
            suite.financial_evidence,
            evidence_spec("keltner", "keltner_state_csv_v1", out, "keltner.csv", 16 * mb,
                          8 * n + 32),
            evidence_spec("summary", "keltner_summary_json_v1", out, "summary.json", mb, 6),
            evidence_spec("producers", "producer_counts_csv_v1", out, "producers.csv", mb, 64),
            evidence_spec("graph", "composition_xml_v1", out, "graph.xml", mb, 1),
            evidence_spec("config", "domain_xml_v1", out, "effective-config.xml", 65536, 1),
            evidence_spec("identities", "computation_ids_csv_v1", out, "identities.csv", mb, 64)};
        const std::array<std::uint32_t, 6> report_ids{1, 2, 3, 4, 5, 6};
        auto reports = take(report_specs(out, report_ids, n - begin, 16 * mb));
        options.evidence.insert(options.evidence.end(), reports.required.begin(),
                                reports.required.end());
        auto report_suite = suite;
        if (store) {
            options.evidence.push_back(evidence_spec("snapshot", "run_snapshot_v1", out,
                                                     "snapshot.xml", 64 * mb, 24 * n + 32));
        }
        options.evidence_account = data.account();
        options.evidence_resources = {0, 0, 2048 * mb, 64 * mb, 0, 256 * mb, 64 * mb};
        auto result = engine(lx::prepare(composition, registry, resolver,
                                         store ? store->service() : nullptr, options));
        if (!result.plan || !result.rejected.empty()) {
            fail(ErrorCode::SourceBinding, "Keltner required observations were not fully admitted");
        }
        auto plan = result.plan;
        auto host =
            take(HostedSimulation::create(data, plan, build, std::move(suite), std::move(decisions),
                                          types.price, std::move(executions)));
        trace->financial = host.financial();
        std::vector<lx::RequiredEvidenceSink> extra;
        extra.push_back(make_sink(options.evidence[1], data.account(), [trace] {
            std::string out = "trial,key,kind,may_submit,funded,close_raw,tr_raw,ema_raw,atr_raw,"
                              "upper_raw,action,reason,quantity_raw,stop_distance_raw,held,"
                              "threshold_raw,highest_close_raw\n";
            for (const auto &row : trace->rows) {
                out += row;
            }
            return out;
        }));
        extra.push_back(make_sink(options.evidence[2], data.account(), [trace, cfg, data, begin] {
            return summaries(*trace, cfg, data, begin);
        }));
        extra.push_back(
            make_report_sink(options.evidence[3], data.account(), [](const lx::RunReport &report) {
                std::ostringstream out;
                out << "id,physical,advances,publications,aliases,waiting,cached_deliveries\n";
                for (const auto &p : report.producers) {
                    out << p.id << ',' << p.physical << ',' << p.advances << ',' << p.publications
                        << ',' << p.aliases << ',' << p.waiting << ',' << p.cached_deliveries
                        << '\n';
                }
                return out.str();
            }));
        extra.push_back(make_sink(options.evidence[4], data.account(), [xml] { return xml; }));
        extra.push_back(
            make_sink(options.evidence[5], data.account(), [text = cfg.xml] { return text; }));
        extra.push_back(make_sink(options.evidence[6], data.account(), [plan] {
            std::ostringstream out;
            out << "id,digest\n";
            for (std::uint32_t i = 0; i < plan->nodes().size(); ++i) {
                out << plan->nodes()[i].spec.id << ',' << lx::wire::hex(plan->identity(i)) << '\n';
            }
            return out.str();
        }));
        auto report_sinks = take(bind_report_sinks(
            reports, host.financial(), std::move(report_suite), data,
            {"keltner-fresh", cfg.profile, "previously_inspected_research_not_holdout",
             "fixed width demonstrations; dataset=" + std::string{data.manifest_sha256()}}));
        for (auto &report : report_sinks) {
            extra.push_back(std::move(report));
        }
        if (store) {
            extra.push_back(
                engine(lx::SnapshotEvidence::create(plan, options.evidence.back(), data.account(),
                                                    {metadata(), 32 * mb, 16384}))
                    .callbacks());
        }
        std::vector<std::string> producers;
        for (const auto &node : plan->nodes()) {
            producers.push_back(node.spec.id);
        }
        auto retention = engine(lx::FileEvidence::create(
            plan->evidence_specification(), "keltner-fresh", std::string{qr::clock_profile},
            std::move(producers), data.account(), {65536, 8192, 2048 * mb}));
        auto instance = engine(
            lx::instantiate(plan, take(host.services(retention.callbacks(), std::move(extra)))));
        const auto advance_start = Clock::now();
        auto work = detail::guard([&]() -> BacktestResult<void> {
            for (std::uint64_t key = 0;; ++key) {
                if (key < n) {
                    take(host.before_open(key));
                }
                auto receipt = engine(lx::advance_one(instance));
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
        if (auto *error = std::get_if<BacktestError>(&work)) {
            engine(lx::fail_host(
                instance, lx::diagnostic(lx::FailureCode::CallbackException, error->text())));
        }
        auto report = engine(lx::finish(instance, host.completion()));
        const auto end = Clock::now();
        const auto us = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };
        return ApplicationRun{
            {host.financial(), std::move(report)}, trace->summaries,      us(start, advance_start),
            us(advance_start, finish_start),       us(finish_start, end), us(start, end)};
    });
}
} // namespace quant::backtest::app
