#include "suite_publication.hpp"
#include "quant/storage/file_integrity.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace quant::backtest::app::suite_publication {
namespace {
namespace lx = ::lexongine;
namespace fs = std::filesystem;
void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
template <class T> T engine(std::variant<T, lx::Diagnostic> value) {
    if (const auto *error = std::get_if<lx::Diagnostic>(&value)) {
        throw std::runtime_error(std::string{error->text()});
    }
    return std::get<T>(std::move(value));
}
std::string hex(const lx::Sha256 &value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : value) {
        auto n = std::to_integer<unsigned>(byte);
        result += digits[n >> 4];
        result += digits[n & 15];
    }
    return result;
}
std::string hash(std::string_view text) {
    return hex(engine(lx::files::digest(text)));
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
    auto relative = path.lexically_relative(root);
    check(!relative.empty() && !relative.is_absolute(), "evidence outside suite directory");
    for (const auto &part : relative) {
        check(part != ".." && part != ".", "unsafe suite evidence path");
    }
    return relative.generic_string();
}
File describe(std::string path, std::string_view text) {
    check(text.size() <= metadata_limit, "suite aggregate exceeds metadata bound");
    return {path, "suite_artifact_v1", std::move(path), text.size(), hash(text)};
}
void verify_inventory(const fs::path &root, std::span<const File> files, const IO &io) {
    check(!files.empty() && files.size() <= 512, "suite file count exceeds ceiling");
    std::set<std::string> expected, actual;
    std::uint64_t total = 0;
    for (const auto &file : files) {
        check(file.path != "acceptance.json" && file.path != "acceptance.pending.json" &&
                  expected.insert(file.path).second,
              "duplicate or self-referential suite inventory path");
        check(file.bytes <= file_limit && total <= 16ULL * 1024 * 1048576 - file.bytes,
              "suite byte inventory exceeds ceiling");
        total += file.bytes;
        verify_file(root, file, io);
    }
    std::size_t entries = 0;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        check(++entries <= 544, "suite directory inventory exceeds ceiling");
        const auto status = entry.symlink_status();
        check(!fs::is_symlink(status), "symlink in suite inventory");
        if (fs::is_directory(status)) {
            continue;
        }
        check(fs::is_regular_file(status), "nonregular suite evidence");
        actual.insert(relative_path(root, entry.path()));
    }
    check(actual == expected, "suite contains missing or undeclared files");
}

// Once entered, rejection is no longer a truthful outcome. All callback errors,
// including exceptions without an acknowledgment, require bounded reconciliation.
Outcome publish_marker(const fs::path &pending, const fs::path &final, std::string_view marker,
                       const IO &io) noexcept {
    try {
        auto result = io.publish(pending, final);
        if (std::holds_alternative<std::monostate>(result)) {
            return {SuiteStatus::Accepted, Stage::None};
        }
    } catch (...) {
    }
    for (unsigned attempt = 0; attempt < reconciliation_reads; ++attempt) {
        try {
            auto result = io.read(final, metadata_limit);
            if (const auto *bytes = std::get_if<lx::files::Bytes>(&result);
                bytes && bytes->value == marker) {
                return {SuiteStatus::Accepted, Stage::None};
            }
        } catch (...) {
        }
    }
    return {SuiteStatus::Unconfirmed, Stage::Visibility};
}
} // namespace

IO native_io(std::shared_ptr<lx::ResourceAccount> account) {
    check(account != nullptr, "suite publication requires a resource account");
    return {[account](const fs::path &path, std::string_view contents) {
                return lx::files::write_new(path.filename().string(), path, contents, account,
                                            metadata_limit, lx::FailureCode::EvidenceIO);
            },
            [account](const fs::path &path, std::uint64_t maximum) {
                return lx::files::read(path, account, maximum);
            },
            [](const fs::path &pending, const fs::path &final) {
                return lx::files::publish(pending, final);
            }};
}
Aggregate aggregate(std::string path, std::string contents) {
    auto expected = describe(std::move(path), contents);
    return {std::move(expected), std::move(contents)};
}
File write_file(const fs::path &root, std::string path, std::string_view contents, const IO &io) {
    const auto target = root / path;
    check(relative_path(root, target) == path, "unsafe suite output path");
    auto expected = describe(std::move(path), contents);
    auto reference = engine(io.write(target, contents));
    check(reference.finalized && reference.absolute_path == target.string() &&
              reference.byte_count == expected.bytes && hex(reference.digest) == expected.sha,
          "suite write did not close the expected bytes");
    return expected;
}
void verify_file(const fs::path &root, const File &file, const IO &io) {
    check(relative_path(root, root / file.path) == file.path && file.bytes <= file_limit,
          "invalid bounded suite file");
    auto bytes = engine(io.read(root / file.path, file_limit));
    check(bytes.value.size() == file.bytes && hash(bytes.value) == file.sha,
          "closed suite evidence changed or cannot be read back");
}
std::string_view failure_text(Stage stage) noexcept {
    switch (stage) {
    case Stage::None:
        return "suite publication completed";
    case Stage::Input:
        return "suite finalization inputs are incomplete, unsafe or outside bounds";
    case Stage::AggregateWrite:
        return "required suite aggregate write/close failed";
    case Stage::Inventory:
        return "complete suite inventory/readback failed before publication";
    case Stage::Marker:
        return "pending suite marker write/close/readback failed before publication";
    case Stage::Visibility:
        return "suite marker publication visibility is unresolved; inspect retained final/pending "
               "paths";
    }
    return "unknown suite publication stage";
}
Outcome finalize(const fs::path &root, std::span<const File> inventory,
                 const Aggregates &aggregates, std::string_view marker, const IO &io) noexcept {
    static_assert(std::is_trivially_copyable_v<Outcome>);
    Stage stage = Stage::Input;
    try {
        path_ok(root);
        check(io.write && io.read && io.publish && fs::is_directory(root),
              "incomplete suite publication binding/root");
        check(!marker.empty() && marker.size() <= metadata_limit && !inventory.empty() &&
                  inventory.size() <= 512,
              "suite marker/inventory exceeds bound");
        constexpr std::array<std::string_view, 4> names{"registry.csv", "cost-comparison.csv",
                                                        "index.html", "manifest.json"};
        for (std::size_t i = 0; i < aggregates.size(); ++i) {
            const auto &item = aggregates[i];
            check(item.expected.path == names[i] &&
                      item.expected == describe(item.expected.path, item.contents),
                  "suite aggregate bytes/identity/order mismatch");
            check(std::count(inventory.begin(), inventory.end(), item.expected) == 1,
                  "required aggregate missing/duplicated in suite inventory");
        }
        const auto pending_path = root / "acceptance.pending.json",
                   final_path = root / "acceptance.json";
        stage = Stage::AggregateWrite;
        for (const auto &item : aggregates) {
            check(write_file(root, item.expected.path, item.contents, io) == item.expected,
                  "closed aggregate differs from expected inventory");
        }
        stage = Stage::Inventory;
        verify_inventory(root, inventory, io);
        stage = Stage::Marker;
        const auto pending = write_file(root, "acceptance.pending.json", marker, io);
        verify_file(root, pending, io);
        // Everything fallible outside reconciliation has finished. This tail call
        // returns only a POD; the noexcept helper cannot escape into a rejection.
        return publish_marker(pending_path, final_path, marker, io);
    } catch (...) {
        return {SuiteStatus::Rejected, stage};
    }
}
} // namespace quant::backtest::app::suite_publication
