#include "application.hpp"
#include "host_identity.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/research/daily_source.hpp"
#include "quant/strategies/stochastic.hpp"
#include "quant/storage/file_integrity.hpp"
#include "file_evidence.hpp"
#include "native_files.hpp"
#include "xml_frontend.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace lx = ::lexongine;
namespace qb = quant::backtest;
namespace qr = quant::research;
namespace qs = quant::strategies;
namespace qd = quant::data;
namespace app = quant::backtest::app;
constexpr std::uint64_t mb = 1048576;

template <class T, class E> T take(std::variant<T, E> value) {
    if (const auto *error = std::get_if<E>(&value)) {
        throw std::runtime_error(std::string{error->text()});
    }
    return std::get<T>(std::move(value));
}
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
qd::Money money(std::int64_t cents) {
    return qd::Money::from_raw(qd::CurrencyCode{"USD"}, cents);
}
std::shared_ptr<lx::ResourceAccount> resources() {
    return take(lx::ResourceAccount::create(
        {{128 * mb, 16 * mb, 512 * mb, 64 * mb, 16 * mb, 256 * mb, 64 * mb},
         8192,
         4096,
         32768,
         128}));
}

qr::DailyDataset fixture(const std::filesystem::path &path, std::size_t count) {
    require(count == 6 || count == 8, "fixture requires six or eight authored Bars");
    constexpr std::array<std::string_view, 8> dates{"2026-01-02", "2026-01-05", "2026-01-06",
                                                    "2026-01-07", "2026-01-08", "2026-01-09",
                                                    "2026-01-12", "2026-01-13"};
    std::ostringstream prices, sessions;
    prices << "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n";
    sessions << "Date\n";
    for (std::size_t key = 0; key < count; ++key) {
        const auto close = key < 4 ? 100 : 60;
        prices << dates[key] << ",100,150,50," << close << ",1," << close << ",0\n";
        sessions << dates[key] << '\n';
    }
    const auto csv = prices.str(), grid = sessions.str();
    quant::storage::files::write_text(path / "prices.csv", csv);
    quant::storage::files::write_text(path / "sessions.csv", grid);
    qr::DatasetSpec spec{"stochastic-host-fixture-v1",
                         "TEST",
                         "USD",
                         "authored-stochastic-host-boundaries",
                         1,
                         1,
                         1,
                         "synthetic_session_grid_v1",
                         std::string{qr::adjustment_profile},
                         std::string{qr::decimal_profile},
                         std::string{qr::volume_profile},
                         std::string{qr::clock_profile},
                         std::string{dates[0]},
                         std::string{dates[4]},
                         std::string{dates[count - 1]}};
    return take(qr::prepare_daily_dataset({path / "prices.csv",
                                           path / "sessions.csv",
                                           path / "bundle",
                                           quant::storage::files::sha256(csv),
                                           quant::storage::files::sha256(grid),
                                           spec,
                                           {mb, 8, 64},
                                           resources()}));
}

struct Receipt {
    std::uint64_t key, id;
    bool rejected, terminal;
    std::optional<qd::Side> side;
    std::int64_t units;
    bool parity_failed;
};
struct Trace {
    qs::StochasticState strategy;
    std::shared_ptr<const qb::FinancialRun> financial;
    std::vector<std::string> rows;
    std::vector<Receipt> receipts;
    std::vector<std::uint64_t> ratio_keys, long_keys;
    std::uint64_t decisions = 0, opens = 0, finalizations = 0;
    bool rejection_unchanged = false;
    std::string serialized;
    void append(std::string row) {
        require(rows.size() < 32 && row.size() < 512,
                "test state evidence exceeded its bounded journal");
        rows.push_back(std::move(row));
    }
};

struct Environment {
    std::filesystem::path path;
    qr::DailyDataset data;
    lx::HostBuild build;
    lx::Registry registry = take(lx::Registry::create());
    quant::lexongine::ComponentTypes types;
    lx::RegisteredType<qs::StochasticValue> ratio;
    lx::SourceResolver resolver;
    qr::RegisteredDailySource source;

