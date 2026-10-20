#include "quant/research/daily_dataset.hpp"
#include "quant/data/validation.hpp"
#include "quant/storage/file_integrity.hpp"
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace quant::research {
namespace {
using backtest::BacktestError;
using backtest::ErrorCode;
using backtest::error;
struct Failure {
    BacktestError error;
};
[[noreturn]] void fail(ErrorCode code, std::string_view message) {
    throw Failure{error(code, message)};
}
template <class F> auto guard(F &&call) noexcept -> decltype(call()) {
    try {
        return call();
    } catch (const Failure &e) {
        return e.error;
    } catch (const std::bad_alloc &) {
        return error(ErrorCode::ResourceExceeded, "dataset allocation failed");
    } catch (const std::exception &e) {
        return error(ErrorCode::IO, e.what());
    } catch (...) {
        return error(ErrorCode::IO, "unknown dataset failure");
    }
}
::lexongine::Reservation reserve(const std::shared_ptr<::lexongine::ResourceAccount> &account,
                                 ::lexongine::ResourceKind kind, std::uint64_t bytes) {
    auto r = account->reserve(kind, bytes);
    if (auto *e = std::get_if<::lexongine::Diagnostic>(&r)) {
        fail(ErrorCode::ResourceExceeded, e->text());
    }
    return std::get<::lexongine::Reservation>(std::move(r));
}
void validate_limits(DatasetLimits limits,
                     const std::shared_ptr<::lexongine::ResourceAccount> &account) {
    if (!account || !limits.max_file_bytes || limits.max_file_bytes > 67108864 ||
        !limits.max_rows || limits.max_rows > 100000 || !limits.max_decimal_bytes ||
        limits.max_decimal_bytes > 128) {
        fail(ErrorCode::InvalidConfig, "invalid explicit daily dataset limits/account");
    }
}
bool digest_valid(std::string_view digest) {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::string hash(std::string_view bytes) {
    return storage::files::sha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}
struct Bytes {
    ::lexongine::Reservation charge;
    std::string text;
};
Bytes read_file(const std::filesystem::path &path, DatasetLimits limits,
                const std::shared_ptr<::lexongine::ResourceAccount> &account) {
    if (!path.is_absolute()) {
        fail(ErrorCode::InvalidConfig, "dataset paths must be absolute");
    }
    struct Handle {
        int fd;
        ~Handle() {
            if (fd >= 0) {
                static_cast<void>(::close(fd));
            }
        }
    } handle{::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    if (handle.fd < 0) {
        fail(ErrorCode::IO, "required dataset file cannot be opened");
    }
    struct stat before{};
    if (::fstat(handle.fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0) {
        fail(ErrorCode::IO, "dataset input is not a regular file");
    }
    const auto size = static_cast<std::uint64_t>(before.st_size);
    if (size > limits.max_file_bytes) {
        fail(ErrorCode::ResourceExceeded, "dataset file exceeds explicit byte bound");
    }
    auto charge = reserve(account, ::lexongine::ResourceKind::Scratch, size * 4 + 4096);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = ::read(handle.fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            fail(ErrorCode::IO, "dataset file truncated while reading");
        }
        offset += static_cast<std::size_t>(n);
    }
    struct stat after{};
    if (::fstat(handle.fd, &after) != 0 || after.st_size != before.st_size ||
        after.st_mtime != before.st_mtime) {
        fail(ErrorCode::Integrity, "dataset file changed while reading");
    }
    if (::close(std::exchange(handle.fd, -1)) != 0) {
        fail(ErrorCode::IO, "dataset read close failed");
    }
    return {std::move(charge), std::move(bytes)};
}
void verify(std::string_view bytes, std::string_view expected) {
    if (!digest_valid(expected) || hash(bytes) != expected) {
        fail(ErrorCode::Integrity, "dataset file hash mismatch");
    }
}
template <class T> T number(std::string_view token) {
    T v{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), v);
    if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        std::to_string(v) != token) {
        fail(ErrorCode::InvalidData, "invalid canonical manifest integer");
    }
    return v;
}
std::int64_t open_time(std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        fail(ErrorCode::InvalidData, "daily session must be YYYY-MM-DD");
    }
    auto digits = [&](std::size_t at, std::size_t n) {
        int value = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = date[at + i];
            if (c < '0' || c > '9') {
                fail(ErrorCode::InvalidData, "invalid session date digit");
            }
            value = value * 10 + c - '0';
        }
        return value;
    };
    const auto y = digits(0, 4), m = digits(5, 2), d = digits(8, 2);
    if (y < 1900 || y > 2200) {
        fail(ErrorCode::NumericRange, "session date exceeds admitted nanosecond range");
    }
    const std::chrono::year_month_day day{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(m)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    if (!day.ok()) {
        fail(ErrorCode::InvalidData, "invalid calendar date");
    }
    return std::chrono::sys_days{day}.time_since_epoch().count() * 86400000000000LL;
}
void validate_spec(const DatasetSpec &s) {
    for (const auto *text : {&s.version, &s.symbol, &s.currency, &s.provenance, &s.schedule,
                             &s.adjustment, &s.normalization, &s.volume, &s.clock,
                             &s.numerical_origin, &s.evaluation_begin, &s.evaluation_end}) {
        if (text->empty() || text->size() > 512 ||
            text->find_first_of("\r\n\t") != std::string::npos ||
            text->find('\0') != std::string::npos) {
            fail(ErrorCode::InvalidConfig, "invalid bounded manifest text");
        }
    }
    if (!s.instrument || !s.venue || !s.source) {
        fail(ErrorCode::InvalidConfig, "dataset identifiers must be explicit and nonzero");
    }
    if (s.currency != "USD" ||
        (s.schedule != "synthetic_session_grid_v1" && s.schedule != "retained_reference_grid_v1") ||
        s.adjustment != adjustment_profile || s.normalization != decimal_profile ||
        s.volume != volume_profile || s.clock != clock_profile) {
        fail(ErrorCode::Unsupported, "unsupported daily dataset policy or currency");
    }
    static_cast<void>(open_time(s.numerical_origin));
    static_cast<void>(open_time(s.evaluation_begin));
    static_cast<void>(open_time(s.evaluation_end));
    if (s.numerical_origin > s.evaluation_begin || s.evaluation_begin > s.evaluation_end) {
        fail(ErrorCode::InvalidConfig, "invalid numerical/financial date ranges");
    }
}
std::string strip_zero(std::string value) {
    const auto p = value.find_first_not_of('0');
    return p == std::string::npos ? "0" : value.substr(p);
}
struct Decimal {
    std::string digits;
    int scale;
    bool negative;
};
Decimal decimal(std::string_view text, std::uint64_t maximum) {
    if (text.empty() || text.size() > maximum) {
        fail(ErrorCode::InvalidData, "empty or excessive decimal token");
    }
    std::size_t p = 0;
    bool negative = false;
    if (text[p] == '-' || text[p] == '+') {
        negative = text[p] == '-';
        ++p;
    }
    std::string digits;
    digits.reserve(text.size());
    std::size_t integer = 0, fraction = 0;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
        digits += text[p++];
        ++integer;
    }
    if (!integer) {
        fail(ErrorCode::InvalidData, "decimal requires integer digits");
    }
    if (p < text.size() && text[p] == '.') {
        ++p;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
            digits += text[p++];
            ++fraction;
        }
        if (!fraction) {
            fail(ErrorCode::InvalidData, "decimal fraction is empty");
        }
    }
    int exponent = 0;
    if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
        ++p;
        bool minus = false;
        if (p < text.size() && (text[p] == '+' || text[p] == '-')) {
            minus = text[p] == '-';
            ++p;
        }
        const auto begin = p;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
            if (exponent > 64) {
                fail(ErrorCode::NumericRange, "decimal exponent exceeds bound");
            }
            exponent = exponent * 10 + text[p++] - '0';
        }
        if (begin == p || exponent > 64) {
            fail(ErrorCode::NumericRange, "decimal exponent exceeds bound");
        }
        if (minus) {
            exponent = -exponent;
        }
    }
    const int scale = static_cast<int>(fraction) - exponent;
    if (p != text.size() || scale < -64 || scale > 64) {
        fail(ErrorCode::InvalidData, "unsupported decimal token or precision");
    }
    return {strip_zero(std::move(digits)), scale, negative};
}
int compare(const Decimal &a, const Decimal &b) {
    if (a.digits == "0" || b.digits == "0") {
        return a.digits == b.digits ? 0 : a.digits == "0" ? -1 : 1;
    }
    const auto am = static_cast<int>(a.digits.size()) - a.scale,
               bm = static_cast<int>(b.digits.size()) - b.scale;
    if (am != bm) {
        return am < bm ? -1 : 1;
    }
    const auto scale = std::max(a.scale, b.scale);
    const auto av = a.digits + std::string(static_cast<std::size_t>(scale - a.scale), '0'),
               bv = b.digits + std::string(static_cast<std::size_t>(scale - b.scale), '0');
    return av == bv ? 0 : av < bv ? -1 : 1;
}
std::string decimal_text(std::string digits, int scale, bool fixed = false) {
    if (scale < 0) {
        digits.append(static_cast<std::size_t>(-scale), '0');
        return digits;
    }
    if (digits.size() <= static_cast<std::size_t>(scale)) {
        digits.insert(0, static_cast<std::size_t>(scale) + 1 - digits.size(), '0');
    }
    if (scale) {
        digits.insert(digits.size() - static_cast<std::size_t>(scale), 1, '.');
        if (!fixed) {
            while (digits.back() == '0') {
                digits.pop_back();
            }
            if (digits.back() == '.') {
                digits.pop_back();
            }
        }
    }
    return digits;
}
std::int64_t normalized(const Decimal &value, int target, bool exact) {
    const auto drop = value.scale - target;
    std::string integer, fraction;
    if (drop <= 0) {
        integer = value.digits + std::string(static_cast<std::size_t>(-drop), '0');
    } else if (value.digits.size() > static_cast<std::size_t>(drop)) {
        integer = value.digits.substr(0, value.digits.size() - static_cast<std::size_t>(drop));
        fraction = value.digits.substr(integer.size());
    } else {
        integer = "0";
        fraction =
            std::string(static_cast<std::size_t>(drop) - value.digits.size(), '0') + value.digits;
    }
    integer = strip_zero(std::move(integer));
    std::int64_t result = 0;
    const auto parsed = std::from_chars(integer.data(), integer.data() + integer.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != integer.data() + integer.size()) {
        fail(ErrorCode::NumericRange, "canonical coefficient exceeds signed64");
    }
    const bool remainder =
        std::any_of(fraction.begin(), fraction.end(), [](char c) { return c != '0'; });
    if (exact && remainder) {
        fail(ErrorCode::InvalidData, "volume is not exactly representable in Quantity6");
    }
    const bool up = !fraction.empty() &&
                    (fraction[0] > '5' ||
                     (fraction[0] == '5' && (std::any_of(fraction.begin() + 1, fraction.end(),
                                                         [](char c) { return c != '0'; }) ||
                                             result % 2)));
    if (!exact && up) {
        if (result == std::numeric_limits<std::int64_t>::max()) {
            fail(ErrorCode::NumericRange, "rounded coefficient overflow");
        }
        ++result;
    }
    return result;
}
std::string difference(const Decimal &a, std::int64_t raw, int target) {
    const auto scale = std::max({a.scale, target, 0});
    auto x = strip_zero(a.digits + std::string(static_cast<std::size_t>(scale - a.scale), '0'));
    auto y = strip_zero(std::to_string(raw) +
                        std::string(static_cast<std::size_t>(scale - target), '0'));
    if (x.size() < y.size() || (x.size() == y.size() && x < y)) {
        std::swap(x, y);
    }
    y.insert(0, x.size() - y.size(), '0');
    int borrow = 0;
    for (std::size_t i = x.size(); i-- > 0;) {
        int v = x[i] - y[i] - borrow;
        borrow = v < 0;
        if (borrow) {
            v += 10;
        }
        x[i] = static_cast<char>('0' + v);
    }
    return decimal_text(strip_zero(std::move(x)), scale);
}
struct Lines {
    std::string_view text;
    bool next(std::string_view &line) {
        if (text.empty()) {
            return false;
        }
        const auto p = text.find('\n');
        line = text.substr(0, p);
        text = p == std::string_view::npos ? std::string_view{} : text.substr(p + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.size() > 2048) {
            fail(ErrorCode::InvalidData, "empty or excessive CSV line");
        }
        return true;
    }
};
constexpr std::string_view csv_header = "Date,Open,High,Low,Close,Volume,AdjClose,Dividend";
std::array<std::string_view, 8> columns(std::string_view line) {
    std::array<std::string_view, 8> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto p = line.find(',');
        if ((i + 1 == result.size()) != (p == std::string_view::npos)) {
            fail(ErrorCode::InvalidData, "wrong retained CSV field count");
        }
        result[i] = line.substr(0, p);
        if (result[i].empty() || result[i].find('"') != std::string_view::npos) {
            fail(ErrorCode::InvalidData, "unsupported quoted or empty CSV token");
        }
        if (p != std::string_view::npos) {
            line.remove_prefix(p + 1);
        }
    }
    return result;
}
std::vector<std::string_view> sessions(std::string_view bytes, DatasetLimits limits) {
    Lines lines{bytes};
    std::string_view line;
    if (!lines.next(line) || line != "Date") {
        fail(ErrorCode::InvalidData, "session grid header mismatch");
    }
    std::vector<std::string_view> result;
    result.reserve(static_cast<std::size_t>(limits.max_rows));
    while (lines.next(line)) {
        static_cast<void>(open_time(line));
        if (result.size() == limits.max_rows) {
            fail(ErrorCode::ResourceExceeded, "session count exceeds bound");
        }
        if (!result.empty() && line <= result.back()) {
            fail(ErrorCode::Coverage, "duplicate or reordered expected session");
        }
        result.push_back(line);
    }
    if (result.empty()) {
        fail(ErrorCode::Coverage, "empty session grid");
    }
    return result;
}
std::string report_text(std::span<const NormalizationChange> changes, std::uint64_t maximum) {
    std::string out = "Date,Field,Original,Normalized,AbsoluteError\n";
    out.reserve(static_cast<std::size_t>(maximum));
    for (const auto &change : changes) {
        const auto row = change.session + "," + change.field + "," + change.original + "," +
                         change.normalized + "," + change.absolute_error + "\n";
        if (row.size() > maximum - out.size()) {
            fail(ErrorCode::ResourceExceeded, "normalization report exceeds file limit");
        }
        out += row;
    }
    return out;
}
struct Manifest {
    DatasetSpec spec;
    std::string prices, sessions, changes, max_error;
    std::uint64_t original_rows = 0, selected_rows = 0, changed_values = 0;
};
std::string encode(const Manifest &m) {
    std::ostringstream out;
    out << daily_format << '\n';
    const auto text = [&](std::string_view key, const std::string &v) {
        out << key << ' ' << std::quoted(v) << '\n';
    };
    text("version", m.spec.version);
    text("symbol", m.spec.symbol);
    text("currency", m.spec.currency);
    text("provenance", m.spec.provenance);
    out << "instrument " << m.spec.instrument << "\nvenue " << m.spec.venue << "\nsource "
        << m.spec.source << '\n';
    text("schedule", m.spec.schedule);
    out << "missing_sessions \"reject\"\n";
    text("adjustment", m.spec.adjustment);
    text("normalization", m.spec.normalization);
    text("volume", m.spec.volume);
    text("clock", m.spec.clock);
    text("numerical_origin", m.spec.numerical_origin);
    text("evaluation_begin", m.spec.evaluation_begin);
    text("evaluation_end", m.spec.evaluation_end);
    text("prices_sha256", m.prices);
    text("sessions_sha256", m.sessions);
    text("normalizations_sha256", m.changes);
    out << "original_rows " << m.original_rows << "\nselected_rows " << m.selected_rows
        << "\nchanged_values " << m.changed_values << '\n';
    text("maximum_error", m.max_error);
    return out.str();
}
Manifest decode(std::string_view bytes) {
    std::istringstream in{std::string{bytes}};
    std::string token;
    in >> token;
    if (token != daily_format) {
        fail(ErrorCode::Unsupported, "unsupported daily bundle version");
    }
    auto key = [&](std::string_view expected) {
        if (!(in >> token) || token != expected) {
            fail(ErrorCode::Integrity, "missing, duplicate or reordered manifest field");
        }
    };
    auto text = [&](std::string_view name) {
        key(name);
        in >> std::ws;
        if (in.peek() != '"') {
            fail(ErrorCode::Integrity, "manifest text must be quoted");
        }
        std::string result;
        if (!(in >> std::quoted(result)) || result.size() > 512) {
            fail(ErrorCode::Integrity, "invalid manifest text");
        }
        return result;
    };
    auto integer = [&](std::string_view name) {
        key(name);
        std::string value;
        if (!(in >> value)) {
            fail(ErrorCode::Integrity, "missing integer");
        }
        return number<std::uint64_t>(value);
    };
    Manifest m;
    m.spec.version = text("version");
    m.spec.symbol = text("symbol");
    m.spec.currency = text("currency");
    m.spec.provenance = text("provenance");
    m.spec.instrument = integer("instrument");
    const auto venue = integer("venue"), source = integer("source");
    if (venue > UINT32_MAX || source > UINT32_MAX) {
        fail(ErrorCode::NumericRange, "identifier width overflow");
    }
    m.spec.venue = static_cast<std::uint32_t>(venue);
    m.spec.source = static_cast<std::uint32_t>(source);
    m.spec.schedule = text("schedule");
    if (text("missing_sessions") != "reject") {
        fail(ErrorCode::Unsupported, "unsupported missing-session policy");
    }
    m.spec.adjustment = text("adjustment");
    m.spec.normalization = text("normalization");
    m.spec.volume = text("volume");
    m.spec.clock = text("clock");
    m.spec.numerical_origin = text("numerical_origin");
    m.spec.evaluation_begin = text("evaluation_begin");
    m.spec.evaluation_end = text("evaluation_end");
    m.prices = text("prices_sha256");
    m.sessions = text("sessions_sha256");
    m.changes = text("normalizations_sha256");
    m.original_rows = integer("original_rows");
    m.selected_rows = integer("selected_rows");
    m.changed_values = integer("changed_values");
    m.max_error = text("maximum_error");
    if (in >> token) {
        fail(ErrorCode::Integrity, "trailing manifest fields");
    }
    validate_spec(m.spec);
    return m;
}
} // namespace

