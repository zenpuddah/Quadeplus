#include "quant/research/daily_dataset.hpp"
#include "quant/storage/file_integrity.hpp"
#include "backtest_source_checks.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace quant::research;
using quant::backtest::BacktestError;
using quant::backtest::ErrorCode;
void require(bool v, std::string_view message) {
    if (!v) {
        throw std::runtime_error(std::string{message});
    }
}
template <class T> T take(std::variant<T, BacktestError> result) {
    if (auto *e = std::get_if<BacktestError>(&result)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(result));
}
template <class T> void rejects(std::variant<T, BacktestError> result, ErrorCode code) {
    const auto *e = std::get_if<BacktestError>(&result);
    require(e && e->code == code, e ? e->text() : "expected rejection");
}
std::shared_ptr<::lexongine::ResourceAccount> account() {
    auto r = ::lexongine::ResourceAccount::create(
        {{134217728, 16777216, 134217728, 16777216, 16777216, 67108864, 8388608},
         4096,
         4096,
         8192,
         128});
    if (auto *e = std::get_if<::lexongine::Diagnostic>(&r)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<std::shared_ptr<::lexongine::ResourceAccount>>(r);
}
std::string digest(std::string_view bytes) {
    return quant::storage::files::sha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}
std::filesystem::path scratch;
struct Directory {
    std::filesystem::path path;
    bool keep = false;
    Directory() {
        std::filesystem::create_directories(scratch);
        auto pattern = (scratch / "daily.XXXXXX").string();
        std::vector<char> data(pattern.begin(), pattern.end());
        data.push_back('\0');
        require(::mkdtemp(data.data()) != nullptr, "scratch directory");
        path = data.data();
    }
    ~Directory() {
        if (!keep) {
            std::error_code e;
            std::filesystem::remove_all(path, e);
        }
    }
};
const std::string prices =
    "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n2004-11-18,1.0000000005,2,1,1.5,100,1.4,"
    "0\n2004-11-19,1.5,2,1,1.6,101,1.5,1.889\n2004-11-22,1.6,2,1,1.7,102,1.6,0\n2004-11-23,1.7,2,1,"
    "1.8,103,1.7,0\n2004-11-24,1.8,2,1,1.9,104,1.8,0\n";
const std::string grid = "Date\n2004-11-18\n2004-11-19\n2004-11-22\n2004-11-23\n2004-11-24\n";
DatasetSpec spec() {
    return {"synthetic-five-v1",
            "TEST",
            "USD",
            "synthetic-authored-input-v1",
            1,
            1,
            1,
            "synthetic_session_grid_v1",
            std::string{adjustment_profile},
            std::string{decimal_profile},
            std::string{volume_profile},
            std::string{clock_profile},
            "2004-11-18",
            "2004-11-22",
            "2004-11-24"};
}
DatasetPreparation request(const Directory &d, std::string_view input = prices,
                           std::string_view sessions = grid) {
    quant::storage::files::write_text(d.path / "input.csv", std::string{input});
    quant::storage::files::write_text(d.path / "grid.csv", std::string{sessions});
    return {d.path / "input.csv", d.path / "grid.csv",
            d.path / "bundle",    digest(input),
            digest(sessions),     spec(),
            {1048576, 32, 64},    account()};
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
    auto at = value.find(from);
    require(at != std::string::npos, "mutation absent");
    value.replace(at, from.size(), to);
    return value;
}
void valid_bundle_and_source() {
    Directory d;
    auto r = request(d);
    auto dataset = take(prepare_daily_dataset(r));
    require(dataset.rows().size() == 5, "five rows not retained");
    require(dataset.rows()[0].bar.open.raw() == 1000000000, "ties-even normalization changed");
    require(dataset.rows()[1].dividend.raw() == 1889000000, "dividend was rounded to cents");
    require(dataset.normalizations().size() == 1 &&
                dataset.maximum_normalization_error() == "0.0000000005",
            "normalization evidence mismatch");
    auto reopened = take(load_daily_dataset(
        {r.output_directory, std::string{dataset.manifest_sha256()}, r.limits, r.account}));
    require(reopened.rows()[4].bar.close.raw() == 1900000000, "readback changed prices");
    rejects(prepare_daily_dataset(r), ErrorCode::IO);
    const auto &first = dataset.rows()[0];
    rejects(dataset.closing(0, first.bar.interval_start.unix_nanos()), ErrorCode::FutureRead);
    rejects(dataset.opening(0, first.bar.interval_start.unix_nanos() - 1), ErrorCode::FutureRead);
    require(take(dataset.opening(0, first.bar.interval_start.unix_nanos())).price().raw() ==
                1000000000,
            "opening projection changed price");
    verify_daily_source(dataset);
    d.keep = true;
    std::cout << "SYNTHETIC_BUNDLE path=" << r.output_directory
              << " manifest=" << dataset.manifest_sha256() << '\n';
}
void raw_validation_before_rounding() {
    for (const auto &bad : std::vector<std::string>{
             replace(prices, "1.0000000005,2,1,1.5", "1.0000000001,1.0000000001,1,1.0000000002"),
             replace(prices, "101,1.5,1.889", "101,1.5,-0.0000000001"),
             replace(prices, "1.0000000005,2,1,1.5", "0.0000000001,2,0.0000000001,1.5")}) {
        Directory d;
        rejects(prepare_daily_dataset(request(d, bad)), ErrorCode::InvalidData);
        require(!std::filesystem::exists(d.path / "bundle"), "invalid raw input created a bundle");
    }
    Directory d;
    rejects(prepare_daily_dataset(request(d, replace(prices, "100,1.4,0", "0.0000001,1.4,0"))),
            ErrorCode::InvalidData);
}
void coverage_and_identity_failures() {
    {
        Directory d;
        auto r = request(d);
        r.expected_prices_sha256 = std::string(64, '0');
        rejects(prepare_daily_dataset(r), ErrorCode::Integrity);
    }
    {
        Directory d;
        auto bad = replace(prices, "2004-11-19,", "2004-11-18,");
        rejects(prepare_daily_dataset(request(d, bad)), ErrorCode::Coverage);
    }
    {
        Directory d;
        auto bad = replace(prices, "2004-11-22,1.6,2,1,1.7,102,1.6,0\n", "");
        rejects(prepare_daily_dataset(request(d, bad)), ErrorCode::Coverage);
    }
    {
        Directory d;
        auto r = request(d);
        r.specification.adjustment = "total_return_plus_dividends";
        rejects(prepare_daily_dataset(r), ErrorCode::Unsupported);
    }
    {
        Directory d;
        auto r = request(d);
        r.specification.currency = "EUR";
        rejects(prepare_daily_dataset(r), ErrorCode::Unsupported);
    }
    {
        Directory d;
        auto r = request(d);
        r.specification.clock = "intrabar_early_close";
        rejects(prepare_daily_dataset(r), ErrorCode::Unsupported);
    }
    {
        Directory d;
        auto r = request(d);
        auto dataset = take(prepare_daily_dataset(r));
        rejects(load_daily_dataset({r.output_directory, std::string(64, '0'), r.limits, r.account}),
                ErrorCode::Integrity);
        std::filesystem::remove(r.output_directory / "prices.csv");
        rejects(load_daily_dataset({r.output_directory, std::nullopt, r.limits, r.account}),
                ErrorCode::IO);
        require(dataset.rows()[0].bar.close.raw() == 1500000000,
                "prepared data reopened changed path");
    }
}
void sealed_grid_and_origin() {
    Directory d;
    auto r = request(d);
    auto hash = take(seal_reference_grid(r.prices_csv, r.expected_prices_sha256,
                                         d.path / "sealed.csv", r.limits, r.account));
    require(hash == digest(grid), "sealed original grid mismatch");
    rejects(seal_reference_grid(r.prices_csv, r.expected_prices_sha256, d.path / "sealed.csv",
                                r.limits, r.account),
            ErrorCode::IO);
    r.specification.numerical_origin = "2004-11-19";
    auto dataset = take(prepare_daily_dataset(r));
    require(dataset.rows().size() == 4 && dataset.rows()[0].session == "2004-11-19",
            "pre-origin rows entered numerical state");
}
// Deliberately rewrite only test-owned adversarial bundles. Rehashing ensures the
// loader reaches semantic checks rather than stopping at an unrelated checksum.
void rewrite(const std::filesystem::path &path, const std::string &text) {
    require(std::filesystem::remove(path), "missing tamper target");
    quant::storage::files::write_text(path, text);
}
void manifest_and_resource_failures() {
    for (const auto &mutation : std::vector<std::pair<std::string, std::string>>{
             {std::string{daily_format}, "quadeplus-daily-research-v999"},
             {"selected_rows 5", "selected_rows 4"},
             {"changed_values 1", "changed_values 0"},
             {"maximum_error \"0.0000000005\"", "maximum_error \"0\""}}) {
        Directory d;
        auto r = request(d);
        auto dataset = take(prepare_daily_dataset(r));
        auto manifest = replace(quant::storage::files::read(r.output_directory / "manifest.qdaily"),
                                mutation.first, mutation.second);
        rewrite(r.output_directory / "manifest.qdaily", manifest);
        rewrite(r.output_directory / "COMPLETE",
                std::string{daily_format} + " " + digest(manifest) + "\n");
        rejects(load_daily_dataset({r.output_directory, std::nullopt, r.limits, r.account}),
                mutation.first == daily_format ? ErrorCode::Unsupported : ErrorCode::Integrity);
        require(dataset.rows().size() == 5, "rewriting disk changed the pinned in-memory dataset");
    }
    {
        Directory d;
        auto r = request(d);
        auto dataset = take(prepare_daily_dataset(r));
        rewrite(r.output_directory / "normalization.csv", "unverified evidence\n");
        rejects(load_daily_dataset({r.output_directory, std::nullopt, r.limits, r.account}),
                ErrorCode::Integrity);
    }
    for (int variant = 0; variant < 4; ++variant) {
        Directory d;
        auto r = request(d);
        const auto before = r.account->current(::lexongine::ResourceKind::Source);
        switch (variant) {
        case 0:
            r.limits.max_rows = 4;
            break;
        case 1:
            r.limits.max_file_bytes = 32;
            break;
        case 2:
            r.limits.max_decimal_bytes = 2;
            break;
        case 3:
            r.account.reset();
            break;
        }
        rejects(prepare_daily_dataset(r), variant == 2   ? ErrorCode::InvalidData
                                          : variant == 3 ? ErrorCode::InvalidConfig
                                                         : ErrorCode::ResourceExceeded);
        require(!std::filesystem::exists(r.output_directory),
                "failed admission published a bundle");
        if (r.account) {
            require(r.account->current(::lexongine::ResourceKind::Source) == before &&
                        r.account->current(::lexongine::ResourceKind::Scratch) == 0,
                    "failed import leaked resource reservations");
        }
    }
    {
        Directory d;
        auto r = request(d);
        auto held = r.account->reserve(::lexongine::ResourceKind::Source, 134217728);
        require(std::holds_alternative<::lexongine::Reservation>(held),
                "reserve test source ceiling");
        rejects(prepare_daily_dataset(r), ErrorCode::ResourceExceeded);
        require(!std::filesystem::exists(r.output_directory),
                "exhausted account published a bundle");
    }
}
void decimal_boundaries() {
    for (const auto &[input, raw] :
         std::vector<std::pair<std::string, std::int64_t>>{{"1.0000000015", 1000000002},
                                                           {"1.0000000025", 1000000002},
                                                           {"1.9999999995", 2000000000},
                                                           {"10000000005e-10", 1000000000}}) {
        Directory d;
        auto r = request(d, replace(prices, "1.0000000005", input));
        auto dataset = take(prepare_daily_dataset(r));
        require(dataset.rows()[0].bar.open.raw() == raw,
                "ties-even/carry/exponent boundary changed");
        require(dataset.maximum_normalization_error() == "0.0000000005",
                "exact boundary error changed");
    }
    for (const auto &input :
         std::vector<std::string>{"nan", "1e", "1e-65", "1.", "1e+999999", "+"}) {
        Directory d;
        auto r = request(d, replace(prices, "1.0000000005", input));
        require(std::holds_alternative<BacktestError>(prepare_daily_dataset(r)),
                "malformed decimal admitted");
    }
    {
        Directory d;
        auto bad = replace(prices, "1.0000000005,2,1,1.5",
                           "9223372036.8547758075,9223372036.8547758075,1,1.5");
        rejects(prepare_daily_dataset(request(d, bad)), ErrorCode::NumericRange);
    }
}
void retained_spy(const std::filesystem::path &path) {
    Directory d;
    auto resources = account();
    DatasetLimits limits{4194304, 10000, 64};
    constexpr std::string_view pin =
        "28963337b6eba735cad1eb62dc333d497079cff73f02076d17d3439c48e167ae";
    auto grid_hash =
        take(seal_reference_grid(path, pin, d.path / "sessions.csv", limits, resources));
    auto s = spec();
    s.version = "retained-spy-v1";
    s.symbol = "SPY";
    s.provenance = "retained-research-2026-10-18/SPY.csv";
    s.schedule = "retained_reference_grid_v1";
    s.evaluation_begin = "2010-01-04";
    s.evaluation_end = "2026-10-15";
    auto data = take(prepare_daily_dataset({path, d.path / "sessions.csv", d.path / "bundle",
                                            std::string{pin}, grid_hash, s, limits, resources}));
    require(data.rows().front().session == "2004-11-18" &&
                data.rows().back().session == "2026-10-15",
            "retained numerical origin/end changed");
    auto reopened = take(load_daily_dataset(
        {d.path / "bundle", std::string{data.manifest_sha256()}, limits, resources}));
    require(reopened.rows().size() == 5511 && reopened.normalizations().size() == 799,
            "retained readback changed counts");
    d.keep = true;
    std::cout << "RETAINED_SPY rows=" << data.rows().size()
              << " normalization_changes=" << data.normalizations().size()
              << " max_error=" << data.maximum_normalization_error()
              << " manifest=" << data.manifest_sha256() << " path=" << d.path / "bundle" << '\n';
}
} // namespace
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        return 1;
    }
    scratch = argv[1];
    unsigned passed = 0;
    const std::pair<const char *, void (*)()> cases[]{
        {"immutable bundle, normalization and five-Bar source", valid_bundle_and_source},
        {"raw validation before canonical normalization", raw_validation_before_rounding},
        {"coverage, identity and policy gates", coverage_and_identity_failures},
        {"sealed reference grid and numerical origin", sealed_grid_and_origin},
        {"manifest semantics and resource admission", manifest_and_resource_failures},
        {"decimal ties, carry, exponent and overflow", decimal_boundaries}};
    for (const auto &[name, test] : cases) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    bool retained = true;
    if (argc == 3) {
        try {
            retained_spy(argv[2]);
        } catch (const std::exception &e) {
            std::cerr << "FAIL retained SPY: " << e.what() << '\n';
            retained = false;
        }
    }
    std::cout << passed << "/" << std::size(cases) << " dataset cases passed\n";
    return passed == std::size(cases) && retained ? 0 : 1;
}
