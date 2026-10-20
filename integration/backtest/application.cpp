#include "application.hpp"
#include "report_evidence.hpp"
#include "quant/strategies/policies.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/research/daily_source.hpp"
#include "quant/storage/file_integrity.hpp"
#include "file_evidence.hpp"
#include "native_files.hpp"
#include "xml_frontend.hpp"
#include <tinyxml2.h>
#include <charconv>
#include <fcntl.h>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>

namespace quant::backtest::app {
namespace {
namespace lx = ::lexongine;
namespace qr = quant::research;
using detail::fail;
using detail::take;
constexpr std::uint64_t mb = 1048576;
// Account journals retain every resource category; retention.log exports only
// Retention events. Its line count is not the runtime journal capacity demand.
constexpr std::uint64_t audit_events = 524288;
constexpr std::string_view package_pin =
    "a907a8bb072405ef0bbb56b54e1e0cd6479e524880199c2258ed3dd3696e9bc4";
constexpr std::string_view spy_pin =
    "28963337b6eba735cad1eb62dc333d497079cff73f02076d17d3439c48e167ae";
template <class T> T engine(std::variant<T, lx::Diagnostic> result) {
    if (auto *e = std::get_if<lx::Diagnostic>(&result)) {
        fail(ErrorCode::SourceBinding, e->text());
    }
    return std::get<T>(std::move(result));
}
std::string hex(const lx::Sha256 &value) {
    constexpr char d[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : value) {
        auto n = std::to_integer<unsigned>(b);
        out += d[n >> 4];
        out += d[n & 15];
    }
    return out;
}
std::string hash(std::string_view text) {
    return storage::files::sha256(std::as_bytes(std::span{text.data(), text.size()}));
}
void path_ok(const std::filesystem::path &path) {
    if (!path.is_absolute() || path.string().find_first_of("\r\n\t") != std::string::npos) {
        fail(ErrorCode::InvalidConfig, "paths must be absolute and contain no control whitespace");
    }
    storage::files::no_symlink(path);
}
void new_output(const std::filesystem::path &out) {
    path_ok(out);
    if (std::filesystem::exists(std::filesystem::symlink_status(out)) ||
        !std::filesystem::is_directory(out.parent_path())) {
        fail(ErrorCode::InvalidConfig, "output must be new beneath an existing directory");
    }
}
std::int64_t integer(std::string_view text) {
    std::int64_t value{};
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        std::to_string(value) != text) {
        fail(ErrorCode::InvalidConfig, "expected a canonical signed64 integer");
    }
    return value;
}
std::string escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out += c;
        }
    }
    return out;
}
void verify_package(const std::filesystem::path &root,
                    const std::shared_ptr<lx::ResourceAccount> &account) {
    auto metadata = engine(account->reserve(lx::ResourceKind::Graph, 4 * mb));
    path_ok(root);
    auto manifest = engine(lx::files::read(root / "package_manifest.json", account, mb));
    if (hash(manifest.value) != package_pin) {
        fail(ErrorCode::Integrity, "extracted package manifest differs from the retained ZIP pin");
    }
    std::istringstream input{manifest.value};
    char token = 0;
    input >> token;
    if (token != '{') {
        fail(ErrorCode::Integrity, "invalid pinned package manifest");
    }
    std::set<std::string> paths;
    for (;;) {
        input >> std::ws;
        if (input.peek() == '}') {
            input.get();
            break;
        }
        std::string name, digest;
        if (input.peek() != '"' || !(input >> std::quoted(name) >> token) || token != ':') {
            fail(ErrorCode::Integrity, "invalid package manifest entry");
        }
        input >> std::ws;
        if (input.peek() != '"' || !(input >> std::quoted(digest))) {
            fail(ErrorCode::Integrity, "invalid package digest");
        }
        std::filesystem::path relative{name};
        if (name.empty() || relative.is_absolute() || name.find('\\') != std::string::npos ||
            !paths.insert(name).second || paths.size() > 372 || digest.size() != 64) {
            fail(ErrorCode::Integrity, "invalid package file identity");
        }
        for (const auto &part : relative) {
            if (part == ".." || part == ".") {
                fail(ErrorCode::Integrity, "unsafe package pathname");
            }
        }
        path_ok(root / relative);
        auto bytes = engine(lx::files::read(root / relative, account, 64 * mb));
        if (hash(bytes.value) != digest) {
            fail(ErrorCode::Integrity, "retained package member hash mismatch");
        }
        input >> token;
        if (token == '}') {
            break;
        }
        if (token != ',') {
            fail(ErrorCode::Integrity, "invalid package manifest separator");
        }
    }
    input >> std::ws;
    if (!input.eof() || paths.size() != 372 || !paths.contains("data/SPY.csv") ||
        !paths.contains("protocol.json") || !paths.contains("engine.py")) {
        fail(ErrorCode::Integrity, "incomplete pinned package inventory");
    }
}
struct DecisionRow {
    std::uint64_t key;
    bool allowed, funded;
    std::optional<std::int64_t> fast, slow;
    ActionKind action;
    std::string reason;
};
struct EvidenceState {
    lx::Reservation charge;
    std::vector<DecisionRow> decisions;
    std::shared_ptr<const FinancialRun> financial;
    std::shared_ptr<std::vector<Summary>> summaries = std::make_shared<std::vector<Summary>>();
    std::uint64_t begin = 0, end = 0;
};
lx::RequiredEvidenceSpec spec(std::string name, std::string format,
                              const std::filesystem::path &path, std::uint64_t bytes,
                              std::uint64_t records) {
    return {std::move(name), std::move(format), path.string(), "create_new",
            bytes,           records,           "on_close",    "stop"};
}
lx::RequiredEvidenceSink report_sink(lx::RequiredEvidenceSpec specification,
                                     const std::shared_ptr<lx::ResourceAccount> &account,
                                     std::function<std::string(const lx::RunReport &)> serialize) {
    struct State {
        lx::RequiredEvidenceSpec spec;
        std::shared_ptr<lx::ResourceAccount> account;
        std::function<std::string(const lx::RunReport &)> serialize;
        int fd = -1;
        ~State() {
            if (fd >= 0) {
                static_cast<void>(::close(fd));
            }
        }
    };
    auto s = std::make_shared<State>(State{specification, account, std::move(serialize)});
    return {{},
            std::move(specification),
            [s]() -> lx::Result<void> {
                s->fd = ::open(s->spec.absolute_path.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (s->fd < 0) {
                    return lx::diagnostic(lx::FailureCode::EvidenceIO,
                                          "required application evidence create-new failed");
                }
                return lx::ok();
            },
            {},
            {},
            [s](const lx::RunReport &report) -> lx::Result<lx::EvidenceReference> {
                return lx::guarded([&]() -> lx::Result<lx::EvidenceReference> {
                    try {
                        auto bytes = s->serialize(report);
                        if (bytes.size() > s->spec.max_bytes) {
                            return lx::diagnostic(lx::FailureCode::EvidenceQuota,
                                                  "application evidence byte limit exceeded");
                        }
                        return lx::files::finish_open(s->fd, s->spec.name, s->spec.absolute_path,
                                                      bytes, s->account, s->spec.max_bytes,
                                                      lx::FailureCode::EvidenceIO);
                    } catch (const detail::Failure &error) {
                        return lx::diagnostic(lx::FailureCode::OutputInvalid, error.value.text());
                    }
                });
            }};
}
lx::RequiredEvidenceSink sink(lx::RequiredEvidenceSpec s,
                              const std::shared_ptr<lx::ResourceAccount> &a,
                              std::function<std::string()> f) {
    return report_sink(std::move(s), a, [f = std::move(f)](const lx::RunReport &) { return f(); });
}
std::string summary_json(EvidenceState &state, const Config &config, std::string_view manifest) {
    if (!state.financial->finalized) {
        return "{\"format\":\"quadeplus_s1_summary_v1\",\"financial_state\":\"partial\","
               "\"evaluation_sessions\":null,\"trials\":[]}\n";
    }
    std::ostringstream out;
    out << "{\"format\":\"quadeplus_s1_summary_v1\",\"financial_state\":\""
        << (state.financial->finalized ? "completed" : "partial") << "\",\"dataset_sha256\":\""
        << manifest << "\",\"experiment\":\"" << hex(state.financial->experiment)
        << "\",\"profile\":\"fresh_research_v1\",\"window_status\":\"previously_inspected_research_"
           "not_holdout\",\"evaluation_sessions\":"
        << state.end - state.begin << ",\"trials\":[";
    const std::string names[]{std::string{strategies::sma_strategy_id}, "cash_usd_no_interest_v1",
                              "buy_hold95_next_open_v1"};
    for (std::size_t i = 0; i < state.financial->trials.size(); ++i) {
        const auto &t = state.financial->trials[i];
        if (!t.account) {
            continue;
        }
        const auto &a = *t.account;
        std::int64_t peak = config.initial_cash, loss = 0, loss_peak = peak,
                     final_equity = config.initial_cash;
        for (const auto &event : state.financial->events) {
            if (event.trial != a.trial) {
                continue;
            }
            if (event.kind == "final" && event.equity) {
                final_equity = *event.equity;
            }
            if (event.kind != "close" || event.key < state.begin || !event.equity) {
                continue;
            }
            peak = std::max(peak, *event.equity);
            const auto current = peak - *event.equity;
            if (take(ratio_greater(current, peak, loss, loss_peak))) {
                loss = current;
                loss_peak = peak;
            }
        }
        if (i) {
            out << ',';
        }
        Summary summary{a.trial,
                        names[i],
                        a.cash.amount(),
                        a.units.raw(),
                        final_equity,
                        detail::sub(final_equity, config.initial_cash),
                        a.realized.amount(),
                        detail::sub(detail::sub(final_equity, a.cash.amount()), a.basis.amount()),
                        a.dividends.amount(),
                        a.fees.amount(),
                        loss,
                        loss_peak};
        state.summaries->push_back(summary);
        out << "{\"id\":" << a.trial << ",\"strategy\":\"" << names[i]
            << "\",\"cash_cents\":" << summary.cash << ",\"units_raw\":" << summary.units
            << ",\"equity_cents\":" << summary.equity << ",\"pnl_cents\":" << summary.pnl
            << ",\"realized_cents\":" << summary.realized
            << ",\"unrealized_cents\":" << summary.unrealized
            << ",\"dividend_cents\":" << summary.dividends << ",\"fee_cents\":" << summary.fees
            << ",\"drawdown_loss_cents\":" << loss << ",\"drawdown_peak_cents\":" << loss_peak
            << '}';
    }
    out << "]}\n";
    return out.str();
}
std::string graph(const qr::DailyDataset &data, const Config &config,
                  const std::filesystem::path &out) {
    const auto count = data.rows().size();
    const auto clock = std::string{qr::clock_profile};
    std::ostringstream xml;
    xml << "<composition version=\"1\"><run id=\"s1-fresh\" clock=\"" << clock
        << "\" driver=\"hosted\" execution=\"synchronous\" sharing=\"" << config.sharing
        << "\" failure=\"stop\" trace=\"steps\" retention_warning_steps=\"128\" "
           "retention_log_path=\""
        << escape((out / "retention.log").string())
        << "\" retention_log_format=\"key_value_lines_v1\" retention_log_open=\"create_new\" "
           "retention_log_flush=\"on_event\" retention_log_failure=\"stop\" "
           "retention_log_overflow=\"stop\" retention_log_max_bytes=\"536870912\" "
           "retention_log_max_events=\"4194304\"/>"
        << "<limits xml_bytes=\"1048576\" xml_depth=\"16\" xml_elements=\"1024\" "
           "string_bytes=\"4096\" nodes=\"32\" edges=\"64\" requests=\"16\" source_updates=\""
        << count
        << "\" managed_buffer_bytes=\"4294967296\" trace_events=\"16384\" "
           "trace_message_bytes=\"256\"/><plugins/><cache mode=\"disabled\"/><sources><source "
           "id=\"s\" operation=\"quadeplus.daily_research\" semantic_version=\"1\" "
           "implementation_id=\"sealed-daily-source-v1\" source_version=\""
        << data.manifest_sha256() << "\" clock=\"" << clock << "\" begin_step=\"0\" end_step=\""
        << count
        << "\" completeness=\"complete_only\" empty=\"reject\" missing=\"fail\" "
           "missing_intervals=\"strict\" availability=\""
        << clock
        << "\"/></sources><nodes><node id=\"close\" operation=\"qaedalus.bar_close\" "
           "semantic_version=\"1\" implementation_id=\"builtin-v1\" alignment=\"same_step\" "
           "initialization=\"stateless\"><input port=\"bar\" producer=\"s\" output=\"bar\" "
           "prior_outputs=\"0\" missing_intervals=\"strict\"/></node>";
    for (const auto window : {20, 50}) {
        xml << "<node id=\"sma" << window
            << "\" operation=\"statistics.sma_price\" semantic_version=\"1\" "
               "implementation_id=\"builtin-v1\" alignment=\"same_step\" "
               "initialization=\"empty_at_step_0\"><param name=\"window\" value=\""
            << window << "\"/><param name=\"max_abs_input_raw\" value=\"" << config.max_price_raw
            << "\"/><param name=\"sum_width\" value=\"signed64\"/><param name=\"rounding\" "
               "value=\"nearest_ties_even\"/><param name=\"overflow\" value=\"fail\"/><input "
               "port=\"price\" producer=\"close\" output=\"price\" prior_outputs=\"0\" "
               "missing_intervals=\"strict\"/></node>";
    }
    xml << "</nodes><observations>";
    for (const auto window : {20, 50}) {
        xml << "<observe id=\"sma" << window << "\" producer=\"sma" << window
            << "\" output=\"mean\" begin_step=\"" << window - 1 << "\" end_step=\"" << count
            << "\" absence=\"require_value\" prior_outputs=\"0\" missing_intervals=\"strict\"/>";
    }
    xml << "</observations></composition>";
    return xml.str();
}
} // namespace
std::shared_ptr<lx::ResourceAccount> resources(bool keltner) {
    return engine(lx::ResourceAccount::create({{128 * mb, 64 * mb, 2048 * mb, 256 * mb, 256 * mb,
                                                2048 * mb, keltner ? 512 * mb : 256 * mb},
                                               131072,
                                               16384,
                                               keltner ? 4 * audit_events : audit_events,
                                               128}));
}
lx::RequiredEvidenceSink make_sink(lx::RequiredEvidenceSpec s,
                                   const std::shared_ptr<lx::ResourceAccount> &a,
                                   std::function<std::string()> f) {
    return sink(std::move(s), a, std::move(f));
}
lx::RequiredEvidenceSink make_report_sink(lx::RequiredEvidenceSpec s,
                                          const std::shared_ptr<lx::ResourceAccount> &a,
                                          std::function<std::string(const lx::RunReport &)> f) {
    return report_sink(std::move(s), a, std::move(f));
}
std::string escape_xml(std::string_view text) {
    return escape(text);
}
void require_new_output(const std::filesystem::path &path) {
    new_output(path);
}
BacktestResult<Config> read_config(const std::filesystem::path &path) noexcept {
    return detail::guard([&]() -> BacktestResult<Config> {
        path_ok(path);
        auto account = resources();
        auto bytes = engine(lx::files::read(path, account, 65536));
        auto charge = engine(account->reserve(lx::ResourceKind::Scratch, 4 * mb));
        tinyxml2::XMLDocument document;
        if (document.Parse(bytes.value.data(), bytes.value.size()) != tinyxml2::XML_SUCCESS) {
            fail(ErrorCode::InvalidConfig, "invalid domain XML");
        }
        const auto *root = document.RootElement();
        if (!root || std::string_view{root->Name()} != "backtest" || root->FirstChild()) {
            fail(ErrorCode::InvalidConfig, "expected an empty backtest configuration element");
        }
        for (auto *node = document.FirstChild(); node; node = node->NextSibling()) {
            if (node != root && !node->ToDeclaration()) {
                fail(ErrorCode::InvalidConfig, "unexpected domain XML node");
            }
        }
        std::map<std::string, std::string> attrs;
        for (auto *attribute = root->FirstAttribute(); attribute; attribute = attribute->Next()) {
            if (!attrs.emplace(attribute->Name(), attribute->Value()).second) {
                fail(ErrorCode::InvalidConfig, "duplicate domain attribute");
            }
        }
        const bool keltner =
            attrs.contains("strategy") && attrs.at("strategy") == "keltner_atr_risk_v1";
        const bool stochastic =
            attrs.contains("strategy") && attrs.at("strategy") == "spy_stochastic5_v1";
        const bool legacy =
            stochastic && attrs.contains("profile") && attrs.at("profile") == "legacy_spy_2026_v1";
        std::set<std::string> names{"version",
                                    "strategy",
                                    "profile",
                                    "symbol",
                                    "currency",
                                    "numerical_origin",
                                    "evaluation_begin",
                                    "evaluation_end",
                                    "initial_cash_cents",
                                    "lot_step_raw",
                                    "commission_bps",
                                    "slippage_bps",
                                    "allocation_bps",
                                    "fast_window",
                                    "slow_window",
                                    "max_price_raw",
                                    "terminal",
                                    "controls",
                                    "cache",
                                    "resource_profile"};
        if (keltner) {
            names.erase("fast_window");
            names.erase("slow_window");
            for (auto name : {"ema_window", "atr_window", "channel_widths", "drawdown_bps",
                              "sharing", "cache_directory"}) {
                names.insert(name);
            }
        }
        if (stochastic) {
            names.erase("fast_window");
            names.erase("slow_window");
            for (auto name :
                 {"window", "zero_range", "entry", "exit", "max_holding_bars", "sharing"}) {
                names.insert(name);
            }
        }
        if (!keltner && !stochastic && attrs.contains("sharing")) {
            names.insert("sharing");
        }
        if (attrs.size() != names.size()) {
            fail(ErrorCode::InvalidConfig, "missing or unknown domain attributes");
        }
        for (const auto &[name, value] : attrs) {
            if (!names.contains(name) || value.empty() ||
                value.size() > (name == "cache_directory" ? 4096U : 128U)) {
                fail(ErrorCode::InvalidConfig, "unsupported domain attribute");
            }
        }
        std::map<std::string, std::string> required{
            {"version", "1"},
            {"strategy", stochastic ? "spy_stochastic5_v1"
                         : keltner  ? "keltner_atr_risk_v1"
                                    : std::string{strategies::sma_strategy_id}},
            {"profile", legacy ? "legacy_spy_2026_v1" : "fresh_research_v1"},
            {"currency", "USD"},
            {"allocation_bps", "9500"},
            {"terminal", legacy ? "synthetic_close" : "mark"},
            {"controls", "cash,buy_hold"},
            {"resource_profile", stochastic ? "stochastic_cli_bounds_v1"
                                 : keltner  ? "keltner_cli_bounds_v1"
                                            : "sma_cli_bounds_v1"}};
        if (keltner) {
            required.insert({{"ema_window", "20"},
                             {"atr_window", "10"},
                             {"channel_widths", "3/2,2/1,5/2,2/1"},
                             {"drawdown_bps", "2000"}});
        } else if (stochastic) {
            required.insert({{"window", "5"},
                             {"zero_range", "1/2"},
                             {"entry", "20"},
                             {"exit", "80"},
                             {"max_holding_bars", "10"},
                             {"cache", "disabled"}});
        } else {
            required.insert({{"fast_window", "20"}, {"slow_window", "50"}, {"cache", "disabled"}});
        }
        for (const auto &[name, value] : required) {
            if (attrs.at(name) != value) {
                fail(ErrorCode::Unsupported, "unsupported financial domain policy");
            }
        }
        const auto commission = integer(attrs.at("commission_bps")),
                   slippage = integer(attrs.at("slippage_bps"));
        if (commission < 0 || commission >= 10000 || slippage < 0 || slippage >= 10000) {
            fail(ErrorCode::InvalidConfig, "invalid cost basis points");
        }
        Config result{attrs.at("symbol"),
                      attrs.at("numerical_origin"),
                      attrs.at("evaluation_begin"),
                      attrs.at("evaluation_end"),
                      bytes.value,
                      integer(attrs.at("initial_cash_cents")),
                      integer(attrs.at("lot_step_raw")),
                      integer(attrs.at("max_price_raw")),
                      {static_cast<std::int32_t>(commission), static_cast<std::int32_t>(slippage)}};
        if (keltner) {
            result.strategy = "keltner_atr_risk_v1";
            result.sharing = attrs.at("sharing");
            if (result.sharing != "exact" && result.sharing != "disabled") {
                fail(ErrorCode::Unsupported, "unsupported sharing mode");
            }
            if (attrs.at("cache") == "disabled") {
                if (attrs.at("cache_directory") != "none") {
                    fail(ErrorCode::InvalidConfig, "disabled cache requires directory=none");
                }
            } else if (attrs.at("cache") == "automatic_read_write" && result.sharing == "exact") {
                result.cache_directory = attrs.at("cache_directory");
                path_ok(result.cache_directory);
            } else {
                fail(ErrorCode::Unsupported, "unsupported Keltner cache mode");
            }
        }
        if (stochastic) {
            result.strategy = "spy_stochastic5_v1";
            result.profile = attrs.at("profile");
            result.terminal = legacy ? TerminalMode::SyntheticClose : TerminalMode::Mark;
            result.sharing = attrs.at("sharing");
            if (result.sharing != "exact" && result.sharing != "disabled") {
                fail(ErrorCode::Unsupported, "unsupported stochastic sharing mode");
            }
        }
        if (!keltner && !stochastic && attrs.contains("sharing")) {
            result.sharing = attrs.at("sharing");
            if (result.sharing != "exact" && result.sharing != "disabled") {
                fail(ErrorCode::Unsupported, "unsupported SMA sharing mode");
            }
        }
        if (result.initial_cash <= 0 || result.lot <= 0 || result.max_price_raw <= 0 ||
            result.max_price_raw > INT64_MAX / 50) {
            fail(ErrorCode::InvalidConfig, "invalid funding, lot or SMA sum bound");
        }
        return result;
    });
}
BacktestResult<qr::DailyDataset> prepare_spy(const std::filesystem::path &root,
                                             const std::filesystem::path &out) noexcept {
    return detail::guard([&]() -> BacktestResult<qr::DailyDataset> {
        new_output(out);
        auto account = resources();
        verify_package(root, account);
        struct Scratch {
            std::filesystem::path path;
            ~Scratch() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        };
        auto pattern = (out.parent_path() / ".s1-prepare.XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        if (!::mkdtemp(name.data())) {
            fail(ErrorCode::IO, "preparation scratch creation failed");
        }
        Scratch scratch{std::filesystem::path{name.data()}};
        qr::DatasetLimits limits{16 * mb, 8000, 64};
        auto grid = take(qr::seal_reference_grid(root / "data/SPY.csv", spy_pin,
                                                 scratch.path / "sessions.csv", limits, account));
        qr::DatasetSpec spec{"retained-spy-cli-v1",
                             "SPY",
                             "USD",
                             "retained_package_sha256:" + std::string{package_pin},
                             1,
                             1,
                             1,
                             "retained_reference_grid_v1",
                             std::string{qr::adjustment_profile},
                             std::string{qr::decimal_profile},
                             std::string{qr::volume_profile},
                             std::string{qr::clock_profile},
                             "2004-11-18",
                             "2010-01-04",
                             "2026-10-15"};
        return qr::prepare_daily_dataset({root / "data/SPY.csv", scratch.path / "sessions.csv", out,
                                          std::string{spy_pin}, grid, spec, limits, account});
    });
}
BacktestResult<ApplicationRun> run(const qr::DailyDataset &data, const Config &config,
                                   const std::filesystem::path &out,
                                   const lx::HostBuild &build) noexcept {
    return detail::guard([&]() -> BacktestResult<ApplicationRun> {
        if (config.strategy == "keltner_atr_risk_v1") {
            return run_keltner(data, config, out, build);
        }
        if (config.strategy == "spy_stochastic5_v1") {
            return run_stochastic(data, config, out, build);
        }
        new_output(out);
        const auto &ds = data.specification();
        const auto n = data.rows().size();
        if (n < 50 || n > 8000 || ds.symbol != config.symbol ||
            ds.numerical_origin != config.origin || ds.evaluation_begin != config.begin ||
            ds.evaluation_end != config.end) {
            fail(ErrorCode::InvalidConfig,
                 "S1 config and immutable dataset range/identity disagree");
        }
        auto first = std::find_if(data.rows().begin(), data.rows().end(),
                                  [&](const auto &r) { return r.session == config.begin; });
        const auto begin = static_cast<std::uint64_t>(first - data.rows().begin());
        if (begin == 0 || begin >= n) {
            fail(ErrorCode::Coverage, "fresh profile needs a previous declared funding close");
        }
        if (!std::filesystem::create_directory(out)) {
            fail(ErrorCode::IO, "cannot exclusively create run directory");
        }
        auto registry = engine(lx::Registry::create());
        auto types = engine(quant::lexongine::register_components(
            registry, {build.identity(), {}, lx::MissingIntervalPolicy::Strict}));
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
        const HostLimits limits{3, 2 * n + 16, 16 * n + 32, 16 * mb};
        auto zero = data::Quantity::from_raw(0);
        auto cash = data::Money::from_raw(data::CurrencyCode{"USD"}, config.initial_cash);
        auto lot = data::Quantity::from_raw(config.lot);
        SuiteConfiguration suite{begin - 1,
                                 begin,
                                 n,
                                 false,
                                 limits,
                                 financial_evidence_spec((out / "financial.txt").string(), limits),
                                 {{1,
                                   std::string{strategies::sma_strategy_id},
                                   "strict_greater;both_ready;structural_risk_only",
                                   cash,
                                   lot,
                                   config.costs,
                                   {},
                                   TerminalMode::Mark,
                                   {{"sma20", 19, n}, {"sma50", 49, n}}},
                                  {2,
                                   "cash_usd_no_interest_v1",
                                   "no_interest;no_orders",
                                   cash,
                                   lot,
                                   config.costs,
                                   {},
                                   TerminalMode::Mark,
                                   {}},
                                  {3,
                                   "buy_hold95_next_open_v1",
                                   "first_evaluation_close;single_entry;no_rebalance",
                                   cash,
                                   lot,
                                   config.costs,
                                   {},
                                   TerminalMode::Mark,
                                   {}}}};
        auto charge = engine(data.account()->reserve(lx::ResourceKind::Graph, 8 * mb + n * 2048));
        auto evidence = std::make_shared<EvidenceState>();
        evidence->charge = std::move(charge);
        evidence->decisions.reserve(n);
        evidence->summaries->reserve(3);
        evidence->begin = begin;
        evidence->end = n;
        auto graph_text = graph(data, config, out);
        auto parsed = engine(lx::parse_xml(graph_text, {mb, 32, 16384, 4096, data.account()}));
        auto composition = engine(lx::resolve_composition(parsed, registry));
        lx::PrepareOptions options{
            "s1-fresh",
            take(source.request({std::string{qr::clock_profile}, 0, n})),
            {data.account(),
             {{n, n, 0, n}, 4096, 32 * mb, 32 * mb, mb},
             lx::MissingIntervalPolicy::Strict,
             {data.owned_bytes(), mb, 0, 8192, sizeof(data::Bar) + 512, 64 * mb, 0}},
            {{0, 32 * mb, 16 * mb, 256 * mb, 256 * mb, 256 * mb, 160 * mb},
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
            spec("decisions", "s1_decisions_csv_v1", out / "decisions.csv", 4 * mb, n),
            spec("summary", "s1_summary_json_v1", out / "summary.json", mb, 3),
            spec("domain_config", "s1_domain_xml_v1", out / "effective-config.xml", 65536, 1),
            spec("graph", "lexongine_composition_xml_v1", out / "graph.xml", mb, 1),
            spec("input_identity", "s1_input_identity_v1", out / "input-identity.txt", 65536, 1)};
        options.evidence_account = data.account();
        options.evidence_resources = {0, 0, 2048 * mb, 16 * mb, 0, 64 * mb, 16 * mb};
        const std::array<std::uint32_t, 3> report_ids{1, 2, 3};
        auto reports = take(report_specs(out, report_ids, n - begin, 16 * mb));
        options.evidence.insert(options.evidence.end(), reports.required.begin(),
                                reports.required.end());
        auto report_suite = suite;
        auto prepared = engine(lx::prepare(composition, registry, resolver, {}, options));
        if (!prepared.plan || !prepared.rejected.empty()) {
            fail(ErrorCode::SourceBinding,
                 "S1 required graph/observations were not fully admitted");
        }
        auto plan = prepared.plan;
        std::vector<DecisionCallback> decisions{
            [evidence](const DecisionContext &c, const Account *a,
                       std::span<const PriceFeature> values) -> BacktestResult<Action> {
                return detail::guard([&]() -> BacktestResult<Action> {
                    auto action = take(strategies::sma20_50_decision(c, a, values));
                    if (evidence->decisions.size() == evidence->end) {
                        fail(ErrorCode::ResourceExceeded,
                             "S1 decision trace exceeds admitted rows");
                    }
                    DecisionRow row{c.key, c.may_submit, c.funded,     {},
                                    {},    action.kind,  action.reason};
                    for (const auto &value : values) {
                        if (value.request == "sma20") {
                            row.fast = value.value.raw();
                        } else if (value.request == "sma50") {
                            row.slow = value.value.raw();
                        }
                    }
                    evidence->decisions.push_back(std::move(row));
                    return action;
                });
            },
            [zero](const auto &, const auto *, auto) -> BacktestResult<Action> {
                return Action{ActionKind::Hold, zero, "cash_control"};
            },
            [zero, begin](const DecisionContext &c, const auto *, auto) -> BacktestResult<Action> {
                return Action{c.may_submit && c.key == begin ? ActionKind::TargetLong
                                                             : ActionKind::Hold,
                              zero, "buy_hold_control"};
            }};
        auto host = take(HostedSimulation::create(data, plan, build, std::move(suite),
                                                  std::move(decisions), types.price));
        evidence->financial = host.financial();
        std::vector<lx::RequiredEvidenceSink> extra;
        extra.push_back(sink(options.evidence[1], data.account(), [evidence] {
            std::ostringstream out;
            out << "key,may_submit,funded,sma20_raw,sma50_raw,action,reason\n";
            for (const auto &row : evidence->decisions) {
                out << row.key << ',' << row.allowed << ',' << row.funded << ','
                    << (row.fast ? std::to_string(*row.fast) : "") << ','
                    << (row.slow ? std::to_string(*row.slow) : "") << ','
                    << static_cast<int>(row.action) << ',' << row.reason << '\n';
            }
            return out.str();
        }));
        extra.push_back(sink(options.evidence[2], data.account(),
                             [evidence, config, manifest = std::string{data.manifest_sha256()}] {
                                 return summary_json(*evidence, config, manifest);
                             }));
        extra.push_back(
            sink(options.evidence[3], data.account(), [text = config.xml] { return text; }));
        extra.push_back(
            sink(options.evidence[4], data.account(), [graph_text] { return graph_text; }));
        extra.push_back(sink(options.evidence[5], data.account(),
                             [manifest = std::string{data.manifest_sha256()},
                              prices = std::string{data.prices_sha256()},
                              sessions = std::string{data.sessions_sha256()}] {
                                 return "quadeplus_s1_input_v1\nmanifest=" + manifest +
                                        "\nprices=" + prices + "\nsessions=" + sessions + "\n";
                             }));
        auto report_sinks = take(bind_report_sinks(
            reports, host.financial(), std::move(report_suite), data,
            {"sma-fresh", config.profile, "previously_inspected_research_not_holdout",
             "native S1; dataset=" + std::string{data.manifest_sha256()}}));
        for (auto &report : report_sinks) {
            extra.push_back(std::move(report));
        }
        std::vector<std::string> producers;
        for (const auto &node : plan->nodes()) {
            producers.push_back(node.spec.id);
        }
        auto retention = engine(lx::FileEvidence::create(
            plan->evidence_specification(), "s1-fresh", std::string{qr::clock_profile},
            std::move(producers), data.account(), {65536, 8192, 2048 * mb}));
        auto result = take(host.run(retention.callbacks(), std::move(extra)));
        return ApplicationRun{std::move(result), evidence->summaries};
    });
}
} // namespace quant::backtest::app
