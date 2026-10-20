#pragma once
#include "quant/backtest/host.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/research/daily_source.hpp"
#include "quant/storage/file_integrity.hpp"
#include "host_identity.hpp"
#include "file_result_store.hpp"
#include "file_evidence.hpp"
#include "xml_frontend.hpp"
#include "metadata_xml.hpp"
#include "native_files.hpp"
#include <iostream>
#include <sstream>
#include <unistd.h>

namespace backtest_host_test {
namespace lx = ::lexongine;
namespace qb = quant::backtest;
namespace qr = quant::research;
namespace qd = quant::data;
template <class T, class E> T take(std::variant<T, E> value) {
    if (const auto *e = std::get_if<E>(&value)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(value));
}
inline void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string{message});
    }
}
template <class T, class E> void rejects(const std::variant<T, E> &value) {
    require(std::holds_alternative<E>(value), "expected rejection");
}
inline std::filesystem::path root;
inline std::optional<lx::HostBuild> inspected_build;
inline std::shared_ptr<lx::ResourceAccount> account() {
    return take(lx::ResourceAccount::create(
        {{134217728, 16777216, 536870912, 67108864, 16777216, 268435456, 67108864},
         8192,
         4096,
         16384,
         128}));
}
inline qd::Money money(std::int64_t n) {
    return qd::Money::from_raw(qd::CurrencyCode{"USD"}, n);
}
inline std::filesystem::path directory() {
    auto pattern = (root / "host.XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    require(::mkdtemp(bytes.data()) != nullptr, "host scratch");
    return bytes.data();
}
inline qr::DailyDataset dataset(const std::filesystem::path &path, bool future_changed = false) {
    const std::string grid = "Date\n2026-01-02\n2026-01-05\n2026-01-06\n2026-01-07\n2026-01-08\n";
    const std::string csv =
        "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n2026-01-02,100,100,100,100,1,100,"
        "0\n2026-01-05,100,100,100,100,1,100,0\n2026-01-06,110," +
        std::string{future_changed ? "500,1,5" : "110,110,110"} +
        ",1,110,1.889\n2026-01-07,90,90,90,90,1,90,0\n2026-01-08,80,80,80,80,1,80,0\n";
    quant::storage::files::write_text(path / "prices.csv", csv);
    quant::storage::files::write_text(path / "sessions.csv", grid);
    qr::DatasetSpec spec{"host-fixture-v1",
                         "TEST",
                         "USD",
                         "authored-host-fixture",
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
    return take(qr::prepare_daily_dataset({path / "prices.csv",
                                           path / "sessions.csv",
                                           path / "bundle",
                                           quant::storage::files::sha256(csv),
                                           quant::storage::files::sha256(grid),
                                           spec,
                                           {1048576, 8, 64},
                                           account()}));
}
inline lx::MetadataLimits metadata_limits() {
    return {{16777216, 1048576, 64, 65536, 67108864}, 33554432};
}
struct Environment {
    std::filesystem::path path = directory();
    qr::DailyDataset data;
    lx::HostBuild build;
    lx::Registry registry = take(lx::Registry::create());
    quant::lexongine::ComponentTypes types;
    lx::SourceResolver resolver;
    qr::RegisteredDailySource source;
    Environment(bool changed = false)
        : data(dataset(path, changed)), build(*inspected_build),
          types(take(quant::lexongine::register_components(
              registry, {build.identity(), {}, lx::MissingIntervalPolicy::Strict}))),
          resolver(take(lx::SourceResolver::create(data.account(), 65536, 4))),
          source(take(qr::register_daily_source(
              registry, resolver, types.bar, data,
              {{5, data.owned_bytes(), 262144, 65536, 1024},
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
    qb::SuiteConfiguration suite(std::uint64_t bytes = 1048576) {
        qb::HostLimits limits{2, 32, 128, bytes};
        return {0,
                1,
                5,
                false,
                limits,
                qb::financial_evidence_spec((path / "ledger.txt").string(), limits),
                {{1,
                  "manual_targets_v1",
                  "long_from_close_1",
                  money(1000000),
                  qd::Quantity::from_raw(1),
                  {5, 5},
                  {},
                  qb::TerminalMode::Mark,
                  {{"A", 0, 5}}},
                 {2,
                  "manual_cash_v1",
                  "cash_only",
                  money(2000000),
                  qd::Quantity::from_raw(1),
                  {5, 5},
                  {},
                  qb::TerminalMode::Mark,
                  {{"B", 0, 5}}}}};
    }
    std::shared_ptr<const lx::PreparedPlan>
    prepare(const qb::SuiteConfiguration &suite, bool partial = false,
            std::shared_ptr<lx::ResultStore> store = {},
            std::span<const lx::RequiredEvidenceSpec> extra_evidence = {}) {
        const auto clock = std::string{qr::clock_profile};
        std::ostringstream xml;
        xml << "<composition version=\"1\"><run id=\"financial-host-test\" clock=\"" << clock
            << "\" driver=\"hosted\" execution=\"synchronous\" sharing=\"exact\" failure=\"stop\" "
               "trace=\"steps\" retention_warning_steps=\"128\" retention_log_path=\""
            << (path / "retention.log").string()
            << "\" retention_log_format=\"key_value_lines_v1\" retention_log_open=\"create_new\" "
               "retention_log_flush=\"on_event\" retention_log_failure=\"stop\" "
               "retention_log_overflow=\"stop\" retention_log_max_bytes=\"4194304\" "
               "retention_log_max_events=\"32768\"/>"
            << "<limits xml_bytes=\"1048576\" xml_depth=\"16\" xml_elements=\"1024\" "
               "string_bytes=\"1024\" nodes=\"32\" edges=\"64\" requests=\"16\" "
               "source_updates=\"5\" managed_buffer_bytes=\"536870912\" trace_events=\"4096\" "
               "trace_message_bytes=\"256\"/><plugins/>";
        if (store) {
            xml << "<cache mode=\"automatic_read_write\" backend=\"local_files\" directory=\""
                << (path / "cache").string()
                << "\" matching=\"exact_full_scope\" trust=\"same_build_local_writer\" "
                   "integrity=\"sha256\" writer=\"single_process_lock\" "
                   "corruption=\"fail_preparation\" capacity=\"bypass_new_entries\" "
                   "cleanup=\"explicit_only\" max_entries=\"64\" max_store_bytes=\"8388608\" "
                   "max_entry_bytes=\"1048576\" max_decoded_bytes=\"1048576\"/>";
        } else {
            xml << "<cache mode=\"disabled\"/>";
        }
        xml << "<sources><source id=\"s\" operation=\"quadeplus.daily_research\" "
               "semantic_version=\"1\" implementation_id=\"sealed-daily-source-v1\" "
               "source_version=\""
            << data.manifest_sha256() << "\" clock=\"" << clock
            << "\" begin_step=\"0\" end_step=\"5\" completeness=\"complete_only\" empty=\"reject\" "
               "missing=\"fail\" missing_intervals=\"strict\" availability=\""
            << clock
            << "\"/></sources><nodes><node id=\"close\" operation=\"qaedalus.bar_close\" "
               "semantic_version=\"1\" implementation_id=\"builtin-v1\" alignment=\"same_step\" "
               "initialization=\"stateless\"><input port=\"bar\" producer=\"s\" output=\"bar\" "
               "prior_outputs=\"0\" missing_intervals=\"strict\"/></node>";
        if (partial) {
            xml << "<node id=\"late\" operation=\"statistics.sma_price\" semantic_version=\"1\" "
                   "implementation_id=\"builtin-v1\" alignment=\"same_step\" "
                   "initialization=\"empty_at_step_0\"><param name=\"window\" value=\"20\"/><param "
                   "name=\"max_abs_input_raw\" value=\"1000000000000\"/><param name=\"sum_width\" "
                   "value=\"signed64\"/><param name=\"rounding\" "
                   "value=\"nearest_ties_even\"/><param name=\"overflow\" value=\"fail\"/><input "
                   "port=\"price\" producer=\"close\" output=\"price\" prior_outputs=\"0\" "
                   "missing_intervals=\"strict\"/></node>";
        }
        xml << "</nodes><observations><observe id=\"A\" producer=\"close\" output=\"price\" "
               "begin_step=\"0\" end_step=\"5\" absence=\"require_value\" prior_outputs=\"0\" "
               "missing_intervals=\"strict\"/><observe id=\"B\" producer=\""
            << (partial ? "late" : "close") << "\" output=\"" << (partial ? "mean" : "price")
            << "\" begin_step=\"0\" end_step=\"5\" absence=\"require_value\" prior_outputs=\"0\" "
               "missing_intervals=\"strict\"/></observations></composition>";
        auto parsed = take(lx::parse_xml(xml.str(), {1048576, 32, 16384, 1024, data.account()}));
        auto composition = take(lx::resolve_composition(parsed, registry));
        lx::PrepareOptions options{
            "financial-host-test",
            take(source.request({clock, 0, 5})),
            {data.account(),
             {{5, 5, 0, 5}, 1024, 65536, 262144, 65536},
             lx::MissingIntervalPolicy::Strict,
             {data.owned_bytes(), 65536, 0, 8192, sizeof(qd::Bar) + 512, 524288, 0}},
            {{0, 1048576, 1048576, 4194304, 1048576, 4194304, 8388608}, 4096, 4096, 16384, 128},
            {1048576, 1048576, 1048576, 1048576, 1048576, 65536, 1048576},
            262144,
            1048576,
            [](std::span<const std::byte> bytes) { return lx::files::digest(bytes); },
            {}};
        options.evidence = {suite.financial_evidence,
                            {"snapshot", "run_snapshot_v1", (path / "snapshot.xml").string(),
                             "create_new", 1048576, 128, "on_close", "stop"}};
        options.evidence.insert(options.evidence.end(), extra_evidence.begin(),
                                extra_evidence.end());
        options.evidence_account = data.account();
        options.evidence_resources = {0, 0, 134217728, 1048576, 0, 33554432, 16777216};
        auto result = take(lx::prepare(composition, registry, resolver, std::move(store), options));
        require(result.plan != nullptr, "no prepared financial graph");
        return result.plan;
    }
    lx::ExecutionEvidence retention(const std::shared_ptr<const lx::PreparedPlan> &p) {
        std::vector<std::string> names;
        for (const auto &node : p->nodes()) {
            names.push_back(node.spec.id);
        }
        return take(lx::FileEvidence::create(p->evidence_specification(), "financial-host-test",
                                             std::string{qr::clock_profile}, std::move(names),
                                             data.account(), {65536, 8192, 16777216}))
            .callbacks();
    }
    std::vector<lx::RequiredEvidenceSink> extra(const std::shared_ptr<const lx::PreparedPlan> &p) {
        return {take(lx::SnapshotEvidence::create(p, p->required_evidence()[1], data.account(),
                                                  {metadata_limits(), 1048576, 4096}))
                    .callbacks()};
    }
};
inline qb::Action hold() {
    return {qb::ActionKind::Hold, qd::Quantity::from_raw(0), "hold"};
}
inline std::vector<qb::DecisionCallback> callbacks() {
    return {
        [](const qb::DecisionContext &c, const qb::Account *,
           std::span<const qb::PriceFeature> f) -> qb::BacktestResult<qb::Action> {
            if (c.may_submit) {
                require(f.size() == 1 && f[0].key == c.key, "current feature receipt missing");
                return qb::Action{qb::ActionKind::TargetLong, qd::Quantity::from_raw(0),
                                  "target_long"};
            }
            return hold();
        },
        [](const auto &, const auto *, auto) -> qb::BacktestResult<qb::Action> { return hold(); }};
}
} // namespace backtest_host_test