struct DailyDataset::Data {
    ::lexongine::Reservation charge;
    std::shared_ptr<::lexongine::ResourceAccount> account;
    DatasetSpec spec;
    std::vector<DailyRow> rows;
    std::vector<NormalizationChange> changes;
    std::string maximum_error = "0", manifest_hash, prices_hash, session_hash;
    std::uint64_t bytes = 0, original_rows = 0;
};
struct DatasetAccess {
    static std::shared_ptr<DailyDataset::Data>
    build(std::string_view prices, std::string_view grid, const DatasetSpec &spec,
          DatasetLimits limits, std::shared_ptr<::lexongine::ResourceAccount> account) {
        validate_spec(spec);
        const auto storage_bytes =
            65536 + limits.max_rows * (sizeof(DailyRow) + 5 * sizeof(NormalizationChange) +
                                       32 * (limits.max_decimal_bytes + 64) + 512);
        auto charge = reserve(account, ::lexongine::ResourceKind::Source, storage_bytes);
        auto data = std::make_shared<DailyDataset::Data>();
        data->charge = std::move(charge);
        data->account = account;
        data->spec = spec;
        data->bytes = storage_bytes;
        data->rows.reserve(static_cast<std::size_t>(limits.max_rows));
        data->changes.reserve(static_cast<std::size_t>(limits.max_rows * 5));
        const auto expected = sessions(grid, limits);
        Lines lines{prices};
        std::string_view line;
        if (!lines.next(line) || line != csv_header) {
            fail(ErrorCode::Unsupported, "unsupported retained daily CSV header");
        }
        std::size_t index = 0;
        std::string_view previous;
        bool origin = false, evaluation = false, end = false;
        while (lines.next(line)) {
            if (index == limits.max_rows) {
                fail(ErrorCode::ResourceExceeded, "CSV row count exceeds bound");
            }
            const auto fields = columns(line);
            const auto open = open_time(fields[0]);
            if ((!previous.empty() && fields[0] <= previous) || index >= expected.size() ||
                fields[0] != expected[index]) {
                fail(ErrorCode::Coverage, "candidate sessions differ from sealed grid");
            }
            previous = fields[0];
            ++index;
            std::array<Decimal, 4> price{decimal(fields[1], limits.max_decimal_bytes),
                                         decimal(fields[2], limits.max_decimal_bytes),
                                         decimal(fields[3], limits.max_decimal_bytes),
                                         decimal(fields[4], limits.max_decimal_bytes)};
            for (const auto &p : price) {
                if (p.negative || p.digits == "0") {
                    fail(ErrorCode::InvalidData, "raw OHLC must be positive");
                }
            }
            if (compare(price[1], price[2]) < 0 || compare(price[0], price[2]) < 0 ||
                compare(price[0], price[1]) > 0 || compare(price[3], price[2]) < 0 ||
                compare(price[3], price[1]) > 0) {
                fail(ErrorCode::InvalidData, "invalid raw OHLC inequalities");
            }
            const auto volume = decimal(fields[5], limits.max_decimal_bytes),
                       dividend = decimal(fields[7], limits.max_decimal_bytes);
            if (volume.negative || dividend.negative) {
                fail(ErrorCode::InvalidData, "raw volume/dividend must be nonnegative");
            }
            static_cast<void>(
                decimal(fields[6],
                        limits.max_decimal_bytes)); // Retain but never trade total-return AdjClose.
            std::array<std::int64_t, 4> raw{};
            for (std::size_t i = 0; i < raw.size(); ++i) {
                raw[i] = normalized(price[i], 9, false);
                if (raw[i] <= 0) {
                    fail(ErrorCode::InvalidData, "OHLC rounds to nonpositive canonical price");
                }
            }
            const auto quantity = normalized(volume, 6, true),
                       per_unit = normalized(dividend, 9, false);
            const auto close = open + 1000000000LL;
            data::Bar bar{{data::InstrumentId{spec.instrument}, data::VenueId{spec.venue},
                           data::Timestamp::from_unix_nanos(close), std::nullopt, std::nullopt,
                           std::nullopt, 0, data::SourceId{spec.source}},
                          data::Timestamp::from_unix_nanos(open),
                          data::Timestamp::from_unix_nanos(close),
                          data::Price::from_raw(raw[0]),
                          data::Price::from_raw(raw[1]),
                          data::Price::from_raw(raw[2]),
                          data::Price::from_raw(raw[3]),
                          data::Quantity::from_raw(quantity)};
            if (!data::validate(bar).empty()) {
                fail(ErrorCode::InvalidData, "normalized Bar fails canonical validation");
            }
            if (fields[0] < spec.numerical_origin || fields[0] > spec.evaluation_end) {
                continue;
            }
            origin |= fields[0] == spec.numerical_origin;
            evaluation |= fields[0] == spec.evaluation_begin;
            end |= fields[0] == spec.evaluation_end;
            if (!data->rows.empty() && data->rows.back().close_available_ns >= open) {
                fail(ErrorCode::InvalidData, "close is not available before next opening slot");
            }
            auto record = [&](std::string_view field, std::string_view original,
                              const Decimal &value, std::int64_t coefficient) {
                const auto error = difference(value, coefficient, 9);
                if (error == "0") {
                    return;
                }
                data->changes.push_back(
                    {std::string{fields[0]}, std::string{field}, std::string{original},
                     decimal_text(std::to_string(coefficient), 9, true), error});
                if (compare(decimal(error, 256), decimal(data->maximum_error, 256)) > 0) {
                    data->maximum_error = error;
                }
            };
            const std::string_view names[]{"Open", "High", "Low", "Close"};
            for (std::size_t i = 0; i < 4; ++i) {
                record(names[i], fields[i + 1], price[i], raw[i]);
            }
            record("Dividend", fields[7], dividend, per_unit);
            data->rows.push_back({std::string{fields[0]}, std::move(bar),
                                  DividendPerUnit9{data::CurrencyCode{spec.currency}, per_unit},
                                  close});
        }
        if (index != expected.size() || data->rows.empty() || !origin || !evaluation || !end) {
            fail(ErrorCode::Coverage, "required origin/evaluation/session coverage is incomplete");
        }
        data->original_rows = index;
        data->prices_hash = hash(prices);
        data->session_hash = hash(grid);
        return data;
    }
    static DailyDataset finish(std::shared_ptr<DailyDataset::Data> data) {
        return DailyDataset{std::move(data)};
    }
};

