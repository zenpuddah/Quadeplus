#pragma once
#include "quant/backtest/host.hpp"
#include <filesystem>
namespace quant::backtest::app {
struct Config {
    std::string symbol, origin, begin, end, xml;
    std::int64_t initial_cash, lot, max_price_raw;
    ExecutionPolicy costs;
    std::string strategy = "sma20_50_long_cash_v1", sharing = "exact";
    std::filesystem::path cache_directory{};
    std::string profile = "fresh_research_v1";
    TerminalMode terminal = TerminalMode::Mark;
};
struct Summary {
    std::uint32_t trial;
    std::string strategy;
    std::int64_t cash, units, equity, pnl, realized, unrealized, dividends, fees, drawdown_loss,
        drawdown_peak;
};
struct ApplicationRun {
    HostReport host;
    std::shared_ptr<const std::vector<Summary>> summaries;
    std::int64_t preparation_us = 0, advance_us = 0, finalization_us = 0, total_us = 0;
};
[[nodiscard]] std::shared_ptr<::lexongine::ResourceAccount> resources(bool keltner = false);
[[nodiscard]] ::lexongine::RequiredEvidenceSink
make_sink(::lexongine::RequiredEvidenceSpec, const std::shared_ptr<::lexongine::ResourceAccount> &,
          std::function<std::string()>);
[[nodiscard]] ::lexongine::RequiredEvidenceSink
make_report_sink(::lexongine::RequiredEvidenceSpec,
                 const std::shared_ptr<::lexongine::ResourceAccount> &,
                 std::function<std::string(const ::lexongine::RunReport &)>);
[[nodiscard]] std::string escape_xml(std::string_view);
void require_new_output(const std::filesystem::path &);
[[nodiscard]] BacktestResult<Config> read_config(const std::filesystem::path &) noexcept;
[[nodiscard]] BacktestResult<research::DailyDataset>
prepare_spy(const std::filesystem::path &research, const std::filesystem::path &out) noexcept;
[[nodiscard]] BacktestResult<ApplicationRun> run(const research::DailyDataset &, const Config &,
                                                 const std::filesystem::path &out,
                                                 const ::lexongine::HostBuild &) noexcept;
[[nodiscard]] BacktestResult<ApplicationRun> run_keltner(const research::DailyDataset &,
                                                         const Config &,
                                                         const std::filesystem::path &out,
                                                         const ::lexongine::HostBuild &) noexcept;
[[nodiscard]] BacktestResult<ApplicationRun>
run_stochastic(const research::DailyDataset &, const Config &, const std::filesystem::path &out,
               const ::lexongine::HostBuild &) noexcept;
} // namespace quant::backtest::app
