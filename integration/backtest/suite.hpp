#pragma once
#include "application.hpp"

namespace quant::backtest::app {
enum class SuiteStatus { Accepted, Rejected, Unconfirmed };
struct SuiteRun {
    SuiteStatus status = SuiteStatus::Rejected;
    std::uint32_t completed_children = 0;
    std::string input_manifest_sha256;
    std::string manifest_sha256;
    std::string diagnostic;
};
// Absolute normalized paths; output must be new beneath an existing directory.
// No required I/O occurs after confirmed publication. Caller may project stdout.
// Aggregate closure/inventory/publication uses the internal suite_publication.hpp
// protocol. Its Unconfirmed outcome is preserved, never translated to rejection.
[[nodiscard]] BacktestResult<SuiteRun> suite(const std::filesystem::path &configPath,
                                             const std::filesystem::path &datasetPath,
                                             const std::filesystem::path &out,
                                             const ::lexongine::HostBuild &) noexcept;
} // namespace quant::backtest::app