BacktestResult<DailyDataset> prepare_daily_dataset(const DatasetPreparation &request) noexcept {
    return guard([&]() -> BacktestResult<DailyDataset> {
        validate_limits(request.limits, request.account);
        validate_spec(request.specification);
        if (!request.output_directory.is_absolute()) {
            fail(ErrorCode::InvalidConfig, "new bundle path must be absolute");
        }
        if (std::filesystem::exists(std::filesystem::symlink_status(request.output_directory))) {
            fail(ErrorCode::IO, "bundle output already exists");
        }
        auto prices = read_file(request.prices_csv, request.limits, request.account),
             grid = read_file(request.sessions_csv, request.limits, request.account);
        verify(prices.text, request.expected_prices_sha256);
        verify(grid.text, request.expected_sessions_sha256);
        auto data = DatasetAccess::build(prices.text, grid.text, request.specification,
                                         request.limits, request.account);
        auto workspace = reserve(request.account, ::lexongine::ResourceKind::Scratch,
                                 request.limits.max_file_bytes * 3 + 65536);
        const auto report = report_text(data->changes, request.limits.max_file_bytes);
        const auto manifest = encode({request.specification, data->prices_hash, data->session_hash,
                                      hash(report), data->maximum_error, data->original_rows,
                                      data->rows.size(), data->changes.size()});
        if (manifest.size() > request.limits.max_file_bytes) {
            fail(ErrorCode::ResourceExceeded, "manifest exceeds byte ceiling");
        }
        if (!std::filesystem::create_directory(request.output_directory)) {
            fail(ErrorCode::IO, "bundle directory cannot be exclusively created");
        }
        storage::files::write_text(request.output_directory / "prices.csv", prices.text);
        storage::files::write_text(request.output_directory / "sessions.csv", grid.text);
        storage::files::write_text(request.output_directory / "normalization.csv", report);
        storage::files::write_text(request.output_directory / "manifest.qdaily", manifest);
        for (const auto &[name, digest] : std::array<std::pair<std::string, std::string>, 4>{
                 {{"prices.csv", data->prices_hash},
                  {"sessions.csv", data->session_hash},
                  {"normalization.csv", hash(report)},
                  {"manifest.qdaily", hash(manifest)}}}) {
            auto stored =
                read_file(request.output_directory / name, request.limits, request.account);
            verify(stored.text, digest);
        }
        data->manifest_hash = hash(manifest);
        storage::files::write_text(request.output_directory / "COMPLETE",
                                   std::string{daily_format} + " " + data->manifest_hash + "\n");
        storage::files::sync(request.output_directory);
        return DatasetAccess::finish(std::move(data));
    });
}
BacktestResult<DailyDataset> load_daily_dataset(const DatasetRequest &request) noexcept {
    return guard([&]() -> BacktestResult<DailyDataset> {
        validate_limits(request.limits, request.account);
        if (!request.directory.is_absolute() ||
            !std::filesystem::is_directory(std::filesystem::symlink_status(request.directory))) {
            fail(ErrorCode::IO, "bundle root must be an absolute real directory");
        }
        auto marker = read_file(request.directory / "COMPLETE", request.limits, request.account);
        std::istringstream completed(marker.text);
        std::string format, digest, extra;
        if (!(completed >> format >> digest) || completed >> extra || format != daily_format ||
            !digest_valid(digest)) {
            fail(ErrorCode::Integrity, "invalid daily completion marker");
        }
        if (request.expected_manifest_sha256 && *request.expected_manifest_sha256 != digest) {
            fail(ErrorCode::Integrity, "selected manifest differs from expected identity");
        }
        auto manifest =
            read_file(request.directory / "manifest.qdaily", request.limits, request.account);
        verify(manifest.text, digest);
        auto decoded = decode(manifest.text);
        auto prices = read_file(request.directory / "prices.csv", request.limits, request.account),
             grid = read_file(request.directory / "sessions.csv", request.limits, request.account),
             changes = read_file(request.directory / "normalization.csv", request.limits,
                                 request.account);
        verify(prices.text, decoded.prices);
        verify(grid.text, decoded.sessions);
        verify(changes.text, decoded.changes);
        auto data = DatasetAccess::build(prices.text, grid.text, decoded.spec, request.limits,
                                         request.account);
        auto scratch = reserve(request.account, ::lexongine::ResourceKind::Scratch,
                               request.limits.max_file_bytes * 2 + 65536);
        if (decoded.original_rows != data->original_rows ||
            decoded.selected_rows != data->rows.size() ||
            decoded.changed_values != data->changes.size() ||
            decoded.max_error != data->maximum_error ||
            report_text(data->changes, request.limits.max_file_bytes) != changes.text) {
            fail(ErrorCode::Integrity,
                 "normalization evidence/row counts disagree with retained input");
        }
        data->manifest_hash = digest;
        return DatasetAccess::finish(std::move(data));
    });
}
BacktestResult<std::string>
seal_reference_grid(const std::filesystem::path &original, std::string_view expected,
                    const std::filesystem::path &output, DatasetLimits limits,
                    std::shared_ptr<::lexongine::ResourceAccount> account) noexcept {
    return guard([&]() -> BacktestResult<std::string> {
        validate_limits(limits, account);
        if (!output.is_absolute()) {
            fail(ErrorCode::InvalidConfig, "reference grid output must be absolute");
        }
        auto bytes = read_file(original, limits, account);
        verify(bytes.text, expected);
        auto scratch =
            reserve(account, ::lexongine::ResourceKind::Scratch, limits.max_file_bytes + 65536);
        Lines lines{bytes.text};
        std::string_view line;
        if (!lines.next(line) || line != csv_header) {
            fail(ErrorCode::Unsupported, "unsupported original CSV header");
        }
        std::string grid = "Date\n";
        grid.reserve(static_cast<std::size_t>(limits.max_file_bytes));
        std::string_view previous;
        std::uint64_t count = 0;
        while (lines.next(line)) {
            const auto fields = columns(line);
            static_cast<void>(open_time(fields[0]));
            if (++count > limits.max_rows) {
                fail(ErrorCode::ResourceExceeded, "reference row count exceeded");
            }
            if (!previous.empty() && fields[0] <= previous) {
                fail(ErrorCode::Coverage, "original reference dates duplicate or regress");
            }
            previous = fields[0];
            if (fields[0].size() + 1 > limits.max_file_bytes - grid.size()) {
                fail(ErrorCode::ResourceExceeded, "reference grid byte bound exceeded");
            }
            grid += fields[0];
            grid += '\n';
        }
        if (!count) {
            fail(ErrorCode::Coverage, "empty original reference");
        }
        storage::files::write_text(output, grid);
        auto stored = read_file(output, limits, account);
        const auto digest = hash(grid);
        verify(stored.text, digest);
        return digest;
    });
}
const DatasetSpec &DailyDataset::specification() const noexcept {
    return data_->spec;
}
std::span<const DailyRow> DailyDataset::rows() const noexcept {
    return data_->rows;
}
std::span<const NormalizationChange> DailyDataset::normalizations() const noexcept {
    return data_->changes;
}
std::string_view DailyDataset::maximum_normalization_error() const noexcept {
    return data_->maximum_error;
}
std::string_view DailyDataset::manifest_sha256() const noexcept {
    return data_->manifest_hash;
}
std::string_view DailyDataset::prices_sha256() const noexcept {
    return data_->prices_hash;
}
std::string_view DailyDataset::sessions_sha256() const noexcept {
    return data_->session_hash;
}
std::uint64_t DailyDataset::owned_bytes() const noexcept {
    return data_->bytes;
}
std::shared_ptr<::lexongine::ResourceAccount> DailyDataset::account() const noexcept {
    return data_->account;
}
BacktestResult<OpeningProjection> DailyDataset::opening(std::uint64_t key,
                                                        std::int64_t now) const noexcept {
    return guard([&]() -> BacktestResult<OpeningProjection> {
        if (key >= data_->rows.size()) {
            return error(ErrorCode::Coverage, "opening key outside dataset",
                         backtest::Phase::Opening);
        }
        const auto &row = data_->rows[static_cast<std::size_t>(key)];
        if (now < row.bar.interval_start.unix_nanos()) {
            return error(ErrorCode::FutureRead, "opening projection is not yet available",
                         backtest::Phase::Opening);
        }
        return OpeningProjection{{data_->manifest_hash, data_->spec.instrument, data_->spec.venue,
                                  data_->spec.source, data::CurrencyCode{data_->spec.currency}},
                                 key,
                                 row.bar.open,
                                 row.bar.interval_start.unix_nanos()};
    });
}
BacktestResult<CloseProjection> DailyDataset::closing(std::uint64_t key,
                                                      std::int64_t now) const noexcept {
    return guard([&]() -> BacktestResult<CloseProjection> {
        if (key >= data_->rows.size()) {
            return error(ErrorCode::Coverage, "closing key outside dataset",
                         backtest::Phase::Closing);
        }
        const auto &row = data_->rows[static_cast<std::size_t>(key)];
        if (now < row.close_available_ns) {
            return error(ErrorCode::FutureRead, "closing projection is not yet available",
                         backtest::Phase::Closing);
        }
        return CloseProjection{{data_->manifest_hash, data_->spec.instrument, data_->spec.venue,
                                data_->spec.source, data::CurrencyCode{data_->spec.currency}},
                               key,
                               row.bar,
                               row.close_available_ns};
    });
}
} // namespace quant::research
