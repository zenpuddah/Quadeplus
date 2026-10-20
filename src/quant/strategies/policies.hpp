#pragma once
#include "quant/backtest/host.hpp"
namespace quant::strategies {
inline constexpr std::string_view sma_strategy_id = "sma20_50_long_cash_v1";
[[nodiscard]] backtest::BacktestResult<backtest::Action>
sma20_50_decision(const backtest::DecisionContext &, const backtest::Account *,
                  std::span<const backtest::PriceFeature>) noexcept;
struct KeltnerState {
    std::string prefix;
    backtest::ExecutionPolicy costs;
    std::optional<std::uint64_t> last_key;
    std::optional<data::Price> previous_close, previous_upper, threshold, highest_close;
    bool held = false;
};
[[nodiscard]] backtest::BacktestResult<backtest::Action>
keltner_decision(KeltnerState &, const backtest::DecisionContext &, const backtest::Account *,
                 std::span<const backtest::PriceFeature>) noexcept;
[[nodiscard]] backtest::BacktestResult<void>
keltner_execution(KeltnerState &, const backtest::FillQuote &, const backtest::Account &) noexcept;
} // namespace quant::strategies
