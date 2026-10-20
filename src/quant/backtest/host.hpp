#pragma once
#include "quant/backtest/accounting.hpp"
#include "quant/backtest/risk.hpp"
#include "quant/research/daily_dataset.hpp"
#include "quant/strategies/stochastic_value.hpp"
#include "lexongine/execution.hpp"
#include "lexongine/plugins.hpp"
#include <functional>

namespace quant::backtest {
enum class FeatureKind { Price, Ratio };
struct RequiredObservation {
    std::string id;
    std::uint64_t begin, end;
    FeatureKind kind = FeatureKind::Price;
};
struct PriceFeature {
    std::string request;
    data::Price value;
    std::uint64_t key;
    std::int64_t available_at_ns;
};
struct RatioFeature {
    std::string request;
    strategies::StochasticValue value;
    std::uint64_t key;
    std::int64_t available_at_ns;
};
struct DecisionContext {
    std::uint64_t key;
    bool may_submit, funded;
    std::span<const RatioFeature> ratios{};
};
enum class ActionKind { Hold, TargetLong, TargetCash, FixedEntry };
struct Action {
    ActionKind kind;
    data::Quantity quantity;
    std::string reason;
    std::optional<data::Price> initial_stop_distance{};
};
using DecisionCallback = std::function<BacktestResult<Action>(
    const DecisionContext &, const Account *, std::span<const PriceFeature>)>;
using ExecutionCallback = std::function<BacktestResult<void>(const FillQuote &, const Account &)>;
struct TrialConfiguration {
    std::uint32_t id;
    std::string strategy_id, strategy_configuration;
    data::Money initial_cash;
    data::Quantity lot_step;
    ExecutionPolicy execution;
    RiskPolicy risk;
    TerminalMode terminal;
    std::vector<RequiredObservation> observations;
};
struct HostLimits {
    std::uint64_t max_trials, max_ledger_events, max_trace_records, max_evidence_bytes;
};
struct SuiteConfiguration {
    std::uint64_t funding_key, evaluation_begin, evaluation_end;
    bool legacy_funding_close;
    HostLimits limits;
    ::lexongine::RequiredEvidenceSpec financial_evidence;
    std::vector<TrialConfiguration> trials;
};
struct HostEvent {
    std::uint32_t trial;
    std::uint64_t key, id;
    std::string kind, reason;
    std::int64_t cash, units, basis, realized, dividends, fees;
    std::optional<std::int64_t> equity;
    std::optional<ValuationPhase> phase;
    std::int64_t price, quantity, gross, fee;
    std::optional<std::uint64_t> decision_key, eligible_key;
    std::string quantity_policy;
    std::int64_t budget;
};
struct TrialResult {
    std::optional<Account> account;
    std::optional<Ledger> ledger;
    std::optional<RiskState> risk;
    std::optional<Intent> pending;
    std::uint64_t next_id = 1;
};
struct FinancialRun {
    ::lexongine::Reservation charge;
    std::vector<TrialResult> trials;
    std::vector<HostEvent> events;
    std::string effective_configuration;
    ::lexongine::Sha256 experiment{};
    bool exhausted = false, finalized = false;
    std::optional<BacktestError> failure;
};
struct HostReport {
    std::shared_ptr<const FinancialRun> financial;
    ::lexongine::RunReport engine;
};
// Explicit adapter state permits an external host to drive the same checked
// phases. run() is the ordinary complete synchronous driver.
class HostedSimulation {
  public:
    [[nodiscard]] static BacktestResult<HostedSimulation>
    create(research::DailyDataset, std::shared_ptr<const ::lexongine::PreparedPlan>,
           const ::lexongine::HostBuild &, SuiteConfiguration, std::vector<DecisionCallback>,
           const ::lexongine::RegisteredType<data::Price> &,
           std::vector<ExecutionCallback> executions = {},
           std::optional<::lexongine::RegisteredType<strategies::StochasticValue>> ratio =
               {}) noexcept;
    [[nodiscard]] BacktestResult<::lexongine::RunServices>
        services(::lexongine::ExecutionEvidence,
                 std::vector<::lexongine::RequiredEvidenceSink>) noexcept;
    [[nodiscard]] BacktestResult<void> before_open(std::uint64_t key) noexcept;
    [[nodiscard]] BacktestResult<void> after_step(const ::lexongine::StepReceipt &) noexcept;
    [[nodiscard]] BacktestResult<void> finalize_financial() noexcept;
    [[nodiscard]] BacktestResult<HostReport>
        run(::lexongine::ExecutionEvidence,
            std::vector<::lexongine::RequiredEvidenceSink>) noexcept;
    [[nodiscard]] std::shared_ptr<const FinancialRun> financial() const noexcept;
    [[nodiscard]] ::lexongine::IntegrationCompletion completion() const;

  private:
    struct State;
    explicit HostedSimulation(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};
[[nodiscard]] ::lexongine::RequiredEvidenceSpec financial_evidence_spec(std::string absolute_path,
                                                                        const HostLimits &);
} // namespace quant::backtest
