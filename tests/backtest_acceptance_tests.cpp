#include "backtest_host_support.hpp"
#include "report_evidence.hpp"
#include <array>
#include <map>
#include <set>

namespace {
using namespace backtest_host_test;
namespace app = qb::app;
namespace files = quant::storage::files;
constexpr std::uint64_t mib = 1048576;
enum class Fault { None, Finalize, LedgerMismatch, HostFailure };

std::map<std::filesystem::path, std::string> markers(const std::filesystem::path &directory) {
    std::map<std::filesystem::path, std::string> result;
    if (std::filesystem::exists(directory)) {
        for (const auto &item : std::filesystem::directory_iterator(directory)) {
            if (item.path().filename().string().ends_with(".accepted.xml")) {
                result.emplace(item.path(), files::read(item.path()));
            }
        }
    }
    return result;
}
app::ReportEvidenceSpecs report_specs(Environment &e) {
    const std::array<std::uint32_t, 2> ids{1, 2};
    return take(app::report_specs(e.path, ids, 4, mib));
}
lx::FileResultStore store(Environment &e) {
    lx::LocalCacheConfiguration config{"automatic_read_write",
                                       "local_files",
                                       (e.path / "cache").string(),
                                       "exact_full_scope",
                                       "same_build_local_writer",
                                       "sha256",
                                       "single_process_lock",
                                       "fail_preparation",
                                       "bypass_new_entries",
                                       "explicit_only",
                                       64,
                                       8 * mib,
                                       mib,
                                       mib};
    lx::FileStoreLimits limits{{{mib, 1024, 32768, 4 * mib}, 32768, 4096, 4096, 4096, 4096},
                               {{mib, 65536, 64, 4096, 8 * mib}, 4 * mib},
                               {mib, 65536, 1024, 4096, 4096, mib, 16 * mib},
                               mib,
                               512,
                               4 * mib,
                               metadata_limits()};
    return take(lx::FileResultStore::create(config, e.types.price, e.build, account(), limits));
}
void contains(std::string_view text, std::string_view wanted) {
    require(text.find(wanted) != std::string_view::npos,
            "missing report projection: " + std::string{wanted});
}
std::string compact_json(std::string_view input) {
    std::string out;
    bool quoted = false, escaped = false;
    for (char c : input) {
        if (quoted) {
            out += c;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
            out += c;
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            out += c;
        }
    }
    return out;
}
void validate_reports(const Environment &e) {
    for (unsigned trial : {1U, 2U}) {
        const auto name = "report-" + std::to_string(trial);
        const auto json = compact_json(files::read(e.path / (name + ".json")));
        const auto html = files::read(e.path / (name + ".html"));
        const auto csv = files::read(e.path / (name + ".csv"));
        contains(json, "\"schema\":\"quadeplus-report-evidence-v1\"");
        contains(json, "\"financial_state\":\"completed\"");
        contains(json, "\"trial_id\":\"" + std::to_string(trial) + "\"");
        contains(json, "\"funding_date\":\"2026-01-02\"");
        contains(json, "\"evaluation_begin\":\"2026-01-05\"");
        contains(json, "\"evaluation_end\":\"2026-01-08\"");
        contains(json, "\"declared_evaluation_sessions\":\"4\"");
        contains(json, "\"funded_date\":\"2026-01-02\"");
        contains(json, "\"final_date\":\"2026-01-08\"");
        contains(json, "\"sessions\":4,");
        contains(json, "\"elapsed_calendar_days\":6,");
        contains(json,
                 "\"dataset_manifest_sha256\":\"" + std::string{e.data.manifest_sha256()} + "\"");
        contains(html, "acceptance&lt;&amp;&gt;");
        contains(html, "<dt>funding_date</dt><dd>2026-01-02</dd>");
        contains(html, "<dt>evaluation_begin</dt><dd>2026-01-05</dd>");
        contains(html, "Engine acceptance is separate.");
        contains(csv, "\"/metadata/declared_evaluation_sessions\",\"string\",\"4\"\r\n");
        contains(csv, "\"/metrics/sessions\",\"number\",\"4\"\r\n");
        contains(csv, "\"/metrics/daily/0/date\",\"string\",\"2026-01-05\"\r\n");
        contains(csv, "\"/metrics/daily/3/date\",\"string\",\"2026-01-08\"\r\n");
        require(csv.find("/metrics/daily/4/") == std::string::npos,
                "funding row was counted as a return session");
        if (trial == 1) {
            // Frozen 95%-cash entry at 110 with 5+5bps, then marked at 80.
            // The funded baseline, not the first filled mark, is the denominator.
            contains(json, "\"funded_equity_cents\":1000000,");
            contains(json, "\"final_equity_cents\":740219,");
            contains(json, "\"net_pnl_cents\":-259781,");
            contains(json,
                     "\"total_return\":{\"value\":{\"numerator\":-259781,\"denominator\":1000000}");
            contains(csv,
                     "\"/metrics/daily/1/daily_return/value/numerator\",\"number\",\"-949\"\r\n");
            contains(
                csv,
                "\"/metrics/daily/1/daily_return/value/denominator\",\"number\",\"1000000\"\r\n");
            contains(json, "\"open_position\":true");
        } else {
            contains(json, "\"funded_equity_cents\":2000000,");
            contains(json, "\"final_equity_cents\":2000000,");
            contains(json, "\"total_return\":{\"value\":{\"numerator\":0,\"denominator\":1}");
            contains(json, "\"sharpe_zero_risk_free\":{\"value\":null,\"undefined_reason\":\"zero_"
                           "return_variance\"}");
            contains(
                json,
                "\"profit_factor\":{\"value\":null,\"undefined_reason\":\"no_losing_trade_pnl\"}");
            contains(csv, "\"/metrics/sharpe_zero_risk_free/value\",\"null\",\"null\"\r\n");
            contains(csv, "\"/metrics/profit_factor/"
                          "undefined_reason\",\"string\",\"no_losing_trade_pnl\"\r\n");
            contains(html, "zero_return_variance");
            contains(html, "no_losing_trade_pnl");
        }
    }
}

void validate_partial_reports(const Environment &e, const qb::FinancialRun &financial) {
    for (unsigned trial : {1U, 2U}) {
        std::uint64_t events = 0, closes = 0;
        for (const auto &event : financial.events) {
            require(event.key <= 2 && event.kind != "final" && event.kind != "terminal",
                    "failed host fabricated a later endpoint");
            if (event.trial == trial) {
                ++events;
                if (event.kind == "close" && event.key >= 1) {
                    ++closes;
                }
            }
        }
        // Trial 1 fails in its key-2 decision after its ordinary close; trial 2
        // retains that opening's dividend but never reaches the key-2 close.
        require(closes == (trial == 1 ? 2U : 1U),
                "unexpected trial-local financial prefix coverage");
        const auto name = "report-" + std::to_string(trial);
        const auto json = compact_json(files::read(e.path / (name + ".json")));
        const auto html = files::read(e.path / (name + ".html"));
        const auto csv = files::read(e.path / (name + ".csv"));
        contains(json, "\"schema\":\"quadeplus-report-evidence-v1\"");
        contains(json, "\"financial_state\":\"partial\"");
        contains(json, "\"trial_id\":\"" + std::to_string(trial) + "\"");
        contains(json, "\"numerical_source_rows\":\"5\"");
        contains(json, "\"funding_date\":\"2026-01-02\"");
        contains(json, "\"evaluation_begin\":\"2026-01-05\"");
        contains(json, "\"evaluation_end\":\"2026-01-08\"");
        contains(json, "\"declared_evaluation_sessions\":\"4\"");
        contains(json, "\"observed_trial_events\":" + std::to_string(events) + ",");
        contains(json, "\"observed_evaluation_close_events\":" + std::to_string(closes) + ",");
        contains(json, "\"exhausted\":false");
        contains(json, "\"finalized\":false");
        contains(json, "test-owned financial decision failure at key2");
        contains(json, "\"metrics\":null");
        require(json.find("\"final_equity_cents\"") == std::string::npos &&
                    json.find("\"net_pnl_cents\"") == std::string::npos,
                "partial JSON invented final equity/PnL metrics");
        contains(html, "Partial financial diagnostic");
        contains(html, "Coverage and final metrics are not established.");
        contains(html, "&quot;financial_state&quot;:&quot;partial&quot;");
        contains(html, "&quot;metrics&quot;:null");
        contains(html, "&quot;declared_evaluation_sessions&quot;:&quot;4&quot;");
        contains(html,
                 "&quot;observed_evaluation_close_events&quot;:" + std::to_string(closes) + ",");
        require(html.find("net_pnl_cents") == std::string::npos &&
                    html.find("final_equity_cents") == std::string::npos,
                "partial HTML invented final equity/PnL metrics");
        contains(csv, "\"/financial_state\",\"string\",\"partial\"\r\n");
        contains(csv, "\"/metadata/declared_evaluation_sessions\",\"string\",\"4\"\r\n");
        contains(csv, "\"/diagnostic/observed_trial_events\",\"number\",\"" +
                          std::to_string(events) + "\"\r\n");
        contains(csv, "\"/diagnostic/observed_evaluation_close_events\",\"number\",\"" +
                          std::to_string(closes) + "\"\r\n");
        contains(csv, "\"/diagnostic/exhausted\",\"boolean\",\"false\"\r\n");
        contains(csv, "\"/diagnostic/finalized\",\"boolean\",\"false\"\r\n");
        contains(csv, "\"/metrics\",\"null\",\"null\"\r\n");
        require(csv.find("/metrics/") == std::string::npos,
                "partial CSV contains computed metric leaves");
    }
}

qb::HostReport run(Environment &e, Fault fault) {
    auto specs = report_specs(e);
    auto suite = e.suite(); // Keep exactly the configuration given to create().
    if (fault == Fault::HostFailure) {
        suite.trials[0].strategy_configuration += ";decision_failure_key2";
    }
    auto cache = store(e);
    const auto plan = e.prepare(suite, false, cache.service(), specs.required);
    require(plan->required_evidence().size() == 8, "reports were not declared before preparation");
    struct Observed {
        unsigned finalized = 0;
        bool injected = false;
    };
    auto observed = std::make_shared<Observed>();
    auto decisions = callbacks();
    if (fault == Fault::HostFailure) {
        auto ordinary = decisions[0];
        decisions[0] =
            [ordinary, observed](
                const qb::DecisionContext &context, const qb::Account *account,
                std::span<const qb::PriceFeature> features) -> qb::BacktestResult<qb::Action> {
            if (context.key == 2) {
                observed->injected = true;
                return qb::error(qb::ErrorCode::Integrity,
                                 "test-owned financial decision failure at key2");
            }
            return ordinary(context, account, features);
        };
    }
    auto host = take(qb::HostedSimulation::create(e.data, plan, e.build, suite,
                                                  std::move(decisions), e.types.price));
    const auto financial = host.financial();
    // A test-owned view permits a bad report/ledger projection without mutating
    // the real host or introducing a production fault hook/const_cast.
    auto projection = std::make_shared<qb::FinancialRun>();
    projection->trials.resize(suite.trials.size());
    projection->experiment = financial->experiment;
    projection->effective_configuration = financial->effective_configuration;
    std::shared_ptr<const qb::FinancialRun> report_input =
        fault == Fault::LedgerMismatch ? projection : financial;
    auto report_sinks =
        take(app::bind_report_sinks(specs, report_input, suite, e.data,
                                    {"acceptance<&>", "fresh_research_v1", "synthetic_fixture",
                                     "authored five-Bar acceptance fixture"}));
    auto extra = e.extra(plan);
    for (std::size_t i = 0; i < report_sinks.size(); ++i) {
        auto original = report_sinks[i].finalize;
        report_sinks[i].finalize =
            [original, observed, financial, projection, fault, i,
             path = e.path](const lx::RunReport &report) -> lx::Result<lx::EvidenceReference> {
            return lx::guarded([&]() -> lx::Result<lx::EvidenceReference> {
                require(markers(path / "cache").empty(),
                        "accepted marker preceded required report finalization");
                if (fault == Fault::HostFailure) {
                    require(
                        !financial->finalized && !financial->exhausted && financial->failure,
                        "host-failure report finalizer did not receive a failed financial prefix");
                } else {
                    require(financial->finalized && financial->exhausted && !financial->failure,
                            "report fault preceded completed financial work");
                }
                ++observed->finalized;
                if (fault == Fault::LedgerMismatch && i == 0) {
                    projection->trials = financial->trials;
                    projection->events = financial->events;
                    projection->exhausted = financial->exhausted;
                    projection->finalized = financial->finalized;
                    auto event = std::find_if(
                        projection->events.begin(), projection->events.end(),
                        [](const auto &v) { return v.trial == 1 && v.kind == "filled"; });
                    require(event != projection->events.end(),
                            "mismatch fixture lacks a real posted fill");
                    ++event->cash;
                    observed->injected = true;
                }
                auto result = original(report);
                // Exercise the actual report write/close/hash first; returning an
                // error must still withhold the entire origin's acceptance marker.
                if (fault == Fault::Finalize && i == 1) {
                    require(std::holds_alternative<lx::EvidenceReference>(result),
                            "real report JSON failed before injected finalization error");
                    observed->injected = true;
                    return lx::diagnostic(lx::FailureCode::EvidenceIO,
                                          "test-owned required report finalization failure");
                }
                return result;
            });
        };
        extra.push_back(std::move(report_sinks[i]));
    }
    auto result = take(host.run(e.retention(plan), std::move(extra)));
    if (fault == Fault::HostFailure) {
        require(!result.financial->finalized && !result.financial->exhausted &&
                    result.financial->failure,
                "injected financial failure lost its partial state");
        require((result.engine.numerical == lx::NumericalOutcome::Aborted ||
                 result.engine.numerical == lx::NumericalOutcome::Failed) &&
                    result.engine.integration == lx::IntegrationOutcome::Failed &&
                    result.engine.complete_updates == 3,
                "failed financial decision did not stop the native host at key2");
    } else {
        require(result.financial->finalized && result.financial->exhausted &&
                    !result.financial->failure,
                "acceptance fixture did not finish its real financial ledger");
        require(result.engine.numerical == lx::NumericalOutcome::Completed &&
                    result.engine.integration == lx::IntegrationOutcome::Completed,
                "report acceptance gate failed earlier than report finalization");
    }
    if (fault == Fault::None) {
        require(observed->finalized == 6 && !observed->injected,
                "not all six native reports finalized");
        require(result.engine.acceptance == lx::AcceptanceStatus::Accepted &&
                    result.engine.cache == lx::CachePublicationStatus::Published,
                "successful required reports did not permit native publication");
        require(markers(e.path / "cache").size() == 1 && result.engine.cache_published_entries > 0,
                "accepted origin/eligible entry missing");
        require(result.engine.evidence.size() == 9,
                "closed evidence inventory lost ledger/snapshot/retention/reports");
        for (const auto &spec : specs.required) {
            const auto count = std::count_if(
                result.engine.evidence.begin(), result.engine.evidence.end(), [&](const auto &ref) {
                    return ref.name == spec.name && ref.absolute_path == spec.absolute_path &&
                           ref.finalized;
                });
            require(count == 1, "report missing/duplicated in finalized native inventory");
        }
        for (const auto &ref : result.engine.evidence) {
            take(lx::files::verify(ref, e.data.account(), 4 * mib));
        }
        const auto marker = markers(e.path / "cache");
        auto accepted = take(
            lx::decode_accepted_run(marker.begin()->second, e.data.account(), metadata_limits()));
        for (const auto &spec : specs.required) {
            require(std::count_if(accepted.value.inventory.begin(), accepted.value.inventory.end(),
                                  [&](const auto &ref) {
                                      return ref.name == spec.name &&
                                             ref.absolute_path == spec.absolute_path;
                                  }) == 1,
                    "published marker omitted required report inventory");
        }
        validate_reports(e);
    } else {
        require(observed->injected, "report fault path was not exercised");
        require(result.engine.acceptance == lx::AcceptanceStatus::Rejected &&
                    result.engine.cache == lx::CachePublicationStatus::NotPublished &&
                    result.engine.cache_published_entries == 0 && markers(e.path / "cache").empty(),
                "invalid required report published an accepted origin");
        require(result.engine.cause.has_value(), "rejected report has no diagnostic");
        contains(result.engine.cause->text(),
                 fault == Fault::Finalize      ? "test-owned required report finalization failure"
                 : fault == Fault::HostFailure ? "test-owned financial decision failure at key2"
                                               : "host event balances differ");
        if (fault == Fault::LedgerMismatch) {
            const auto actual =
                std::find_if(financial->events.begin(), financial->events.end(),
                             [](const auto &v) { return v.trial == 1 && v.kind == "filled"; });
            require(actual != financial->events.end() && actual->cash == 50000,
                    "test corrupted the host instead of only its report input");
        }
        if (fault == Fault::HostFailure) {
            require(observed->finalized == 6,
                    "failed financial host did not finalize all six partial report artifacts");
            const auto &trial = result.financial->trials[0];
            require(trial.account && trial.ledger && trial.account->cash.amount() == 50000 &&
                        trial.account->units.raw() == 86277361 &&
                        trial.account->basis.amount() == 950000,
                    "financial failure discarded the already-posted entry account");
            require(trial.ledger->rows().size() == 3 &&
                        trial.ledger->rows().back().after == *trial.account,
                    "financial failure discarded/mutated the posted ledger prefix");
            require(std::count_if(trial.ledger->rows().begin(), trial.ledger->rows().end(),
                                  [](const auto &row) {
                                      return std::holds_alternative<qb::Fill>(row.event);
                                  }) == 1,
                    "financial failure duplicated or removed the posted entry fill");
            for (const auto &spec : specs.required) {
                auto reference = std::find_if(result.engine.evidence.begin(),
                                              result.engine.evidence.end(), [&](const auto &ref) {
                                                  return ref.name == spec.name &&
                                                         ref.absolute_path == spec.absolute_path &&
                                                         ref.finalized;
                                              });
                require(reference != result.engine.evidence.end(),
                        "partial report missing from finalized diagnostic inventory");
                take(lx::files::verify(*reference, e.data.account(), mib));
            }
            validate_partial_reports(e, *result.financial);
        }
    }
    return result;
}

void probe(Environment &e, bool expected_artifact) {
    auto cache = store(e);
    auto specs = report_specs(e);
    auto plan = e.prepare(e.suite(), false, cache.service(), specs.required);
    require((plan->artifact(1) != nullptr) == expected_artifact,
            "fresh preparation selected the wrong eligible origin set");
}
void report_acceptance() {
    for (auto fault : {Fault::None, Fault::Finalize, Fault::LedgerMismatch, Fault::HostFailure}) {
        Environment e;
        auto result = run(e, fault);
        probe(e, fault == Fault::None);
        require(markers(e.path / "cache").size() == (fault == Fault::None ? 1U : 0U),
                "preparation published a new cache origin");
        std::cout << "PASS native report "
                  << (fault == Fault::None          ? "acceptance/metrics/inventory"
                      : fault == Fault::Finalize    ? "finalization failure"
                      : fault == Fault::HostFailure ? "failed financial host/partial diagnostics"
                                                    : "ledger mismatch")
                  << " evidence=" << e.path << '\n';
    }
}
void source_corruption() {
    Environment e;
    auto result = run(e, Fault::None);
    const auto original = markers(e.path / "cache");
    qr::DatasetRequest request{
        e.path / "bundle", std::string{e.data.manifest_sha256()}, {mib, 8, 64}, account()};
    auto good = take(qr::load_daily_dataset(request));
    require(good.rows().size() == 5, "source reload positive control failed");
    const std::string tamper = "\ncorrupt-retained-source\n";
    files::write_bytes(e.path / "bundle/prices.csv",
                       std::as_bytes(std::span{tamper.data(), tamper.size()}), true);
    const auto bad = qr::load_daily_dataset(request);
    const auto *error = std::get_if<qb::BacktestError>(&bad);
    require(error && error->code == qb::ErrorCode::Integrity,
            "corrupted source did not fail checked fresh loading");
    require(markers(e.path / "cache") == original,
            "failed source reload changed/published an accepted marker");
    std::cout
        << "PASS corrupt retained source rejects fresh load; original marker unchanged evidence="
        << e.path << '\n';
}
void cache_corruption() {
    Environment e;
    auto result = run(e, Fault::None);
    const auto original = markers(e.path / "cache");
    probe(e, true); // The intact payload and its complete report inventory are reusable.
    std::vector<std::filesystem::path> payloads;
    for (const auto &item : std::filesystem::directory_iterator(e.path / "cache")) {
        if (item.path().filename().string().ends_with(".price.xml")) {
            payloads.push_back(item.path());
        }
    }
    require(!payloads.empty(), "successful run staged no reusable Price payload");
    const std::string tamper = "\ncorrupt-accepted-payload\n";
    files::write_bytes(payloads.front(), std::as_bytes(std::span{tamper.data(), tamper.size()}),
                       true);
    auto cache = store(e);
    auto specs = report_specs(e);
    bool rejected = false;
    try {
        static_cast<void>(e.prepare(e.suite(), false, cache.service(), specs.required));
    } catch (const std::runtime_error &error) {
        contains(error.what(), "accepted payload integrity mismatch");
        rejected = true;
    }
    require(rejected, "corrupt accepted payload remained eligible during fresh preparation");
    require(markers(e.path / "cache") == original,
            "corrupt-cache preparation changed/published accepted origin");
    std::cout
        << "PASS corrupt Price payload rejects preparation; original marker unchanged evidence="
        << e.path << '\n';
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: backtest_acceptance_tests SCRATCH_ROOT");
        root = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(root);
        inspected_build = take(qb::inspect_financial_host(account(), {134217728, 1048576, 512}));
        report_acceptance();
        source_corruption();
        cache_corruption();
        std::cout
            << "PASS 6 native report acceptance groups: real reports, finalize failure, ledger "
               "mismatch, failed host/partial diagnostics, source corruption, cache corruption\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
