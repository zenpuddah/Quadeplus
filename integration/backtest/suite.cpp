#include "suite.hpp"
#include "suite_publication.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/storage/file_integrity.hpp"
#include "native_files.hpp"
#include <tinyxml2.h>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace quant::backtest::app {
namespace {
namespace lx = ::lexongine;
namespace fs = std::filesystem;
using detail::fail;
using detail::take;
namespace publication = suite_publication;
using publication::File;
using publication::write_file;
using publication::verify_file;
constexpr std::uint64_t mib = 1048576, file_limit = publication::file_limit;
constexpr std::array<std::string_view, 4> profiles{"sma", "keltner", "stochastic-fresh",
                                                   "stochastic-legacy"};
constexpr std::array<int, 3> costs{5, 10, 20};
void check(bool condition, std::string_view message) {
    if (!condition) {
        fail(ErrorCode::Integrity, message);
    }
}
template <class T> T engine(std::variant<T, lx::Diagnostic> value) {
    if (const auto *error = std::get_if<lx::Diagnostic>(&value)) {
        fail(ErrorCode::IO, error->text());
    }
    return std::get<T>(std::move(value));
}
std::string hex(const lx::Sha256 &value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : value) {
        auto n = std::to_integer<unsigned>(byte);
        result += digits[n >> 4];
        result += digits[n & 15];
    }
    return result;
}
std::string hash(std::string_view value) {
    return hex(engine(lx::files::digest(value)));
}
std::string quote(std::string_view value) {
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += digits[c >> 4];
            result += digits[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}
void path_ok(const fs::path &path) {
    check(path.is_absolute() && path == path.lexically_normal() && path.string().size() <= 4096,
          "suite path must be normalized absolute and bounded");
    for (unsigned char c : path.string()) {
        check(c >= 32 && c != 127, "control character in suite path");
    }
    storage::files::no_symlink(path);
}
std::string relative_path(const fs::path &root, const fs::path &path) {
    path_ok(path);
    auto result = path.lexically_relative(root);
    check(!result.empty() && !result.is_absolute(), "evidence outside suite directory");
    for (const auto &part : result) {
        check(part != ".." && part != ".", "unsafe suite evidence path");
    }
    return result.generic_string();
}
using Attributes = std::map<std::string, std::string>;
Attributes attributes(const tinyxml2::XMLElement &element) {
    Attributes result;
    for (auto *a = element.FirstAttribute(); a; a = a->Next()) {
        check(std::string_view{a->Value()}.size() <= 4096 &&
                  result.emplace(a->Name(), a->Value()).second,
              "duplicate or oversized suite attribute");
    }
    return result;
}
struct Profile {
    std::string name;
    Config config;
};
std::vector<Profile> parse_suite(const fs::path &path, std::string_view text) {
    tinyxml2::XMLDocument document;
    check(document.Parse(text.data(), text.size()) == tinyxml2::XML_SUCCESS, "invalid suite XML");
    auto *root = document.RootElement();
    check(root && std::string_view{root->Name()} == "suite" &&
              attributes(*root) ==
                  Attributes{{"version", "1"}, {"costs", "5,10,20"}, {"cache", "disabled"}},
          "unsupported suite root/attributes");
    bool declaration = false;
    for (auto *n = document.FirstChild(); n; n = n->NextSibling()) {
        if (n == root) {
            continue;
        }
        check(n == document.FirstChild() && n->ToDeclaration() && !declaration &&
                  std::string_view{n->Value()} == "xml version=\"1.0\"",
              "unexpected suite XML node/declaration");
        declaration = true;
    }
    std::vector<Profile> result;
    for (auto *node = root->FirstChild(); node; node = node->NextSibling()) {
        auto *element = node->ToElement();
        check(element && std::string_view{element->Name()} == "profile" && !element->FirstChild() &&
                  result.size() < profiles.size(),
              "invalid suite profile element");
        const auto attrs = attributes(*element);
        check(attrs.size() == 2 && attrs.contains("name") && attrs.contains("config") &&
                  attrs.at("name") == profiles[result.size()] && !attrs.at("config").empty(),
              "suite requires the four ordered named profiles");
        fs::path config_path{attrs.at("config")};
        if (config_path.is_relative()) {
            config_path = path.parent_path() / config_path;
        }
        config_path = config_path.lexically_normal();
        path_ok(config_path);
        auto config = take(read_config(config_path));
        const bool legacy = result.size() == 3, keltner = result.size() == 1;
        const std::string expected = result.empty() ? "sma20_50_long_cash_v1"
                                     : keltner      ? "keltner_atr_risk_v1"
                                                    : "spy_stochastic5_v1";
        check(config.strategy == expected &&
                  config.profile == (legacy ? "legacy_spy_2026_v1" : "fresh_research_v1") &&
                  config.terminal == (legacy ? TerminalMode::SyntheticClose : TerminalMode::Mark) &&
                  config.cache_directory.empty() && config.symbol == "SPY" &&
                  config.origin == "2004-11-18" &&
                  config.begin == (legacy ? "2026-08-17" : "2010-01-04") &&
                  config.end == "2026-10-15" && config.initial_cash == 1000000 && config.lot == 1,
              "suite profile identity/defaults/cache mismatch");
        result.push_back({attrs.at("name"), std::move(config)});
    }
    check(result.size() == profiles.size(), "incomplete suite profile inventory");
    return result;
}
std::string effective_xml(const Config &config, int cost) {
    tinyxml2::XMLDocument document;
    check(document.Parse(config.xml.data(), config.xml.size()) == tinyxml2::XML_SUCCESS,
          "invalid retained child XML");
    auto *root = document.RootElement();
    check(root != nullptr, "missing child XML root");
    root->SetAttribute("commission_bps", cost);
    root->SetAttribute("slippage_bps", cost);
    tinyxml2::XMLPrinter printer;
    document.Print(&printer);
    return printer.CStr();
}
std::string file_json(const File &f) {
    return "{\"name\":" + quote(f.name) + ",\"format\":" + quote(f.format) +
           ",\"path\":" + quote(f.path) + ",\"bytes\":" + std::to_string(f.bytes) +
           ",\"sha256\":" + quote(f.sha) + "}";
}
std::string files_json(const std::vector<File> &files) {
    std::string out = "[";
    for (const auto &f : files) {
        if (out.size() > 1) {
            out += ',';
        }
        out += file_json(f);
    }
    return out + ']';
}
std::string build_json(const lx::HostBuild &build) {
    const auto &id = build.identity();
    check(id.binary_digest != lx::Sha256{} && id.dependencies_digest != lx::Sha256{} &&
              id.source_build_digest != lx::Sha256{} && !id.sdk_build_contract_id.empty() &&
              !build.platform_identity().empty(),
          "incomplete inspected native build");
    return "{\"binary_sha256\":" + quote(hex(id.binary_digest)) +
           ",\"dependencies_sha256\":" + quote(hex(id.dependencies_digest)) +
           ",\"source_build_sha256\":" + quote(hex(id.source_build_digest)) +
           ",\"sdk_build_contract_id\":" + quote(id.sdk_build_contract_id) +
           ",\"platform\":" + quote(build.platform_identity()) + "}";
}
std::string summary_json(const Summary &s) {
    std::ostringstream out;
    out << "{\"trial\":" << s.trial << ",\"strategy\":" << quote(s.strategy)
        << ",\"cash_cents\":" << s.cash << ",\"units_raw\":" << s.units
        << ",\"equity_cents\":" << s.equity << ",\"pnl_cents\":" << s.pnl
        << ",\"realized_cents\":" << s.realized << ",\"unrealized_cents\":" << s.unrealized
        << ",\"dividend_cents\":" << s.dividends << ",\"fee_cents\":" << s.fees << '}';
    return out.str();
}
struct Child {
    std::string profile, path, config, config_sha, experiment;
    int cost;
    std::vector<File> files;
    std::vector<Summary> summaries;
};
std::string child_json(const Child &c) {
    std::string out = "{\"profile\":" + quote(c.profile) +
                      ",\"cost_bps\":" + std::to_string(c.cost) + ",\"path\":" + quote(c.path) +
                      ",\"config_path\":" + quote(c.config) +
                      ",\"config_sha256\":" + quote(c.config_sha) +
                      ",\"experiment_sha256\":" + quote(c.experiment) +
                      ",\"facts\":{\"numerical\":\"Completed\",\"integration\":\"Completed\","
                      "\"acceptance\":\"Accepted\",\"cache\":\"Disabled\","
                      "\"new_cache_origins\":0,\"financial_exhausted\":true,\"financial_"
                      "finalized\":true,\"required_count\":" +
                      std::to_string(c.files.size()) +
                      "},\"required_files\":" + files_json(c.files) + ",\"trials\":[";
    for (std::size_t i = 0; i < c.summaries.size(); ++i) {
        if (i) {
            out += ',';
        }
        out += summary_json(c.summaries[i]);
    }
    return out + "]}";
}
void close_child(Child &child, const ApplicationRun &run, const fs::path &out,
                 const publication::IO &io) {
    const auto &r = run.host.engine;
    const auto &f = run.host.financial;
    check(r.numerical == lx::NumericalOutcome::Completed &&
              r.integration == lx::IntegrationOutcome::Completed &&
              r.acceptance == lx::AcceptanceStatus::Accepted &&
              r.cache == lx::CachePublicationStatus::Disabled && !r.cause && !r.secondary_cause &&
              r.rejected.empty() && !r.cache_published_entries && !r.cache_staged_entries &&
              r.cache_entries.empty() && r.cache_commit_id.empty() && f && f->exhausted &&
              f->finalized && !f->failure,
          "child did not complete accepted no-cache execution");
    check(f->experiment != lx::Sha256{} && f->experiment == r.host_experiment_identity,
          "child experiment identity mismatch");
    child.experiment = hex(f->experiment);
    const std::size_t count = child.profile == "keltner" ? 6 : 3;
    check(run.summaries && run.summaries->size() == count && f->trials.size() == count,
          "child trial summary inventory mismatch");
    child.summaries = *run.summaries;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &s = child.summaries[i];
        const auto &t = f->trials[i];
        const std::array<std::string_view, 4> variants{"keltner_3_2", "keltner_2_1", "keltner_5_2",
                                                       "keltner_2_1_duplicate"};
        const std::string_view expected = i == count - 2               ? "cash_usd_no_interest_v1"
                                          : i == count - 1             ? "buy_hold95_next_open_v1"
                                          : child.profile == "keltner" ? variants[i]
                                          : child.profile == "sma"     ? "sma20_50_long_cash_v1"
                                                                       : "spy_stochastic5_v1";
        check(s.strategy == expected, "child strategy/variant summary mismatch");
        check(t.account && s.trial == t.account->trial && s.trial == i + 1 &&
                  s.cash == t.account->cash.amount() && s.units == t.account->units.raw() &&
                  s.realized == t.account->realized.amount() &&
                  s.dividends == t.account->dividends.amount() &&
                  s.fees == t.account->fees.amount() && s.pnl == detail::sub(s.equity, 1000000) &&
                  s.pnl == detail::add(detail::add(s.realized, s.unrealized), s.dividends),
              "child scalar ledger reconciliation failed");
        std::size_t endpoints = 0;
        for (const auto &event : f->events) {
            if (event.trial == s.trial && event.kind == "final") {
                check(event.equity && *event.equity == s.equity,
                      "child summary differs from final financial event");
                ++endpoints;
            }
        }
        check(endpoints == 1, "child lacks a unique final financial event");
    }
    check(child.summaries[count - 2].strategy == "cash_usd_no_interest_v1" &&
              child.summaries.back().strategy == "buy_hold95_next_open_v1",
          "child matched controls absent");
    check(!r.required_evidence.empty() && r.required_evidence.size() <= 64 &&
              r.required_evidence.size() == r.evidence.size(),
          "child closed evidence is incomplete");
    std::set<std::string> names, paths;
    for (const auto &spec : r.required_evidence) {
        auto it = std::find_if(r.evidence.begin(), r.evidence.end(),
                               [&](const auto &ref) { return ref.name == spec.name; });
        check(it != r.evidence.end() && names.insert(spec.name).second && it->finalized &&
                  it->absolute_path == spec.absolute_path && it->byte_count <= spec.max_bytes &&
                  it->byte_count <= file_limit && it->digest != lx::Sha256{},
              "child required reference differs from declaration");
        auto path = relative_path(out, fs::path{it->absolute_path});
        check(path.starts_with(child.path + "/") && paths.insert(path).second,
              "child evidence escapes its directory or duplicates a path");
        File file{it->name, spec.format, std::move(path), it->byte_count, hex(it->digest)};
        verify_file(out, file, io);
        child.files.push_back(std::move(file));
    }
    for (const auto &s : child.summaries) {
        for (const auto ext : {"html", "json", "csv"}) {
            const auto name = "report-" + std::to_string(s.trial) + "-" + ext;
            check(std::any_of(child.files.begin(), child.files.end(),
                              [&](const auto &file) {
                                  return file.name == name &&
                                         file.path == child.path + "/report-" +
                                                          std::to_string(s.trial) + "." + ext &&
                                         file.format == "quadeplus_report_evidence_" +
                                                            std::string{ext} + "_v1";
                              }),
                  "required per-trial report has not been integrated before acceptance");
        }
    }
    for (const auto name :
         {"financial.txt", "effective-config.xml", "graph.xml", "summary.json", "retention.log"}) {
        check(paths.contains(child.path + "/" + name), "child lacks required basic evidence");
    }
    if (child.profile != "keltner") {
        check(paths.contains(child.path + "/input-identity.txt"),
              "child lacks its declared input identity evidence");
    }
    auto config = engine(io.read(out / child.path / "effective-config.xml", 65536));
    check(hash(config.value) == child.config_sha,
          "child effective config differs from suite effective config");
}
} // namespace

