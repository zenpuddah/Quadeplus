#pragma once
#include "quant/backtest/model.hpp"
namespace quant::backtest {
struct RiskPolicy {
    std::optional<std::uint32_t> drawdown_bps, daily_loss_bps;
};
enum class RiskReason { None, Drawdown, DailyLoss };
struct RiskState {
    data::Money high_water, previous_close;
    bool latched = false;
    RiskReason reason = RiskReason::None;
    std::optional<std::uint64_t> last_close;
};
struct RiskDecision {
    bool liquidate, block_entries, new_trigger;
    RiskReason reason;
};
[[nodiscard]] BacktestResult<void> validate_risk_policy(const RiskPolicy &) noexcept;
[[nodiscard]] BacktestResult<RiskState> initialize_risk(data::Money initial) noexcept;
[[nodiscard]] BacktestResult<RiskDecision>
evaluate_risk(RiskState &, const Account &, const EquityPoint &, const RiskPolicy &) noexcept;
} // namespace quant::backtest
