#pragma once

#include "quant/strategies/stochastic_value.hpp"
#include "quant/backtest/host.hpp"
#include "quant/lexongine/components.hpp"

namespace lexongine {
template <> struct PublicationTraits<quant::strategies::StochasticValue> {
    using type = quant::strategies::StochasticValue;
    static constexpr auto storage = StorageKind::ScalarCopy;
};
} // namespace lexongine

namespace quant::strategies {
[[nodiscard]] ::lexongine::TypeContract stochastic_type_contract();
// Key {"quadeplus", "stochastic5", 1, "stochastic-v1"}; input "bar", output "k".
// Required parameters: window=5, zero_range=1/2, coverage=strict_complete_bars.
// Initialization is empty_at_step_0. The source identity binds the date origin;
// observer scope must include every legacy ready output starting at source key 4.
// Sharing is eligible; both operation/output persistent cacheability are false.
[[nodiscard]] ::lexongine::Result<::lexongine::RegisteredType<StochasticValue>>
register_stochastic(::lexongine::Registry &, const quant::lexongine::ComponentTypes &,
                    const quant::lexongine::ComponentRegistration &) noexcept;

enum class StochasticProfile { Fresh, Legacy };
inline constexpr std::string_view stochastic_strategy_id = "spy_stochastic5_v1";
inline constexpr std::string_view stochastic_fresh_policy = "fresh_rejected_episode_consumed_v1";
inline constexpr std::string_view stochastic_legacy_policy = "legacy_rejection_fails_parity_v1";

// One fresh instance per financial trial. Market-only state never owns an account.
// Configure profile before the first callback and keep it fixed for the run.
struct StochasticState {
    StochasticProfile profile = StochasticProfile::Fresh;
    std::optional<std::uint64_t> last_key, episode_start;
    std::optional<std::uint32_t> trial;
    std::optional<data::Side> pending_side;
    std::optional<std::uint64_t> pending_decision;
    std::int64_t held_units = 0;
    bool ready_seen = false, activated = false, entry_consumed = false;
    bool parity_failed = false, terminal = false;
};
// Call after successful graph advancement. optional K means numerical Warming,
// never permission to reset a previously ready indicator or erase an episode.
// Fresh state first activates when may_submit becomes true; Legacy replays K
// before funding. At the legacy funding close, may_submit must already be true.
[[nodiscard]] backtest::BacktestResult<backtest::Action>
stochastic_decision(StochasticState &, const backtest::DecisionContext &, const backtest::Account *,
                    std::optional<StochasticValue>) noexcept;
// Invoke once after a posted fill or an ordinary rejection (account is post-event).
// Compatible with ExecutionCallback via a lambda capturing this trial's state.
// A synthetic terminal sale is supported without a next-open decision/intent.
[[nodiscard]] backtest::BacktestResult<void>
stochastic_execution(StochasticState &, const backtest::FillQuote &,
                     const backtest::Account &) noexcept;
} // namespace quant::strategies
