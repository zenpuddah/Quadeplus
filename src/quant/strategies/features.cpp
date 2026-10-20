#include "quant/strategies/features.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/data/validation.hpp"
#include <array>

namespace quant::strategies {
namespace {
namespace lx = ::lexongine;
using data::Price;
using data::Bar;
struct Failure {
    lx::Diagnostic value;
};
template <class T> T get(std::variant<T, lx::Diagnostic> result) {
    if (auto *e = std::get_if<lx::Diagnostic>(&result)) {
        throw Failure{*e};
    }
    return std::get<T>(std::move(result));
}
template <class F> auto safe(F &&fn) noexcept -> decltype(fn()) {
    return lx::guarded([&]() -> decltype(fn()) {
        try {
            return fn();
        } catch (const Failure &e) {
            return e.value;
        }
    });
}
[[noreturn]] void fail(lx::FailureCode code, std::string_view message) {
    throw Failure{lx::diagnostic(code, message)};
}
lx::Contract fact(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
Price weighted(Price a, std::uint64_t wa, Price b, std::uint64_t wb, std::uint64_t d) {
    auto result = backtest::weighted_price(a, wa, b, wb, d);
    if (auto *e = std::get_if<backtest::BacktestError>(&result)) {
        fail(lx::FailureCode::NumericOverflow, e->text());
    }
    return std::get<Price>(result);
}
lx::ParameterField fixed(std::string name, std::string value) {
    lx::ParameterConstraints c;
    c.allowed_text.push_back(std::move(value));
    return {std::move(name), lx::ParamKind::Text, true, std::move(c)};
}
lx::ParameterSchema schema(std::string_view kind) {
    lx::ParameterConstraints bound;
    bound.signed_min = 1;
    bound.signed_max = INT64_MAX / 50;
    lx::ParameterSchema s{1,
                          {{"max_input_raw", lx::ParamKind::Signed64, true, bound},
                           fixed("rounding", "nearest_ties_even"),
                           fixed("overflow", "fail")}};
    if (kind == "ema20") {
        s.fields.push_back(fixed("seed", "mean_first_20"));
        s.fields.push_back(fixed("recurrence", "2_current_19_previous_over_21"));
    } else if (kind == "atr10") {
        s.fields.push_back(fixed("seed", "mean_first_10"));
        s.fields.push_back(fixed("recurrence", "9_previous_current_over_10"));
    } else if (kind == "true_range") {
        s.fields.push_back(fixed("seed", "first_high_low"));
    } else {
        lx::ParameterConstraints c;
        c.unsigned_min = 1;
        c.unsigned_max = 5;
        s.fields.push_back({"numerator", lx::ParamKind::Unsigned64, true, c});
        s.fields.push_back({"denominator", lx::ParamKind::Unsigned64, true, c});
    }
    return s;
}
template <class T> T parameter(const lx::CanonicalParams &p, std::string_view name) {
    for (const auto &field : p.values) {
        if (field.name == name) {
            return std::get<T>(field.value);
        }
    }
    fail(lx::FailureCode::InvalidConfig, "missing feature parameter");
}
struct Params {
    lx::CanonicalParams canonical;
    std::int64_t bound;
    std::uint64_t num = 0, den = 1;
    bool ema = false;
};
lx::Result<Params> parse(const lx::ParamSet &input, std::string_view kind) {
    return safe([&]() -> lx::Result<Params> {
        auto p = get(lx::parse_parameters(input, schema(kind), 256));
        Params out{p, parameter<std::int64_t>(p, "max_input_raw")};
        out.ema = kind == "ema20";
        if (kind == "upper_channel") {
            out.num = parameter<std::uint64_t>(p, "numerator");
            out.den = parameter<std::uint64_t>(p, "denominator");
            if (!((out.num == 3 && out.den == 2) || (out.num == 2 && out.den == 1) ||
                  (out.num == 5 && out.den == 2))) {
                fail(lx::FailureCode::InvalidConfig,
                     "unsupported fixed Keltner demonstration width");
            }
        }
        return out;
    });
}
void initialization(const lx::PreparationFacts &facts, bool stateful) {
    const auto &init = facts.initialization;
    if (facts.missing_intervals != lx::MissingIntervalPolicy::Strict ||
        facts.alignment != lx::Alignment::SameStep ||
        init.contract.id != (stateful ? "empty_at_step_0" : "stateless") ||
        init.contract.version != 1 || init.contract.parameters.schema_version != 1 ||
        init.explicit_values.schema_version != 1 || !init.contract.parameters.values.empty() ||
        !init.explicit_values.values.empty() ||
        init.actual_prefix.clock != facts.processing_scope.clock ||
        init.actual_prefix.begin != init.actual_prefix.end ||
        init.actual_prefix.begin != facts.processing_scope.begin ||
        (stateful && facts.processing_scope.begin != 0)) {
        fail(lx::FailureCode::UnsupportedContract, "feature requires its complete declared origin");
    }
}
void bound(Price p, const Params &params, bool positive = false) {
    if (p.raw() < 0 || (positive && p.raw() == 0) || p.raw() > params.bound) {
        fail(lx::FailureCode::NumericBound, "Keltner feature input exceeds explicit bound");
    }
}
lx::OperationContract contract(std::string kind,
                               const quant::lexongine::ComponentRegistration &build,
                               std::vector<lx::InputContract> inputs, bool stateful) {
    return {
        {"quadeplus", kind, 1, "keltner-v1"},
        build.implementation,
        schema(kind),
        std::move(inputs),
        {{"price", quant::lexongine::price_contract(), lx::StorageKind::ScalarCopy,
          fact("qaedalus.price9_xml"), fact("qaedalus.price9"), true}},
        {stateful ? lx::SharingClass::StatefulDeclared : lx::SharingClass::StatelessDeterministic,
         fact(stateful ? "empty_at_step_0" : "stateless"),
         fact("same_step"),
         fact("none"),
         true,
         true,
         true,
         {"backtest_features", "keltner_fraction_oracle"}},
        {"interval_v1"},
        {lx::Alignment::SameStep},
        {lx::MissingIntervalPolicy::Strict},
        {"count"},
        "Pinned-origin, exact-rounding Keltner feature v1"};
}
lx::InputContract input(std::string name, lx::TypeContract type) {
    return {std::move(name), std::move(type), lx::Alignment::SameStep,
            lx::MissingIntervalPolicy::Strict, lx::AbsencePolicy::RequireValue};
}
struct Recurrence {
    lx::Reservation charge;
    std::array<std::optional<lx::ParentSelection>, 20> seed;
    std::vector<lx::ParentSelection> selected;
    std::uint64_t count = 0;
    std::int64_t sum = 0;
    Price previous = Price::from_raw(0);
};
struct Range {
    lx::Reservation charge;
    std::optional<Price> previous;
    std::optional<lx::ParentSelection> parent;
};
struct Empty {};
} // namespace
lx::Result<void>
register_keltner_features(lx::Registry &registry, const quant::lexongine::ComponentTypes &types,
                          const quant::lexongine::ComponentRegistration &build) noexcept {
    return safe([&]() -> lx::Result<void> {
        if (build.missing_intervals != lx::MissingIntervalPolicy::Strict ||
            build.implementation.binary_digest == lx::Sha256{} ||
            build.implementation.dependencies_digest == lx::Sha256{} ||
            build.implementation.source_build_digest == lx::Sha256{} ||
            build.implementation.sdk_build_contract_id.empty()) {
            fail(lx::FailureCode::InvalidConfig,
                 "Keltner features require explicit native identity and Strict input");
        }
        for (const bool ema : {true, false}) {
            const std::string kind = ema ? "ema20" : "atr10";
            lx::TypedOperation<Params, Recurrence, std::tuple<Price>, std::tuple<Price>> operation{
                contract(kind, build, {input("price", types.price.policy.contract)}, true),
                {{"price", types.price, 0}},
                {{"price", types.price}},
                ema ? +[](const lx::ParamSet &p) { return parse(p, "ema20"); }
                    : +[](const lx::ParamSet &p) { return parse(p, "atr10"); },
                [](const Params &p) -> lx::Result<lx::CanonicalParams> { return p.canonical; },
                [](const Params &p,
                   const lx::PreparationFacts &facts) -> lx::Result<lx::HistoryRequirement> {
                    return safe([&]() -> lx::Result<lx::HistoryRequirement> {
                        initialization(facts, true);
                        return lx::HistoryRequirement{lx::CountHistory{p.ema ? 19U : 9U}};
                    });
                },
                [](const Params &, const lx::PreparationFacts &) -> lx::Result<lx::ResourceBounds> {
                    return lx::ResourceBounds{0, 65536, 0, 65536, 4096, 8192, 0};
                },
                [](const Params &, const lx::InitializationSpec &,
                   lx::StateAllocator &allocator) -> lx::Result<Recurrence> {
                    return safe([&]() -> lx::Result<Recurrence> {
                        Recurrence state{get(allocator.reserve(65536)), {}, {}};
                        state.selected.reserve(20);
                        return state;
                    });
                },
                [](Recurrence &state, const Params &p,
                   const std::tuple<lx::ReadValue<Price>> &values, const lx::UpdateContext &,
                   lx::PublicationAllocator &allocator) -> lx::StepResult<std::tuple<Price>> {
                    return safe([&]() -> lx::StepResult<std::tuple<Price>> {
                        const auto length = p.ema ? 20U : 10U;
                        const auto ema = p.ema;
                        const auto &in = std::get<0>(values);
                        bound(in.value, p, ema);
                        if (state.count < length) {
                            state.sum = data::detail::checked_add(state.sum, in.value.raw());
                            state.seed[state.count] = in.parent();
                            ++state.count;
                            if (state.count < length) {
                                return lx::Warming{length - state.count};
                            }
                            state.previous = weighted(Price::from_raw(state.sum), 1,
                                                      Price::from_raw(0), 0, length);
                            for (std::size_t i = 0; i < length; ++i) {
                                state.selected.push_back(*state.seed[i]);
                            }
                            auto selected = get(allocator.parents(state.selected));
                            for (auto &parent : state.seed) {
                                parent.reset();
                            }
                            state.selected.clear();
                            return lx::Published{std::tuple{state.previous}, std::move(selected)};
                        }
                        state.previous = ema ? weighted(in.value, 2, state.previous, 19, 21)
                                             : weighted(state.previous, 9, in.value, 1, 10);
                        return lx::Published{std::tuple{state.previous},
                                             get(allocator.parents({in.parent()}))};
                    });
                },
                [](const Params &p, const std::tuple<Price> &output,
                   const lx::UpdateContext &) -> lx::Result<void> {
                    return safe([&]() -> lx::Result<void> {
                        bound(std::get<0>(output), p);
                        return lx::ok();
                    });
                },
                build.pin};
            get(registry.register_definition(std::move(operation)));
            get(registry.register_source_constraint<Price>(
                {{"quadeplus", kind, 1, "keltner-v1"},
                 build.implementation,
                 fact("quadeplus.keltner_input_bound_v1")},
                0, types.price,
                [](const Price &value, const lx::CanonicalParams &p,
                   const lx::UpdateContext &) -> lx::Result<void> {
                    return safe([&]() -> lx::Result<void> {
                        bound(value, {p, parameter<std::int64_t>(p, "max_input_raw")});
                        return lx::ok();
                    });
                },
                build.pin));
        }
        lx::TypedOperation<Params, Range, std::tuple<Bar>, std::tuple<Price>> range{
            contract("true_range", build, {input("bar", types.bar.policy.contract)}, true),
            {{"bar", types.bar, 1}},
            {{"price", types.price}},
            [](const lx::ParamSet &p) { return parse(p, "true_range"); },
            [](const Params &p) -> lx::Result<lx::CanonicalParams> { return p.canonical; },
            [](const Params &,
               const lx::PreparationFacts &facts) -> lx::Result<lx::HistoryRequirement> {
                return safe([&]() -> lx::Result<lx::HistoryRequirement> {
                    initialization(facts, true);
                    return lx::HistoryRequirement{lx::CountHistory{0}};
                });
            },
            [](const Params &, const lx::PreparationFacts &) -> lx::Result<lx::ResourceBounds> {
                return lx::ResourceBounds{0, 16384, 0, 16384, 4096, 8192, 0};
            },
            [](const Params &, const lx::InitializationSpec &,
               lx::StateAllocator &allocator) -> lx::Result<Range> {
                return safe([&]() -> lx::Result<Range> {
                    return Range{get(allocator.reserve(16384)), {}, {}};
                });
            },
            [](Range &state, const Params &p, const std::tuple<lx::ReadValue<Bar>> &values,
               const lx::UpdateContext &context,
               lx::PublicationAllocator &allocator) -> lx::StepResult<std::tuple<Price>> {
                return safe([&]() -> lx::StepResult<std::tuple<Price>> {
                    const auto &in = std::get<0>(values);
                    const auto &b = in.value;
                    const auto *extent = std::get_if<lx::Interval>(&context.extent);
                    if (!extent || extent->begin_ns != b.interval_start.unix_nanos() ||
                        extent->end_ns != b.interval_end.unix_nanos() ||
                        context.activation_available_at_ns < extent->end_ns ||
                        !data::validate(b).empty()) {
                        fail(lx::FailureCode::SourceInvalid,
                             "TR requires a valid complete Bar extent");
                    }
                    bound(b.high, p, true);
                    bound(b.low, p, true);
                    bound(b.close, p, true);
                    auto value = b.high.raw() - b.low.raw();
                    if (state.previous) {
                        const auto h = b.high.raw() - state.previous->raw(),
                                   l = b.low.raw() - state.previous->raw();
                        value = std::max({value, h < 0 ? -h : h, l < 0 ? -l : l});
                    }
                    auto parents = state.parent
                                       ? get(allocator.parents({*state.parent, in.parent()}))
                                       : get(allocator.parents({in.parent()}));
                    state.previous = b.close;
                    state.parent = in.parent();
                    return lx::Published{std::tuple{Price::from_raw(value)}, std::move(parents)};
                });
            },
            [](const Params &p, const std::tuple<Price> &output,
               const lx::UpdateContext &) -> lx::Result<void> {
                return safe([&]() -> lx::Result<void> {
                    bound(std::get<0>(output), p);
                    return lx::ok();
                });
            },
            build.pin};
        get(registry.register_definition(std::move(range)));
        get(registry.register_source_constraint<Bar>(
            {{"quadeplus", "true_range", 1, "keltner-v1"},
             build.implementation,
             fact("quadeplus.tr_bar_bound_v1")},
            0, types.bar,
            [](const Bar &value, const lx::CanonicalParams &p,
               const lx::UpdateContext &) -> lx::Result<void> {
                return safe([&]() -> lx::Result<void> {
                    Params params{p, parameter<std::int64_t>(p, "max_input_raw")};
                    bound(value.high, params, true);
                    bound(value.low, params, true);
                    bound(value.close, params, true);
                    return lx::ok();
                });
            },
            build.pin));
        lx::TypedOperation<Params, Empty, std::tuple<Price, Price>, std::tuple<Price>> channel{
            contract("upper_channel", build,
                     {input("ema", types.price.policy.contract),
                      input("atr", types.price.policy.contract)},
                     false),
            {{"ema", types.price, 0}, {"atr", types.price, 0}},
            {{"price", types.price}},
            [](const lx::ParamSet &p) { return parse(p, "upper_channel"); },
            [](const Params &p) -> lx::Result<lx::CanonicalParams> { return p.canonical; },
            [](const Params &,
               const lx::PreparationFacts &facts) -> lx::Result<lx::HistoryRequirement> {
                return safe([&]() -> lx::Result<lx::HistoryRequirement> {
                    initialization(facts, false);
                    return lx::HistoryRequirement{lx::CountHistory{0}};
                });
            },
            [](const Params &, const lx::PreparationFacts &) -> lx::Result<lx::ResourceBounds> {
                return lx::ResourceBounds{0, 0, 0, 16384, 4096, 8192, 0};
            },
            [](const Params &, const lx::InitializationSpec &,
               lx::StateAllocator &) -> lx::Result<Empty> { return Empty{}; },
            [](Empty &, const Params &p,
               const std::tuple<lx::ReadValue<Price>, lx::ReadValue<Price>> &values,
               const lx::UpdateContext &,
               lx::PublicationAllocator &allocator) -> lx::StepResult<std::tuple<Price>> {
                return safe([&]() -> lx::StepResult<std::tuple<Price>> {
                    const auto &ema = std::get<0>(values);
                    const auto &atr = std::get<1>(values);
                    bound(ema.value, p);
                    bound(atr.value, p);
                    return lx::Published{
                        std::tuple{weighted(ema.value, p.den, atr.value, p.num, p.den)},
                        get(allocator.parents({ema.parent(), atr.parent()}))};
                });
            },
            [](const Params &p, const std::tuple<Price> &output,
               const lx::UpdateContext &) -> lx::Result<void> {
                if (std::get<0>(output).raw() < 0 || std::get<0>(output).raw() > p.bound * 4) {
                    return lx::diagnostic(lx::FailureCode::OutputInvalid,
                                          "invalid upper channel bound");
                }
                return lx::ok();
            },
            build.pin};
        get(registry.register_definition(std::move(channel)));
        return lx::ok();
    });
}
} // namespace quant::strategies