BacktestResult<SuiteRun> suite(const fs::path &configPath, const fs::path &datasetPath,
                               const fs::path &out, const lx::HostBuild &build) noexcept {
    return detail::guard([&]() -> BacktestResult<SuiteRun> {
        path_ok(configPath);
        path_ok(datasetPath);
        require_new_output(out);
        path_ok(out);
        auto account = resources(true);
        auto metadata = engine(account->reserve(lx::ResourceKind::Graph, 16 * mib));
        const auto io = publication::native_io(account);
        auto xml = engine(lx::files::read(configPath, account, 65536));
        auto configuration = parse_suite(configPath, xml.value);
        const auto native = build_json(build), config_sha = hash(xml.value);
        std::string manifest_pin;
        {
            auto dataset = take(
                research::load_daily_dataset({datasetPath, {}, {16 * mib, 8000, 64}, resources()}));
            manifest_pin = dataset.manifest_sha256();
        }
        SuiteRun result{SuiteStatus::Rejected, 0, manifest_pin, {}, {}};
        check(fs::create_directory(out), "cannot exclusively create suite output");
        std::string active_child;
        try {
            check(fs::create_directory(out / "configs"), "cannot create suite config directory");
            check(fs::create_directory(out / "runs"), "cannot create suite runs directory");
            std::vector<File> aggregate;
            aggregate.push_back(write_file(out, "suite.xml", xml.value, io));
            std::vector<Child> children;
            children.reserve(12);
            for (const auto &p : configuration) {
                aggregate.push_back(
                    write_file(out, "configs/source-" + p.name + ".xml", p.config.xml, io));
                for (int cost : costs) {
                    const auto id = p.name + "-" + std::to_string(cost);
                    active_child = "runs/" + id;
                    Child child{p.name, "runs/" + id, "configs/" + id + ".xml", {}, {}, cost, {},
                                {}};
                    const auto effective = effective_xml(p.config, cost);
                    child.config_sha = hash(effective);
                    aggregate.push_back(write_file(out, child.config, effective, io));
                    // Scope releases financial trace, immutable dataset and its resource journal
                    // before the next independent child is loaded.
                    {
                        auto config = take(read_config(out / child.config));
                        auto dataset =
                            take(research::load_daily_dataset({datasetPath,
                                                               manifest_pin,
                                                               {16 * mib, 8000, 64},
                                                               resources(p.name == "keltner")}));
                        auto run_result = take(run(dataset, config, out / child.path, build));
                        close_child(child, run_result, out, io);
                    }
                    children.push_back(std::move(child));
                    ++result.completed_children;
                }
            }
            std::ostringstream registry, comparison, html;
            registry << "profile,cost_bps,trial,strategy,equity_cents,pnl_cents,realized_cents,"
                        "unrealized_cents,dividend_cents,fee_cents,report\n";
            comparison << "profile,cost_bps,trial,strategy,equity_cents,delta_vs_base_cents,delta_"
                          "vs_cash_cents,delta_vs_buy_hold_cents\n";
            html << "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>Backtest "
                    "suite</title><h1>Backtest suite</h1>"
                 << "<p>Previously inspected research, not a holdout. Fixed demonstrations, no "
                    "winner selection. Costs are commission + adverse slippage per fill. "
                 << "Legacy 20+20 is diagnostic; no original archived float-ledger parity target. "
                    "Cash/buy-and-hold are not risk-matched to Keltner.</p>"
                 << "<p>Fresh accounts are continuous from 2010, marked at the endpoint. Legacy is "
                    "the independent funded 43-session cohort with synthetic final-close "
                    "liquidation.</p>"
                 << "<a href=\"registry.csv\">Registry</a> <a href=\"cost-comparison.csv\">Cost "
                    "comparison</a><ul>";
            for (const auto &c : children) {
                const auto base =
                    std::find_if(children.begin(), children.end(), [&](const auto &b) {
                        return b.profile == c.profile && b.cost == 5;
                    });
                for (std::size_t i = 0; i < c.summaries.size(); ++i) {
                    const auto &s = c.summaries[i];
                    const auto report = c.path + "/report-" + std::to_string(s.trial) + ".html";
                    registry << c.profile << ',' << c.cost << ',' << s.trial << ',' << s.strategy
                             << ',' << s.equity << ',' << s.pnl << ',' << s.realized << ','
                             << s.unrealized << ',' << s.dividends << ',' << s.fees << ',' << report
                             << '\n';
                    comparison << c.profile << ',' << c.cost << ',' << s.trial << ',' << s.strategy
                               << ',' << s.equity << ','
                               << detail::sub(s.equity, base->summaries[i].equity) << ','
                               << detail::sub(s.equity, c.summaries[c.summaries.size() - 2].equity)
                               << ',' << detail::sub(s.equity, c.summaries.back().equity) << '\n';
                    html << "<li><a href=\"" << report << "\">" << c.profile << " " << c.cost << "+"
                         << c.cost << " bps / trial " << s.trial << "</a></li>";
                }
            }
            html << "</ul></html>\n";
            publication::Aggregates final_files{
                publication::aggregate("registry.csv", registry.str()),
                publication::aggregate("cost-comparison.csv", comparison.str()),
                publication::aggregate("index.html", html.str()), publication::Aggregate{}};
            for (std::size_t i = 0; i < 3; ++i) {
                aggregate.push_back(final_files[i].expected);
            }
            const std::string identity = "\"input_manifest_sha256\":" + quote(manifest_pin) +
                                         ",\"suite_config_sha256\":" + quote(config_sha) +
                                         ",\"native_build\":" + native;
            std::string manifest = "{\"schema\":\"quadeplus-suite-manifest\",\"version\":1," +
                                   identity + ",\"children\":[";
            std::vector<File> all = aggregate;
            for (std::size_t i = 0; i < children.size(); ++i) {
                if (i) {
                    manifest += ',';
                }
                manifest += child_json(children[i]);
                all.insert(all.end(), children[i].files.begin(), children[i].files.end());
            }
            manifest += "],\"aggregate_files\":" + files_json(aggregate) + "}\n";
            final_files[3] = publication::aggregate("manifest.json", std::move(manifest));
            result.manifest_sha256 = final_files[3].expected.sha;
            all.push_back(final_files[3].expected);
            const std::string marker =
                "{\"schema\":\"quadeplus-suite-acceptance\",\"version\":1,\"status\":\"Accepted\","
                "\"cache\":\"disabled\",\"new_cache_origins\":0," +
                identity + ",\"manifest_sha256\":" + quote(result.manifest_sha256) +
                ",\"required_files\":" + files_json(all) + "}\n";
            // Prepare both terminal results before the publication attempt. After confirmed
            // success there is no further required I/O or potentially allocating work.
            SuiteRun unresolved{SuiteStatus::Unconfirmed, result.completed_children, manifest_pin,
                                result.manifest_sha256,
                                "suite marker publication visibility is unresolved; inspect "
                                "retained final/pending paths"};
            static_assert(std::is_nothrow_constructible_v<BacktestResult<SuiteRun>, SuiteRun &&>);
            const auto published = publication::finalize(out, all, final_files, marker, io);
            if (published.status == SuiteStatus::Accepted) {
                result.status = SuiteStatus::Accepted;
                return BacktestResult<SuiteRun>{std::move(result)};
            }
            if (published.status == SuiteStatus::Unconfirmed) {
                return BacktestResult<SuiteRun>{std::move(unresolved)};
            }
            fail(ErrorCode::IO, publication::failure_text(published.stage));
        } catch (const detail::Failure &error) {
            result.status = SuiteStatus::Rejected;
            result.diagnostic = error.value.text();
        } catch (const std::exception &error) {
            result.status = SuiteStatus::Rejected;
            result.diagnostic = error.what();
        } catch (...) {
            result.status = SuiteStatus::Rejected;
            result.diagnostic = "suite stopped on an unknown prepublication failure";
        }
        // Best effort only. All child prefixes remain untouched. This is never an
        // acceptance artifact and failures here cannot transform rejection to success.
        try {
            static_cast<void>(write_file(out, "failure.json",
                                         "{\"schema\":\"quadeplus-suite-failure\",\"version\":1,"
                                         "\"status\":\"Rejected\",\"completed_children\":" +
                                             std::to_string(result.completed_children) +
                                             ",\"active_child\":" + quote(active_child) +
                                             ",\"input_manifest_sha256\":" + quote(manifest_pin) +
                                             ",\"suite_config_sha256\":" + quote(config_sha) +
                                             ",\"native_build\":" + native +
                                             ",\"reason\":" + quote(result.diagnostic) + "}\n",
                                         io));
        } catch (...) {
        }
        return result;
    });
}
} // namespace quant::backtest::app
