#include "quant/strategies/stochastic.hpp"
#include "quant/backtest/detail.hpp"
#include "quant/data/validation.hpp"

#include <array>
#include <limits>

namespace quant::strategies {
namespace {
namespace lx = ::lexongine;
namespace qb = backtest;
struct Failure {
    lx::Diagnostic value;
};
template <class T> T take(std::variant<T, lx::Diagnostic> result) {
    if (const auto *error = std::get_if<lx::Diagnostic>(&result)) {
        throw Failure{*error};
    }
    return std::get<T>(std::move(result));
}
template <class F> auto safe(F &&fn) noexcept -> decltype(fn()) {
    return lx::guarded([&]() -> decltype(fn()) {
        try {
            return fn();
        } catch (const Failure &error) {
            return error.value;
        }
    });
}
[[noreturn]] void fail(lx::FailureCode code, std::string_view message) {
    throw Failure{lx::diagnostic(code, message)};
}
lx::Contract fact(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
lx::ParameterField fixed(std::string name, std::string value) {
    lx::ParameterConstraints constraints;
    constraints.allowed_text.push_back(std::move(value));
    return {std::move(name), lx::ParamKind::Text, true, std::move(constraints)};
}
lx::ParameterSchema schema() {
    lx::ParameterConstraints window;
    window.unsigned_min = window.unsigned_max = 5;
    return {1,
            {{"window", lx::ParamKind::Unsigned64, true, window},
             fixed("zero_range", "1/2"),
             fixed("coverage", "strict_complete_bars")}};
}
lx::Result<void> validate(const StochasticValue &value) {
    if (!valid_stochastic(value)) {
        return lx::diagnostic(lx::FailureCode::OutputInvalid, "K must be an exact ratio in [0,1]");
    }
    return lx::ok();
}
void initialization(const lx::PreparationFacts &facts) {
    const auto &init = facts.initialization;
    if (facts.missing_intervals != lx::MissingIntervalPolicy::Strict ||
        facts.alignment != lx::Alignment::SameStep || facts.processing_scope.clock.empty() ||
        facts.processing_scope.begin != 0 || facts.processing_scope.end == 0 ||
        init.contract.id != "empty_at_step_0" || init.contract.version != 1 ||
        init.contract.parameters.schema_version != 1 || !init.contract.parameters.values.empty() ||
        init.explicit_values.schema_version != 1 || !init.explicit_values.values.empty() ||
        init.actual_prefix.clock != facts.processing_scope.clock || init.actual_prefix.begin != 0 ||
        init.actual_prefix.end != 0) {
        fail(lx::FailureCode::UnsupportedContract,
             "stochastic requires Strict full-origin empty_at_step_0 initialization");
    }
}
struct Window {
    lx::Reservation charge;
    std::array<std::int64_t, 5> highs{}, lows{};
    std::array<std::optional<lx::ParentSelection>, 5> parents{};
    std::size_t head = 0, count = 0;
    std::optional<std::uint64_t> last_key{};
    std::optional<std::int64_t> previous_end{};
};
constexpr std::uint64_t state_bytes = sizeof(Window) + 256;

// Exact comparisons 5*n < d and 5*n > 4*d without overflowing either
// cross-product, even at INT64_MAX. For >80 use 5*(d-n) < d.
bool less_than_fifth(std::int64_t n, std::int64_t d) noexcept {
    return n < d / 5 || (n == d / 5 && d % 5 != 0);
}
[[noreturn]] void policy_fail(qb::ErrorCode code, std::string_view message,
                              qb::Phase phase = qb::Phase::Closing) {
    throw qb::detail::Failure{qb::error(code, message, phase)};
}
qb::Action action(qb::ActionKind kind, std::string reason) {
    return {kind, data::Quantity::from_raw(0), std::move(reason)};
}
} // namespace

::lexongine::TypeContract stochastic_type_contract() {
    return {"quadeplus.stochastic_ratio",
            1,
            {1,
             {{"meaning", std::string{"K_over_100_exact_0_to_1"}, lx::Origin::VersionedFact,
               "S3 dimensionless ratio"}}}};
}

lx::Result<lx::RegisteredType<StochasticValue>>
register_stochastic(lx::Registry &registry, const quant::lexongine::ComponentTypes &types,
                    const quant::lexongine::ComponentRegistration &build) noexcept {
    return safe([&]() -> lx::Result<lx::RegisteredType<StochasticValue>> {
        if (build.missing_intervals != lx::MissingIntervalPolicy::Strict ||
            build.implementation.binary_digest == lx::Sha256{} ||
            build.implementation.source_build_digest == lx::Sha256{} ||
            build.implementation.dependencies_digest == lx::Sha256{} ||
            build.implementation.sdk_build_contract_id.empty()) {
            fail(lx::FailureCode::InvalidConfig,
                 "stochastic requires explicit build identity and Strict coverage");
        }
        auto type = take(registry.bind_type<StochasticValue>(
            {stochastic_type_contract(), lx::StorageKind::ScalarCopy, validate,
             [](const StochasticValue &) -> lx::Result<std::uint64_t> {
                 return sizeof(StochasticValue);
             },
             lx::FreezeStrategy::ScalarCopy, [](const StochasticValue &value) { return value; },
             build.pin}));
        lx::OperationContract contract{
            {"quadeplus", "stochastic5", 1, "stochastic-v1"},
            build.implementation,
            schema(),
            {{"bar", types.bar.policy.contract, lx::Alignment::SameStep,
              lx::MissingIntervalPolicy::Strict, lx::AbsencePolicy::RequireValue}},
            {{"k", type.policy.contract, lx::StorageKind::ScalarCopy, fact("none"),
              fact("quadeplus.stochastic_ratio"), false}},
            {lx::SharingClass::StatefulDeclared,
             fact("empty_at_step_0"),
             fact("same_step"),
             fact("none"),
             true,
             true,
             false,
             {"backtest_stochastic", "stochastic_fraction_reference"}},
            {"interval_v1"},
            {lx::Alignment::SameStep},
            {lx::MissingIntervalPolicy::Strict},
            {"count"},
            "Exact five-completed-Bar stochastic ratio; flat=1/2; no persistent codec"};
        lx::TypedOperation<lx::CanonicalParams, Window, std::tuple<data::Bar>,
                           std::tuple<StochasticValue>>
            operation{
                std::move(contract),
                {{"bar", types.bar, 0}},
                {{"k", type}},
                [](const lx::ParamSet &input) -> lx::Result<lx::CanonicalParams> {
                    return lx::parse_parameters(input, schema(), 256);
                },
                [](const lx::CanonicalParams &params) -> lx::Result<lx::CanonicalParams> {
                    return params;
                },
                [](const lx::CanonicalParams &,
                   const lx::PreparationFacts &facts) -> lx::Result<lx::HistoryRequirement> {
                    return safe([&]() -> lx::Result<lx::HistoryRequirement> {
                        initialization(facts);
                        return lx::HistoryRequirement{lx::CountHistory{4}};
                    });
                },
                [](const lx::CanonicalParams &,
                   const lx::PreparationFacts &facts) -> lx::Result<lx::ResourceBounds> {
                    return safe([&]() -> lx::Result<lx::ResourceBounds> {
                        const auto certificates = take(
                            lx::checked_multiply(5, take(lx::parent_certificate_storage_bytes(
                                                        facts.processing_scope.clock.size()))));
                        return lx::ResourceBounds{
                            0,    state_bytes, 0, take(lx::checked_add(certificates, 8192)),
                            4096, 8192,        0};
                    });
                },
                [](const lx::CanonicalParams &, const lx::InitializationSpec &,
                   lx::StateAllocator &allocator) -> lx::Result<Window> {
                    return safe([&]() -> lx::Result<Window> {
                        return Window{take(allocator.reserve(state_bytes))};
                    });
                },
                [](Window &state, const lx::CanonicalParams &,
                   const std::tuple<lx::ReadValue<data::Bar>> &values,
                   const lx::UpdateContext &context, lx::PublicationAllocator &allocator)
                    -> lx::StepResult<std::tuple<StochasticValue>> {
                    return safe([&]() -> lx::StepResult<std::tuple<StochasticValue>> {
                        const auto &input = std::get<0>(values);
                        const auto &bar = input.value;
                        const auto *extent = std::get_if<lx::Interval>(&context.extent);
                        if (!extent || extent->begin_ns != bar.interval_start.unix_nanos() ||
                            extent->end_ns != bar.interval_end.unix_nanos() ||
                            context.activation_available_at_ns < extent->end_ns ||
                            !data::validate(bar).empty() || bar.low.raw() <= 0 ||
                            bar.high.raw() < bar.low.raw()) {
                            fail(lx::FailureCode::SourceInvalid,
                                 "stochastic requires a valid positive complete Bar extent");
                        }
                        const auto &publication = input.publication();
                        if (publication.key != context.key ||
                            publication.extent != context.extent ||
                            publication.available_at_ns > context.activation_available_at_ns ||
                            publication.available_at_ns < extent->end_ns) {
                            fail(lx::FailureCode::SourceInvalid,
                                 "stochastic input publication extent/availability mismatch");
                        }
                        const auto key = context.key.sequence;
                        if ((!state.last_key && key != 0) ||
                            (state.last_key &&
                             (key <= *state.last_key || key - *state.last_key != 1)) ||
                            (state.previous_end && extent->begin_ns < *state.previous_end)) {
                            fail(lx::FailureCode::UpdateProtocol,
                                 "stochastic requires successive source keys and nonoverlapping "
                                 "intervals");
                        }
                        state.highs[state.head] = bar.high.raw();
                        state.lows[state.head] = bar.low.raw();
                        state.parents[state.head] = input.parent();
                        state.head = (state.head + 1) % 5;
                        state.count = std::min(state.count + 1, std::size_t{5});
                        state.last_key = key;
                        state.previous_end = extent->end_ns;
                        if (state.count < 5) {
                            return lx::Warming{5 - state.count};
                        }
                        const auto high = *std::max_element(state.highs.begin(), state.highs.end());
                        const auto low = *std::min_element(state.lows.begin(), state.lows.end());
                        // Positive signed64 prices bound both subtractions to [0,INT64_MAX).
                        const StochasticValue value =
                            high == low ? StochasticValue{1, 2}
                                        : StochasticValue{bar.close.raw() - low, high - low};
                        take(validate(value));
                        const std::array<lx::ParentSelection, 5> parents{
                            *state.parents[state.head], *state.parents[(state.head + 1) % 5],
                            *state.parents[(state.head + 2) % 5],
                            *state.parents[(state.head + 3) % 5],
                            *state.parents[(state.head + 4) % 5]};
                        return lx::Published{std::tuple{value}, take(allocator.parents(parents))};
                    });
                },
                [](const lx::CanonicalParams &, const std::tuple<StochasticValue> &values,
                   const lx::UpdateContext &) -> lx::Result<void> {
                    return validate(std::get<0>(values));
                },
                build.pin};
        take(registry.register_definition(std::move(operation)));
        return type;
    });
}

qb::BacktestResult<qb::Action> stochastic_decision(StochasticState &state,
                                                   const qb::DecisionContext &context,
                                                   const qb::Account *account,
                                                   std::optional<StochasticValue> k) noexcept {
    return qb::detail::guard(
        [&]() -> qb::BacktestResult<qb::Action> {
            if (state.parity_failed || state.terminal) {
                policy_fail(qb::ErrorCode::Integrity,
                            "stochastic trial is already failed or finalized");
            }
            if (state.profile != StochasticProfile::Fresh &&
                state.profile != StochasticProfile::Legacy) {
                policy_fail(qb::ErrorCode::InvalidConfig,
                            "unknown stochastic initialization profile");
            }
            if (state.last_key &&
                (context.key <= *state.last_key || context.key - *state.last_key != 1)) {
                policy_fail(qb::ErrorCode::Coverage,
                            "stochastic observations require successive source keys");
            }
            if (state.profile == StochasticProfile::Legacy && !state.last_key && context.key > 4) {
                policy_fail(qb::ErrorCode::Coverage,
                            "legacy episode replay must include the first ready historical K");
            }
            if (k && !valid_stochastic(*k)) {
                policy_fail(qb::ErrorCode::InvalidData, "invalid stochastic ratio");
            }
            if (!k && (state.ready_seen || context.key >= 4)) {
                policy_fail(qb::ErrorCode::SourceBinding,
                            "required stochastic observation missing after readiness");
            }
            if (k && context.key < 4) {
                policy_fail(qb::ErrorCode::SourceBinding,
                            "stochastic cannot be ready before five Bars");
            }
            if (context.funded != (account != nullptr) || (context.may_submit && !context.funded)) {
                policy_fail(qb::ErrorCode::Integrity,
                            "stochastic account/funding context mismatch");
            }
            if (account && ((state.trial && *state.trial != account->trial) ||
                            account->units.raw() != state.held_units)) {
                policy_fail(qb::ErrorCode::Integrity,
                            "stochastic trial/account execution state differs");
            }
            if (state.pending_side) {
                policy_fail(qb::ErrorCode::Integrity,
                            "stochastic decision precedes pending execution notification");
            }
            if (state.activated && !context.may_submit) {
                policy_fail(qb::ErrorCode::Coverage,
                            "stochastic financial activation cannot regress");
            }
            auto next = state; // Failures leave decision state unchanged.
            next.last_key = context.key;
            next.ready_seen = next.ready_seen || k.has_value();
            if (account) {
                next.trial = account->trial;
            }
            const auto finish = [&](qb::ActionKind kind, std::string reason) -> qb::Action {
                auto result = action(kind, std::move(reason));
                state = next;
                return result;
            };
            if (state.profile == StochasticProfile::Fresh && !context.may_submit) {
                return finish(qb::ActionKind::Hold, "warmup");
            }
            next.activated = next.activated || context.may_submit;
            if (!k) {
                return finish(qb::ActionKind::Hold, "warming");
            }
            std::string reason;
            if (next.episode_start) {
                const bool threshold =
                    less_than_fifth(k->denominator - k->numerator, k->denominator);
                if (threshold || context.key - *next.episode_start >= 10) {
                    next.episode_start.reset();
                    next.entry_consumed = false;
                    reason = threshold ? "threshold_exit" : "timer_exit";
                } else {
                    reason = "active";
                }
            } else if (less_than_fifth(k->numerator, k->denominator)) {
                next.episode_start = context.key;
                next.entry_consumed = false;
                reason = "activate";
            } else {
                reason = "inactive";
            }
            if (!context.may_submit) {
                return finish(qb::ActionKind::Hold, "warmup");
            }
            if (next.episode_start && !next.entry_consumed && next.held_units == 0) {
                next.entry_consumed = true;
                next.pending_side = data::Side::Buy;
                next.pending_decision = context.key;
                return finish(qb::ActionKind::TargetLong, std::move(reason));
            }
            if (!next.episode_start && next.held_units > 0) {
                next.pending_side = data::Side::Sell;
                next.pending_decision = context.key;
                return finish(qb::ActionKind::TargetCash, std::move(reason));
            }
            return finish(qb::ActionKind::Hold, std::move(reason));
        },
        account ? std::optional{account->trial} : std::nullopt);
}

qb::BacktestResult<void> stochastic_execution(StochasticState &state, const qb::FillQuote &quote,
                                              const qb::Account &account) noexcept {
    return qb::detail::guard(
        [&]() -> qb::BacktestResult<void> {
            const auto bad = [](std::string_view message) {
                policy_fail(qb::ErrorCode::Integrity, message, qb::Phase::Opening);
            };
            if (state.parity_failed || state.terminal || !state.trial ||
                *state.trial != account.trial) {
                bad("unexpected stochastic execution callback");
            }
            if (const auto *rejected = std::get_if<qb::OrderRejection>(&quote)) {
                if (rejected->trial != account.trial || !rejected->id ||
                    state.pending_side != data::Side::Buy || state.held_units != 0 ||
                    account.units.raw() != 0) {
                    bad("unexpected rejected stochastic order");
                }
                state.pending_side.reset();
                state.pending_decision.reset();
                if (state.profile == StochasticProfile::Legacy) {
                    state.parity_failed = true;
                    bad("legacy stochastic entry rejected: parity gate failed");
                }
                return qb::ok(); // Consume episode; its activation/timer are untouched.
            }
            const auto &fill = std::get<qb::Fill>(quote);
            if (fill.trial != account.trial || !fill.id || fill.mark.source() != account.source ||
                fill.quantity.raw() <= 0 ||
                fill.expected_revision == std::numeric_limits<std::uint64_t>::max() ||
                account.revision != fill.expected_revision + 1) {
                bad("stochastic fill/account posting mismatch");
            }
            if (fill.synthetic_terminal) {
                if (fill.intent || fill.side != data::Side::Sell || state.held_units <= 0 ||
                    account.units.raw() != 0 || fill.quantity.raw() != state.held_units ||
                    fill.mark.phase() != qb::ValuationPhase::Close) {
                    bad("invalid stochastic synthetic terminal sale");
                }
                state.held_units = 0;
                state.pending_side.reset();
                state.pending_decision.reset();
                state.terminal = true;
                return qb::ok();
            }
            if (!fill.intent || state.pending_side != fill.side || !state.pending_decision ||
                fill.intent->trial != account.trial ||
                fill.intent->decision_key != *state.pending_decision ||
                fill.mark.phase() != qb::ValuationPhase::Open ||
                fill.mark.key() <= *state.pending_decision ||
                fill.mark.key() - *state.pending_decision != 1 ||
                fill.intent->earliest_open_key != fill.mark.key()) {
                bad("stochastic fill lacks its exact next-open decision");
            }
            if (fill.side == data::Side::Buy) {
                if (state.held_units != 0 || !state.episode_start || !state.entry_consumed ||
                    account.units != fill.quantity ||
                    !std::holds_alternative<qb::BuyCashBudget>(fill.intent->order)) {
                    bad("invalid stochastic entry receipt");
                }
                state.held_units = account.units.raw();
            } else if (fill.side == data::Side::Sell) {
                if (state.held_units <= 0 || account.units.raw() != 0 ||
                    fill.quantity.raw() != state.held_units ||
                    !std::holds_alternative<qb::SellAll>(fill.intent->order)) {
                    bad("invalid stochastic full-sale receipt");
                }
                state.held_units = 0;
            } else {
                bad("unsupported stochastic fill side");
            }
            state.pending_side.reset();
            state.pending_decision.reset();
            return qb::ok();
        },
        account.trial);
}
} // namespace quant::strategies
