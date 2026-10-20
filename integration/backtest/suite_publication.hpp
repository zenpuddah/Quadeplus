#pragma once
#include "suite.hpp"
#include "native_files.hpp"
#include <array>
#include <functional>
#include <span>
#include <string_view>

namespace quant::backtest::app {
// Internal bounded suite-publication API, not a cache store or CLI fault hook.
namespace suite_publication {
inline constexpr std::uint64_t metadata_limit = 4 * 1048576ULL, file_limit = 512 * 1048576ULL;
inline constexpr unsigned reconciliation_reads = 3;
struct File {
    std::string name, format, path;
    std::uint64_t bytes;
    std::string sha;
    friend bool operator==(const File &, const File &) = default;
};
struct Aggregate {
    File expected;
    std::string contents;
};
// Exactly registry, cost comparison, HTML index, then manifest. Descriptors are
// expected bytes/hashes until finalize() closes and independently verifies them.
using Aggregates = std::array<Aggregate, 4>;

// Native callbacks use the existing bounded write_new (including close/readback),
// read and atomic no-replace publish helpers. Tests may wrap individual calls.
// write must never publish acceptance; read must not mutate evidence. Publish
// errors/throws do NOT establish nonvisibility, even when they occur before rename.
struct IO {
    std::function<::lexongine::Result<::lexongine::EvidenceReference>(const std::filesystem::path &,
                                                                      std::string_view)>
        write;
    std::function<::lexongine::Result<::lexongine::files::Bytes>(const std::filesystem::path &,
                                                                 std::uint64_t)>
        read;
    std::function<::lexongine::Result<void>(const std::filesystem::path &,
                                            const std::filesystem::path &)>
        publish;
};
[[nodiscard]] IO native_io(std::shared_ptr<::lexongine::ResourceAccount>);
[[nodiscard]] Aggregate aggregate(std::string relative_path, std::string contents);
// Prepublication helpers throw on failure; suite's existing failure path retains
// prefixes. These same helpers are used both by child closure and final inventory.
[[nodiscard]] File write_file(const std::filesystem::path &root, std::string relative_path,
                              std::string_view, const IO &);
void verify_file(const std::filesystem::path &root, const File &, const IO &);

enum class Stage { None, Input, AggregateWrite, Inventory, Marker, Visibility };
struct Outcome {
    SuiteStatus status;
    Stage stage;
};
[[nodiscard]] std::string_view failure_text(Stage) noexcept;

// Caller owns the exclusively created root and supplies the complete expected
// inventory (including all four aggregates, excluding either acceptance path).
// The caller renders the marker from that inventory/manifest before calling.
// No financial/JSON semantics are inferred here: suite() has already checked the
// accepted children and constructed its fixed schema and inspected identities.
//
// Writes/closes the four aggregates -> verifies the entire bounded directory and
// hashes -> writes/closes/readbacks the pending marker -> publishes exactly once.
// Any prepublication failure is Rejected. After a publish error or exception,
// read the final path at most three times. Only exact marker bytes confirm Accepted;
// missing, mismatched, unreadable or throwing reads remain Unconfirmed. A successful
// publish acknowledgment performs zero further reads, writes or mandatory work.
//
// This noexcept POD return and catch-all reconciliation perform no diagnostic
// allocation after confirmation. The caller must prepare its owning success and
// unconfirmed results beforehand and move them out without fallible postprocessing.
// Prefixes are never removed/repaired. Existing native power-loss semantics apply.
[[nodiscard]] Outcome finalize(const std::filesystem::path &root, std::span<const File> inventory,
                               const Aggregates &, std::string_view marker, const IO &) noexcept;
} // namespace suite_publication
} // namespace quant::backtest::app