    static std::filesystem::path create(const std::filesystem::path &root, std::string_view name) {
        const auto path = root / name;
        require(std::filesystem::create_directory(path), "test case directory must be new");
        return path;
    }
    Environment(const std::filesystem::path &root, std::string_view name, std::size_t count,
                const lx::HostBuild &inspected)
        : path(create(root, name)), data(fixture(path, count)), build(inspected),
          types(take(quant::lexongine::register_components(
              registry, {build.identity(), {}, lx::MissingIntervalPolicy::Strict}))),
          ratio(take(qs::register_stochastic(
              registry, types, {build.identity(), {}, lx::MissingIntervalPolicy::Strict}))),
          resolver(take(lx::SourceResolver::create(data.account(), 65536, 4))),
          source(take(qr::register_daily_source(
              registry, resolver, types.bar, data,
              {{count, data.owned_bytes(), 262144, 65536, 1024},
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
              {build.identity(), {}, lx::MissingIntervalPolicy::Strict}))) {
        take(registry.freeze());
        take(resolver.freeze());
    }
    lx::RequiredEvidenceSpec state_spec() const {
        return {"stochastic_state",
                "stochastic_host_state_csv_v1",
                (path / "state.csv").string(),
                "create_new",
                65536,
                32,
                "on_close",
                "stop"};
    }
    qb::SuiteConfiguration suite(bool legacy, std::int64_t initial,
                                 qb::TerminalMode terminal) const {
        const auto n = static_cast<std::uint64_t>(data.rows().size());
        const qb::HostLimits limits{1, 32, 128, mb};
        // This generic-host fixture deliberately bypasses app::run_stochastic's
        // pinned historical SPY profile guard. Production CLI policy is unchanged.
        return {
            3,
            4,
            n,
            legacy,
            limits,
            qb::financial_evidence_spec((path / "financial.txt").string(), limits),
            {{1,
              std::string{qs::stochastic_strategy_id},
              std::string{"authored_host_boundary;profile="} + (legacy ? "legacy" : "fresh") +
                  ";rejection=" +
                  std::string{legacy ? qs::stochastic_legacy_policy : qs::stochastic_fresh_policy},
              money(initial),
              qd::Quantity::from_raw(1),
              {5, 5},
              {},
              terminal,
              {{"K", 4, n, qb::FeatureKind::Ratio}}}}};
    }
    std::shared_ptr<const lx::PreparedPlan> prepare(const qb::SuiteConfiguration &suite) {
        const auto n = data.rows().size();
        const std::string clock{qr::clock_profile};
        std::ostringstream xml;
        xml << "<composition version=\"1\"><run id=\"stochastic-host-test\" clock=\"" << clock
            << "\" driver=\"hosted\" execution=\"synchronous\" sharing=\"exact\" failure=\"stop\" "
               "trace=\"steps\""
            << " retention_warning_steps=\"128\" retention_log_path=\""
            << app::escape_xml((path / "retention.log").string())
            << "\" retention_log_format=\"key_value_lines_v1\" retention_log_open=\"create_new\" "
               "retention_log_flush=\"on_event\""
            << " retention_log_failure=\"stop\" retention_log_overflow=\"stop\" "
               "retention_log_max_bytes=\"4194304\" retention_log_max_events=\"32768\"/>"
            << "<limits xml_bytes=\"1048576\" xml_depth=\"16\" xml_elements=\"1024\" "
               "string_bytes=\"4096\" nodes=\"32\" edges=\"64\""
            << " requests=\"16\" source_updates=\"" << n
            << "\" managed_buffer_bytes=\"536870912\" trace_events=\"4096\" "
               "trace_message_bytes=\"256\"/>"
            << "<plugins/><cache mode=\"disabled\"/><sources><source id=\"s\" "
               "operation=\"quadeplus.daily_research\" semantic_version=\"1\""
            << " implementation_id=\"sealed-daily-source-v1\" source_version=\""
            << data.manifest_sha256() << "\" clock=\"" << clock << "\" begin_step=\"0\" end_step=\""
            << n << "\" completeness=\"complete_only\" empty=\"reject\" missing=\"fail\""
            << " missing_intervals=\"strict\" availability=\"" << clock << "\"/></sources><nodes>"
            << "<node id=\"close\" operation=\"qaedalus.bar_close\" semantic_version=\"1\" "
               "implementation_id=\"builtin-v1\" alignment=\"same_step\" "
               "initialization=\"stateless\">"
            << "<input port=\"bar\" producer=\"s\" output=\"bar\" prior_outputs=\"0\" "
               "missing_intervals=\"strict\"/></node>"
            << "<node id=\"stochastic\" operation=\"quadeplus.stochastic5\" semantic_version=\"1\" "
               "implementation_id=\"stochastic-v1\" alignment=\"same_step\" "
               "initialization=\"empty_at_step_0\">"
            << "<param name=\"window\" value=\"5\"/><param name=\"zero_range\" "
               "value=\"1/2\"/><param name=\"coverage\" value=\"strict_complete_bars\"/>"
            << "<input port=\"bar\" producer=\"s\" output=\"bar\" prior_outputs=\"0\" "
               "missing_intervals=\"strict\"/></node></nodes><observations>"
            << "<observe id=\"K\" producer=\"stochastic\" output=\"k\" begin_step=\"4\" end_step=\""
            << n
            << "\" absence=\"require_value\" prior_outputs=\"0\" missing_intervals=\"strict\"/>"
            << "<observe id=\"P\" producer=\"close\" output=\"price\" begin_step=\"0\" end_step=\""
            << n
            << "\" absence=\"require_value\" prior_outputs=\"0\" "
               "missing_intervals=\"strict\"/></observations></composition>";
        auto parsed = take(lx::parse_xml(xml.str(), {mb, 32, 16384, 4096, data.account()}));
        auto composition = take(lx::resolve_composition(parsed, registry));
        lx::PrepareOptions options{
            "stochastic-host-test",
            take(source.request({clock, 0, n})),
            {data.account(),
             {{n, n, 0, n}, 1024, 65536, 262144, 65536},
             lx::MissingIntervalPolicy::Strict,
             {data.owned_bytes(), 65536, 0, 8192, sizeof(qd::Bar) + 512, mb, 0}},
            {{0, mb, mb, 8 * mb, mb, 4 * mb, 16 * mb}, 4096, 4096, 32768, 128},
            {mb, mb, mb, mb, mb, 65536, mb},
            262144,
            mb,
            [](std::span<const std::byte> bytes) { return lx::files::digest(bytes); },
            {}};
        options.evidence = {suite.financial_evidence, state_spec()};
        options.evidence_account = data.account();
        options.evidence_resources = {0, 0, 128 * mb, mb, 0, 32 * mb, 16 * mb};
        auto prepared = take(lx::prepare(composition, registry, resolver, {}, options));
        require(prepared.plan && prepared.rejected.empty(),
                "hosted stochastic graph was not fully admitted");
        return prepared.plan;
    }
    lx::ExecutionEvidence retention(const std::shared_ptr<const lx::PreparedPlan> &plan) const {
        std::vector<std::string> producers;
        for (const auto &node : plan->nodes()) {
            producers.push_back(node.spec.id);
        }
        return take(lx::FileEvidence::create(plan->evidence_specification(), "stochastic-host-test",
                                             std::string{qr::clock_profile}, std::move(producers),
                                             data.account(), {65536, 8192, 16 * mb}))
            .callbacks();
    }
    std::vector<lx::RequiredEvidenceSink> evidence(const std::shared_ptr<Trace> &trace,
                                                   bool inject_failure = false) const {
        auto sink = app::make_sink(state_spec(), data.account(), [trace, inject_failure] {
            ++trace->finalizations;
            require(trace->financial != nullptr, "state sink lost its financial lifetime pin");
            if (inject_failure) {
                require(trace->financial->finalized && trace->financial->exhausted,
                        "injected failure ran before financial completion");
                qb::detail::fail(qb::ErrorCode::Integrity,
                                 "injected required stochastic state finalization failure");
            }
            std::string text = "kind,key,episode_start,entry_consumed,held_units,pending,parity_"
                               "failed,cash,units\n";
            for (const auto &row : trace->rows) {
                text += row;
            }
            trace->serialized = text;
            return text;
        });
        auto open = sink.open;
        sink.open = [trace, open]() -> lx::Result<void> {
            ++trace->opens;
            return open();
        };
        return {std::move(sink)};
    }
};

void append_state(Trace &trace, std::string_view kind, std::uint64_t key, const qb::Account *a) {
    const auto &s = trace.strategy;
    std::ostringstream row;
    row << kind << ',' << key << ',' << (s.episode_start ? std::to_string(*s.episode_start) : "")
        << ',' << s.entry_consumed << ',' << s.held_units << ',' << s.pending_side.has_value()
        << ',' << s.parity_failed << ',' << (a ? std::to_string(a->cash.amount()) : "") << ','
        << (a ? std::to_string(a->units.raw()) : "") << '\n';
    trace.append(row.str());
}
std::vector<qb::DecisionCallback> decisions(const std::shared_ptr<Trace> &trace,
                                            const qr::DailyDataset &data) {
    return {[trace,
             data](const qb::DecisionContext &c, const qb::Account *a,
                   std::span<const qb::PriceFeature> prices) -> qb::BacktestResult<qb::Action> {
        return qb::detail::guard([&]() -> qb::BacktestResult<qb::Action> {
            ++trace->decisions;
            require(prices.empty(),
                    "S3 received the globally observed Price instead of its filtered Ratio input");
            require(c.ratios.size() == (c.key < 4 ? 0U : 1U),
                    "S3 readiness/typed inbox filtering changed");
            require(c.funded == (a != nullptr) && c.funded == (c.key >= 3),
                    "hosted stochastic funding boundary changed");
            std::optional<qs::StochasticValue> k;
            if (!c.ratios.empty()) {
                const auto &f = c.ratios.front();
                require(f.request == "K" && f.key == c.key &&
                            f.available_at_ns == data.rows()[c.key].close_available_ns,
                        "stochastic callback observed a wrong/stale/unavailable ratio");
                require(f.value.denominator > 0 && f.value.numerator == f.value.denominator / 10 &&
                            f.value.denominator % 10 == 0,
                        "registered five-Bar kernel did not produce the authored K=10");
                k = f.value;
                trace->ratio_keys.push_back(c.key);
            }
            auto result = take(qs::stochastic_decision(trace->strategy, c, a, k));
            if (result.kind == qb::ActionKind::TargetLong) {
                trace->long_keys.push_back(c.key);
            }
            append_state(*trace, "decision", c.key, a);
            return result;
        });
    }};
}
std::vector<qb::ExecutionCallback> executions(const std::shared_ptr<Trace> &trace) {
    return {[trace](const qb::FillQuote &quote, const qb::Account &a) -> qb::BacktestResult<void> {
        return qb::detail::guard([&]() -> qb::BacktestResult<void> {
            require(trace->financial && trace->financial->trials.size() == 1,
                    "execution lacks the hosted financial ledger");
            const auto &t = trace->financial->trials[0];
            require(t.ledger && t.account && *t.account == a,
                    "execution received a different account snapshot");
            const auto *rejection = std::get_if<qb::OrderRejection>(&quote);
            const auto *fill = std::get_if<qb::Fill>(&quote);
            require(fill || a.last_event.has_value(),
                    "rejected order has no opening account location");
            const auto key = fill ? fill->mark.key() : a.last_event->key;
            if (rejection) {
                require(rejection->reason == qb::RejectionReason::BelowMinimumLot,
                        "wrong hosted rejection boundary");
                require(!t.ledger->rows().empty(), "opening dividend checkpoint missing");
                const auto &previous = t.ledger->rows().back();
                const auto *dividend = std::get_if<qb::DividendEvent>(&previous.event);
                // Zero-dividend postings legitimately advance account revision.
                // The rejected quote itself must match that exact pre-quote state.
                require(dividend && dividend->prior_close.key() == key && previous.after == a,
                        "rejected entry mutated account/revision or appended a fill");
                require(a.cash.amount() == 1 && a.units.raw() == 0 && a.basis.amount() == 0 &&
                            a.realized.amount() == 0 && a.dividends.amount() == 0 &&
                            a.fees.amount() == 0,
                        "rejected entry changed financial balances");
                trace->rejection_unchanged = true;
            } else {
                require(fill && !t.ledger->rows().empty(), "posted fill checkpoint missing");
                const auto *posted = std::get_if<qb::Fill>(&t.ledger->rows().back().event);
                require(posted && *posted == *fill,
                        "execution notification arrived before ledger posting");
            }
            auto notified = qs::stochastic_execution(trace->strategy, quote, a);
            trace->receipts.push_back({key, fill ? fill->id : rejection->id, rejection != nullptr,
                                       fill && fill->synthetic_terminal,
                                       fill ? std::optional{fill->side} : std::nullopt,
                                       a.units.raw(), trace->strategy.parity_failed});
            append_state(*trace,
                         rejection                  ? "rejected"
                         : fill->synthetic_terminal ? "terminal"
                                                    : "filled",
                         key, &a);
            // The diagnostic receipt is retained before legacy rejection becomes
            // a host failure. Numerical observation callbacks never touch state.
            return notified;
        });
    }};
}

struct Completed {
    std::shared_ptr<Trace> trace;
    qb::HostReport report;
};
Completed run(Environment &e, bool legacy, std::int64_t initial, qb::TerminalMode terminal,
              bool fail_evidence = false) {
    auto suite = e.suite(legacy, initial, terminal);
    auto plan = e.prepare(suite);
    auto trace = std::make_shared<Trace>();
    trace->strategy.profile = legacy ? qs::StochasticProfile::Legacy : qs::StochasticProfile::Fresh;
    auto host =
        take(qb::HostedSimulation::create(e.data, plan, e.build, suite, decisions(trace, e.data),
                                          e.types.price, executions(trace), e.ratio));
    trace->financial = host.financial();
    auto report = take(host.run(e.retention(plan), e.evidence(trace, fail_evidence)));
    require(trace->opens == 1 && trace->finalizations == 1,
            "required S3 state evidence did not run exactly once");
    return {std::move(trace), std::move(report)};
}
std::size_t events(const qb::FinancialRun &run, std::string_view kind) {
    return static_cast<std::size_t>(std::count_if(run.events.begin(), run.events.end(),
                                                  [&](const auto &e) { return e.kind == kind; }));
}
void accepted_evidence(const Environment &e, const Completed &result) {
    require(result.report.engine.acceptance == lx::AcceptanceStatus::Accepted &&
                result.report.engine.numerical == lx::NumericalOutcome::Completed &&
                result.report.engine.integration == lx::IntegrationOutcome::Completed &&
                result.report.financial->exhausted && result.report.financial->finalized &&
                !result.report.financial->failure,
            "valid hosted stochastic run was not accepted");
    require(result.report.engine.evidence.size() == 3,
            "accepted inventory lacks retention/financial/state evidence");
    bool state = false;
    for (const auto &evidence : result.report.engine.evidence) {
        take(lx::files::verify(evidence, e.data.account(), 4 * mb));
        state |= evidence.name == "stochastic_state";
    }
    require(state, "required stochastic state was not in accepted evidence");
    const auto bytes = take(lx::files::read(e.path / "state.csv", e.data.account(), 65536));
    require(bytes.value == result.trace->serialized,
            "persisted stochastic state differs from callback prefix");
}
void terminal_checks(const Completed &result) {
    const auto &f = *result.report.financial;
    const auto &t = f.trials.at(0);
    const auto &s = result.trace->strategy;
    require(t.account && t.account->units.raw() == 0 && t.account->basis.amount() == 0 &&
                !t.pending,
            "synthetic endpoint left holdings/basis/pending order");
    require(s.terminal && s.held_units == 0 && !s.pending_side && !s.pending_decision &&
                !s.parity_failed,
            "synthetic sale did not clear trial execution state");
    require(result.trace->long_keys == std::vector<std::uint64_t>{4} &&
                result.trace->ratio_keys == std::vector<std::uint64_t>{4, 5},
            "five-Bar activation or hosted ratio delivery changed");
    const auto &receipts = result.trace->receipts;
    require(receipts.size() == 2 && receipts[0].key == 5 && receipts[1].key == 5 &&
                !receipts[0].terminal && receipts[1].terminal &&
                receipts[0].side == qd::Side::Buy && receipts[1].side == qd::Side::Sell &&
                receipts[0].units > 0 && receipts[1].units == 0,
            "final-morning buy and final-close synthetic sale notifications changed");
    std::set<std::uint64_t> ids;
    std::size_t fills = 0;
    for (const auto &row : t.ledger->rows()) {
        if (const auto *fill = std::get_if<qb::Fill>(&row.event)) {
            ++fills;
            require(ids.insert(fill->id).second && fill->mark.key() == 5,
                    "duplicate/wrong-session posted fill");
            require(fill->synthetic_terminal
                        ? (!fill->intent && fill->mark.phase() == qb::ValuationPhase::Close)
                        : (fill->intent && fill->intent->decision_key == 4 &&
                           fill->intent->earliest_open_key == 5 &&
                           fill->mark.phase() == qb::ValuationPhase::Open),
                    "synthetic sale fabricated an opening intent");
        }
    }
    require(fills == 2 && events(f, "submitted") == 1 && events(f, "filled") == 1 &&
                events(f, "terminal") == 1 && events(f, "rejected") == 0,
            "duplicate/missing order or terminal fill");
}

void terminal_and_state_failure(const std::filesystem::path &root, const lx::HostBuild &build) {
    Environment good(root, "final_morning_terminal", 6, build);
    auto accepted = run(good, true, 1000000, qb::TerminalMode::SyntheticClose);
    accepted_evidence(good, accepted);
    terminal_checks(accepted);
    std::cout << "PASS hosted S3 final-morning entry/terminal notification evidence=" << good.path
              << '\n';

    Environment bad(root, "required_state_failure", 6, build);
    auto rejected = run(bad, true, 1000000, qb::TerminalMode::SyntheticClose, true);
    terminal_checks(rejected);
    require(rejected.report.financial->exhausted && rejected.report.financial->finalized &&
                !rejected.report.financial->failure &&
                rejected.report.engine.numerical == lx::NumericalOutcome::Completed &&
                rejected.report.engine.acceptance == lx::AcceptanceStatus::Rejected &&
                rejected.report.engine.cause,
            "required state finalization failure did not reject otherwise completed financial run");
    require(
        std::none_of(rejected.report.engine.evidence.begin(), rejected.report.engine.evidence.end(),
                     [](const auto &reference) { return reference.name == "stochastic_state"; }),
        "failed state evidence obtained a digest reference");
    require(rejected.report.engine.cache_published_entries == 0 &&
                rejected.report.engine.cache_commit_id.empty(),
            "failed state evidence published a cache origin");
    std::cout << "PASS required S3 state evidence failure rejects acceptance evidence=" << bad.path
              << '\n';
}
void rejection_profiles(const std::filesystem::path &root, const lx::HostBuild &build) {
    Environment fresh(root, "fresh_rejected_episode", 8, build);
    auto accepted = run(fresh, false, 1, qb::TerminalMode::Mark);
    accepted_evidence(fresh, accepted);
    const auto &f = *accepted.report.financial;
    const auto &s = accepted.trace->strategy;
    require(accepted.trace->rejection_unchanged && accepted.trace->decisions == 8 &&
                accepted.trace->ratio_keys == std::vector<std::uint64_t>{4, 5, 6, 7} &&
                accepted.trace->long_keys == std::vector<std::uint64_t>{4} &&
                accepted.trace->receipts.size() == 1 && accepted.trace->receipts[0].rejected &&
                accepted.trace->receipts[0].key == 5,
            "fresh episode retried or lost rejection notification");
    require(s.episode_start == 4 && s.entry_consumed && s.held_units == 0 && !s.pending_side &&
                !s.parity_failed && !f.trials[0].pending &&
                f.trials[0].account->cash.amount() == 1 && f.trials[0].account->units.raw() == 0 &&
                events(f, "submitted") == 1 && events(f, "rejected") == 1 &&
                events(f, "filled") == 0 && events(f, "terminal") == 0,
            "fresh rejected episode mutated account/reset timer/retried entry");
    require(std::all_of(f.trials[0].ledger->rows().begin(), f.trials[0].ledger->rows().end(),
                        [](const auto &row) {
                            return std::holds_alternative<qb::DividendEvent>(row.event);
                        }),
            "rejected entry created a ledger fill");
    std::cout << "PASS hosted fresh rejected episode consumed without account mutation evidence="
              << fresh.path << '\n';

    Environment legacy(root, "legacy_rejected_entry", 6, build);
    auto rejected = run(legacy, true, 1, qb::TerminalMode::SyntheticClose);
    const auto &failed = *rejected.report.financial;
    require(rejected.report.engine.acceptance == lx::AcceptanceStatus::Rejected && failed.failure &&
                failed.failure->code == qb::ErrorCode::Integrity && !failed.finalized &&
                !failed.exhausted && rejected.trace->strategy.parity_failed &&
                rejected.trace->rejection_unchanged,
            "legacy below-lot entry did not fatally reject hosted parity");
    require(rejected.trace->decisions == 5 && rejected.report.engine.complete_updates == 5 &&
                rejected.trace->receipts.size() == 1 && rejected.trace->receipts[0].rejected &&
                rejected.trace->receipts[0].parity_failed && events(failed, "rejected") == 1 &&
                events(failed, "filled") == 0 && events(failed, "terminal") == 0 &&
                failed.trials[0].account->cash.amount() == 1 &&
                failed.trials[0].account->units.raw() == 0,
            "fatal legacy rejection lost prefix or continued to close/terminal");
    require(std::none_of(failed.events.begin(), failed.events.end(),
                         [](const auto &event) {
                             return event.key >= 5 &&
                                    (event.kind == "close" || event.kind == "submitted");
                         }),
            "legacy rejection reached a post-failure close decision");
    const auto diagnostic =
        take(lx::files::read(legacy.path / "state.csv", legacy.data.account(), 65536));
    require(diagnostic.value == rejected.trace->serialized &&
                diagnostic.value.find("rejected,5,4,1,0,0,1,1,0\n") != std::string::npos,
            "fatal legacy rejected-state row was not finalized before failure propagation");
    std::cout << "PASS hosted legacy rejection fails parity and preserves state prefix evidence="
              << legacy.path << '\n';
}
void typed_admission(const std::filesystem::path &root, const lx::HostBuild &build) {
    Environment e(root, "typed_admission", 6, build);
    const auto suite = e.suite(true, 1000000, qb::TerminalMode::SyntheticClose);
    auto plan = e.prepare(suite);
    auto trace = std::make_shared<Trace>();
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto wrong = suite;
        auto type = std::optional{e.ratio};
        if (mode == 0) {
            wrong.trials[0].observations[0].kind = qb::FeatureKind::Price;
        } // actual Ratio, required Price
        if (mode == 1) {
            wrong.trials[0].observations[0] = {"P", 0, 6, qb::FeatureKind::Ratio};
        } // actual Price, required Ratio
        if (mode == 2) {
            wrong.trials[0].observations[0].begin = 5;
        } // fully admitted plan has [4,6)
        if (mode == 3) {
            type.reset();
        } // actual custom output with no typed host witness
        auto created =
            qb::HostedSimulation::create(e.data, plan, e.build, wrong, decisions(trace, e.data),
                                         e.types.price, executions(trace), type);
        const auto *error = std::get_if<qb::BacktestError>(&created);
        require(error && error->code == qb::ErrorCode::InvalidConfig,
                "required observation type/scope mismatch was admitted");
        require(trace->decisions == 0 && trace->receipts.empty() && trace->opens == 0 &&
                    trace->finalizations == 0 &&
                    !std::filesystem::exists(e.path / "financial.txt") &&
                    !std::filesystem::exists(e.path / "state.csv") &&
                    !std::filesystem::exists(e.path / "retention.log"),
                "admission rejection opened evidence or reached funding callbacks");
    }
    // Prove the underlying registered graph is usable, rather than getting four
    // rejections from an unrelated invalid fixture or source identity mismatch.
    trace->strategy.profile = qs::StochasticProfile::Legacy;
    auto host =
        take(qb::HostedSimulation::create(e.data, plan, e.build, suite, decisions(trace, e.data),
                                          e.types.price, executions(trace), e.ratio));
    require(!host.financial()->trials[0].account && host.financial()->events.empty(),
            "create funded the valid trial prematurely");
    trace->financial = host.financial();
    Completed valid{trace, take(host.run(e.retention(plan), e.evidence(trace)))};
    accepted_evidence(e, valid);
    terminal_checks(valid);
    std::cout << "PASS actual Ratio/Price reverse mismatch, changed scope, missing witness reject "
                 "before funding\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: backtest_stochastic_host_tests <new-scratch-directory>");
        const auto root = std::filesystem::absolute(argv[1]);
        require(std::filesystem::is_directory(root.parent_path()) &&
                    std::filesystem::create_directory(root),
                "hosted stochastic scratch must be new beneath an existing directory");
        const auto build = take(qb::inspect_financial_host(resources(), {128 * mb, mb, 512}));
        terminal_and_state_failure(root, build);
        rejection_profiles(root, build);
        typed_admission(root, build);
        std::cout << "PASS 5 hosted stochastic groups: terminal, required-state failure, fresh "
                     "rejection, legacy rejection, typed admission\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
