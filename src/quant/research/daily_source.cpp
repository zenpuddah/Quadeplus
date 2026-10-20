#include "quant/research/daily_source.hpp"

namespace quant::research {
namespace detail {
struct DailySourceBinding {
    // Pins outlive every typed callback/policy, and the reservation precedes copies.
    ::lexongine::ModulePin pin;
    ::lexongine::Reservation charge;
    DailyDataset dataset;
    DailySourceConfig config;
    ::lexongine::RegisteredType<data::Bar> type;
    ::lexongine::ImplementationIdentity implementation;
    ::lexongine::Sha256 content, references;
};
} // namespace detail
namespace {
struct SourceFailure {
    ::lexongine::Diagnostic diagnostic;
};
template <class T> T checked(std::variant<T, ::lexongine::Diagnostic> value) {
    if (const auto *failure = std::get_if<::lexongine::Diagnostic>(&value)) {
        throw SourceFailure{*failure};
    }
    return std::get<T>(std::move(value));
}
void checked(::lexongine::Result<void> value) {
    if (const auto *failure = std::get_if<::lexongine::Diagnostic>(&value)) {
        throw SourceFailure{*failure};
    }
}
[[noreturn]] void fail(::lexongine::FailureCode code, std::string_view message) {
    throw SourceFailure{::lexongine::diagnostic(code, message)};
}
template <class F> auto source_safe(F &&call) noexcept -> decltype(call()) {
    return ::lexongine::guarded([&]() -> decltype(call()) {
        try {
            return call();
        } catch (const SourceFailure &failure) {
            return failure.diagnostic;
        }
    });
}
template <class F> auto backtest_safe(F &&call) noexcept -> decltype(call()) {
    try {
        return call();
    } catch (const SourceFailure &failure) {
        const auto code = failure.diagnostic.code;
        return backtest::error(code == ::lexongine::FailureCode::ResourceExceeded ||
                                       code == ::lexongine::FailureCode::ResourcePlanExceeded
                                   ? backtest::ErrorCode::ResourceExceeded
                                   : backtest::ErrorCode::SourceBinding,
                               failure.diagnostic.text(), backtest::Phase::Source);
    } catch (const std::bad_alloc &) {
        return backtest::error(backtest::ErrorCode::ResourceExceeded,
                               "daily source allocation failed", backtest::Phase::Source);
    } catch (const std::exception &error) {
        return backtest::error(backtest::ErrorCode::SourceBinding, error.what(),
                               backtest::Phase::Source);
    } catch (...) {
        return backtest::error(backtest::ErrorCode::SourceBinding, "daily source callback failed",
                               backtest::Phase::Source);
    }
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    return checked(::lexongine::checked_add(a, b));
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    return checked(::lexongine::checked_multiply(a, b));
}
::lexongine::Contract contract(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
::lexongine::Sha256 digest(std::string_view text) {
    if (text.size() != 64) {
        fail(::lexongine::FailureCode::SourceInvalid, "daily bundle digest must be SHA-256");
    }
    ::lexongine::Sha256 result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') {
                return static_cast<unsigned>(c - '0');
            }
            if (c >= 'a' && c <= 'f') {
                return static_cast<unsigned>(c - 'a' + 10);
            }
            fail(::lexongine::FailureCode::SourceInvalid, "noncanonical daily bundle digest");
        };
        result[i] = static_cast<std::byte>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    }
    if (result == ::lexongine::Sha256{}) {
        fail(::lexongine::FailureCode::SourceInvalid, "unresolved daily bundle digest");
    }
    return result;
}
::lexongine::CanonicalParams policies(const detail::DailySourceBinding &binding) {
    const auto &config = binding.config;
    return {
        1,
        {{"adjustment", std::string{adjustment_profile}, ::lexongine::Origin::VersionedFact,
          "daily-bundle"},
         {"availability", config.availability, ::lexongine::Origin::ExplicitConfig, "daily-source"},
         {"clock", config.clock, ::lexongine::Origin::ExplicitConfig, "daily-source"},
         {"completeness", config.completeness, ::lexongine::Origin::ExplicitConfig, "daily-source"},
         {"currency", config.currency, ::lexongine::Origin::ExplicitConfig, "daily-source"},
         {"empty", config.empty, ::lexongine::Origin::ExplicitConfig, "daily-source"},
         {"manifest_sha256", std::string{binding.dataset.manifest_sha256()},
          ::lexongine::Origin::VersionedFact, "daily-bundle"},
         {"missing", config.missing, ::lexongine::Origin::ExplicitConfig, "daily-source"},
         {"missing_intervals", std::string{"strict"}, ::lexongine::Origin::ExplicitConfig,
          "daily-source"},
         {"normalization", std::string{decimal_profile}, ::lexongine::Origin::VersionedFact,
          "daily-bundle"},
         {"prices_sha256", std::string{binding.dataset.prices_sha256()},
          ::lexongine::Origin::VersionedFact, "daily-bundle"},
         {"sessions_sha256", std::string{binding.dataset.sessions_sha256()},
          ::lexongine::Origin::VersionedFact, "daily-bundle"},
         {"volume", std::string{volume_profile}, ::lexongine::Origin::VersionedFact,
          "daily-bundle"}}};
}
void scope_valid(const detail::DailySourceBinding &binding,
                 const ::lexongine::SequenceScope &scope) {
    if (scope.clock != binding.config.clock || scope.begin >= scope.end ||
        scope.end > binding.dataset.rows().size()) {
        fail(::lexongine::FailureCode::SourceCoverage,
             "daily source scope is empty, outside the dataset, or on another clock");
    }
}
void request_valid(const detail::DailySourceBinding &binding,
                   const ::lexongine::SourceRequest &request,
                   const ::lexongine::SourceValidationServices &services) {
    if (services.account != binding.dataset.account()) {
        fail(::lexongine::FailureCode::InvalidConfig,
             "daily dataset adoption requires its original ResourceAccount");
    }
    if (request.provider_name != binding.config.provider_name ||
        request.snapshot_name != binding.config.snapshot_name ||
        request.requested_version != binding.dataset.manifest_sha256()) {
        fail(::lexongine::FailureCode::SourceChanged,
             "daily source snapshot/version/provider mismatch");
    }
    if (!(request.interpretation == binding.type.policy.contract)) {
        fail(::lexongine::FailureCode::TypeMismatch,
             "daily source requires its exact registered Bar interpretation");
    }
    if (services.missing_intervals != ::lexongine::MissingIntervalPolicy::Strict ||
        !(request.provider_policies == policies(binding))) {
        fail(::lexongine::FailureCode::UnsupportedContract,
             "daily source requires its exact sealed policies and Strict coverage");
    }
    scope_valid(binding, request.required_scope);
}
struct Inspection {
    ::lexongine::SequenceScope scope;
};
struct SourceData {
    std::shared_ptr<const detail::DailySourceBinding> binding;
    ::lexongine::Reservation charge;
    ::lexongine::SourceMetadata metadata;
    std::vector<::lexongine::TimingDescriptor> timeline;
};
struct Cursor {
    std::size_t position = 0;
};
} // namespace

const DailyDataset &RegisteredDailySource::dataset() const noexcept {
    return binding_->dataset;
}
const DailySourceConfig &RegisteredDailySource::config() const noexcept {
    return binding_->config;
}
BacktestResult<::lexongine::SourceRequest>
RegisteredDailySource::request(const ::lexongine::SequenceScope &scope) const noexcept {
    return backtest_safe([&]() -> BacktestResult<::lexongine::SourceRequest> {
        if (!binding_) {
            fail(::lexongine::FailureCode::InvalidConfig, "daily source binding was moved");
        }
        scope_valid(*binding_, scope);
        return ::lexongine::SourceRequest{binding_->config.provider_name,
                                          binding_->config.snapshot_name,
                                          std::string{binding_->dataset.manifest_sha256()},
                                          binding_->type.policy.contract,
                                          scope,
                                          policies(*binding_)};
    });
}

BacktestResult<RegisteredDailySource>
register_daily_source(::lexongine::Registry &registry, ::lexongine::SourceResolver &resolver,
                      const ::lexongine::RegisteredType<data::Bar> &type,
                      const DailyDataset &dataset, const DailySourceConfig &config,
                      const quant::lexongine::ComponentRegistration &build) noexcept {
    return backtest_safe([&]() -> BacktestResult<RegisteredDailySource> {
        const auto &limits = config.limits;
        const auto &spec = dataset.specification();
        if (!dataset.account() || dataset.rows().empty() || !dataset.owned_bytes() ||
            !limits.max_records || !limits.dataset_bytes || !limits.metadata_bytes ||
            !limits.control_bytes || !limits.string_bytes) {
            fail(::lexongine::FailureCode::InvalidConfig,
                 "daily source requires a charged dataset and explicit nonzero limits");
        }
        if (config.clock != clock_profile || spec.clock != clock_profile ||
            config.availability != clock_profile || spec.adjustment != adjustment_profile ||
            spec.normalization != decimal_profile || spec.volume != volume_profile ||
            spec.currency != "USD" || config.currency != "USD" ||
            config.completeness != "complete_only" || config.empty != "reject" ||
            config.missing != "fail" ||
            config.missing_intervals != ::lexongine::MissingIntervalPolicy::Strict ||
            build.missing_intervals != config.missing_intervals) {
            fail(::lexongine::FailureCode::UnsupportedContract,
                 "unsupported daily source clock, currency, or policy");
        }
        if (build.implementation.binary_digest == ::lexongine::Sha256{} ||
            build.implementation.dependencies_digest == ::lexongine::Sha256{} ||
            build.implementation.source_build_digest == ::lexongine::Sha256{} ||
            !config.operation.semantic_version || !spec.instrument || !spec.venue || !spec.source) {
            fail(::lexongine::FailureCode::InvalidConfig,
                 "daily source requires resolved operation, instrument, and build identities");
        }
        std::uint64_t text_bytes = 0;
        for (const std::string_view text :
             {std::string_view{config.provider_name}, std::string_view{config.snapshot_name},
              std::string_view{config.clock}, std::string_view{config.operation.name_space},
              std::string_view{config.operation.name},
              std::string_view{config.operation.implementation_selector},
              std::string_view{build.implementation.sdk_build_contract_id},
              std::string_view{spec.version}, std::string_view{spec.symbol},
              std::string_view{spec.provenance}, std::string_view{spec.schedule}}) {
            checked(::lexongine::validate_text(text, limits.string_bytes, false));
            text_bytes = add(text_bytes, add(text.size(), 32));
        }
        const auto type_bytes = checked(::lexongine::type_storage_bytes(type.policy.contract));
        const auto control_minimum = add(16384, multiply(add(text_bytes, type_bytes), 8));
        const auto per_row =
            add(sizeof(::lexongine::TimingDescriptor) + sizeof(::lexongine::UpdateKey),
                multiply(config.clock.size() + 32, 2));
        const auto metadata_minimum =
            add(control_minimum, multiply(dataset.rows().size(), multiply(per_row, 2)));
        if (dataset.rows().size() > limits.max_records ||
            dataset.owned_bytes() > limits.dataset_bytes ||
            limits.control_bytes < control_minimum || limits.metadata_bytes < metadata_minimum) {
            fail(::lexongine::FailureCode::ResourcePlanExceeded,
                 "daily source dataset, timeline, or control ceiling exceeded");
        }
        auto charge = checked(
            dataset.account()->reserve(::lexongine::ResourceKind::Graph, limits.control_bytes));
        if (!(type.policy.contract == quant::lexongine::bar_contract()) ||
            type.witness.type != std::type_index(typeid(data::Bar)) ||
            type.policy.storage != ::lexongine::StorageKind::ImmutableOwned ||
            !type.policy.validate || !type.policy.measure || !type.policy.freeze ||
            (type.policy.freeze_strategy != ::lexongine::FreezeStrategy::DeepCopy &&
             type.policy.freeze_strategy != ::lexongine::FreezeStrategy::ExclusiveConstruction)) {
            fail(::lexongine::FailureCode::TypeMismatch,
                 "daily source requires the registered immutable Bar policy");
        }
        checked(registry.describe_type<data::Bar>(type.policy.contract));
        if (registry.frozen() ||
            std::holds_alternative<std::shared_ptr<const ::lexongine::OperationDefinition>>(
                registry.resolve(config.operation))) {
            fail(::lexongine::FailureCode::InvalidConfig,
                 "daily source operation registry is frozen or key already registered");
        }
        const auto content = digest(dataset.manifest_sha256());
        const auto references = digest(dataset.sessions_sha256());
        (void)digest(dataset.prices_sha256());
        auto pin =
            checked(::lexongine::combine_pins(dataset.account(), build.pin, type.policy.pin));
        pin =
            checked(::lexongine::combine_pins(dataset.account(), std::move(pin), type.witness.pin));
        auto binding = std::make_shared<const detail::DailySourceBinding>(
            detail::DailySourceBinding{std::move(pin), std::move(charge), dataset, config, type,
                                       build.implementation, content, references});
        ::lexongine::TypedSourceProvider<data::Bar, Inspection, SourceData, Cursor> provider{
            config.provider_name,
            type,
            [binding](const ::lexongine::SourceRequest &request,
                      ::lexongine::SourceValidationServices &services)
                -> ::lexongine::Result<Inspection> {
                return source_safe([&]() -> ::lexongine::Result<Inspection> {
                    request_valid(*binding, request, services);
                    return Inspection{request.required_scope};
                });
            },
            [binding](const Inspection &) -> ::lexongine::Result<::lexongine::SourceBounds> {
                return source_safe([&]() -> ::lexongine::Result<::lexongine::SourceBounds> {
                    const auto &bound = binding->config.limits;
                    // Source bytes are already reserved by DailyDataset; adoption does
                    // not copy or charge them again. Engine wrappers reserve themselves.
                    return ::lexongine::SourceBounds{
                        {binding->config.clock, 0, binding->dataset.rows().size()},
                        {binding->dataset.owned_bytes(), bound.control_bytes, 0, 8192,
                         sizeof(data::Bar) + 512, add(bound.metadata_bytes, bound.control_bytes),
                         0}};
                });
            },
            [binding](Inspection &&inspection, const ::lexongine::SourceRequest &request,
                      ::lexongine::SourceValidationServices &services)
                -> ::lexongine::Result<SourceData> {
                return source_safe([&]() -> ::lexongine::Result<SourceData> {
                    request_valid(*binding, request, services);
                    if (inspection.scope != request.required_scope) {
                        fail(::lexongine::FailureCode::SourceChanged,
                             "daily scope changed after inspection");
                    }
                    auto metadata_charge = checked(services.account->reserve(
                        ::lexongine::ResourceKind::Graph, binding->config.limits.metadata_bytes));
                    ::lexongine::SourceCoverage coverage{
                        inspection.scope, {}, {}, contract("quadeplus.sealed_daily_grid_v1")};
                    std::vector<::lexongine::TimingDescriptor> timeline;
                    const auto count =
                        static_cast<std::size_t>(inspection.scope.end - inspection.scope.begin);
                    coverage.complete_keys.reserve(count);
                    timeline.reserve(count);
                    const auto rows = binding->dataset.rows();
                    for (auto key = inspection.scope.begin; key < inspection.scope.end; ++key) {
                        const auto &row = rows[static_cast<std::size_t>(key)];
                        const auto start = row.bar.interval_start.unix_nanos(),
                                   end = row.bar.interval_end.unix_nanos();
                        if (start % 86400000000000LL != 0 ||
                            start > std::numeric_limits<std::int64_t>::max() - 1000000000LL ||
                            end != start + 1000000000LL || row.close_available_ns != end ||
                            (key + 1 < rows.size() &&
                             end >= rows[static_cast<std::size_t>(key + 1)]
                                        .bar.interval_start.unix_nanos())) {
                            fail(::lexongine::FailureCode::SourceInvalid,
                                 "daily Bar extent/availability differs from the sealed synthetic "
                                 "clock");
                        }
                        coverage.complete_keys.push_back({binding->config.clock, key});
                        timeline.push_back(
                            {{binding->config.clock, key},
                             ::lexongine::Interval{start, end},
                             row.close_available_ns,
                             ::lexongine::CompleteRecord{key - inspection.scope.begin}});
                    }
                    ::lexongine::SourceMetadata metadata{
                        binding->config.snapshot_name,
                        std::string{binding->dataset.manifest_sha256()},
                        binding->content,
                        binding->type.policy.contract,
                        {binding->pin, typeid(data::Bar)},
                        inspection.scope,
                        std::move(coverage),
                        contract("interval_v1"),
                        contract("quadeplus.original_research_grid_keys_v1"),
                        contract(binding->config.availability),
                        contract("quadeplus.sealed_daily_bars_v1"),
                        binding->references,
                        binding->implementation};
                    return SourceData{binding, std::move(metadata_charge), std::move(metadata),
                                      std::move(timeline)};
                });
            },
            [](const SourceData &data) -> ::lexongine::Result<::lexongine::SourceMetadata> {
                return data.metadata;
            },
            [](const SourceData &data)
                -> ::lexongine::Result<std::span<const ::lexongine::TimingDescriptor>> {
                return std::span<const ::lexongine::TimingDescriptor>{data.timeline};
            },
            [](const SourceData &, ::lexongine::SourceCursorServices &)
                -> ::lexongine::Result<Cursor> { return Cursor{}; },
            [](Cursor &cursor, const SourceData &data, const ::lexongine::UpdateContext &context,
               ::lexongine::PublicationAllocator &allocator)
                -> ::lexongine::Result<::lexongine::SourceDelivery<quant::data::Bar>> {
                if (cursor.position >= data.timeline.size()) {
                    return ::lexongine::diagnostic(::lexongine::FailureCode::SourceCoverage,
                                                   "daily cursor exhausted; finish separately");
                }
                const auto &timing = data.timeline[cursor.position];
                if (context.key != timing.key || context.extent != timing.extent ||
                    context.activation_available_at_ns != timing.available_at_ns) {
                    return ::lexongine::diagnostic(
                        ::lexongine::FailureCode::UpdateProtocol,
                        "daily activation differs from validated timing");
                }
                if (context.decision_time_ns < timing.available_at_ns) {
                    return ::lexongine::diagnostic(::lexongine::FailureCode::FutureRead,
                                                   "daily close is not yet available");
                }
                const auto index =
                    static_cast<std::size_t>(data.metadata.validated_scope.begin) + cursor.position;
                auto output = allocator.copy_output(0, data.binding->dataset.rows()[index].bar);
                if (const auto *failure = std::get_if<::lexongine::Diagnostic>(&output)) {
                    return *failure;
                }
                ::lexongine::SourceRecord<quant::data::Bar> record{
                    timing, std::get<::lexongine::Immutable<quant::data::Bar>>(std::move(output))};
                ++cursor.position;
                return ::lexongine::SourceDelivery<quant::data::Bar>{std::move(record)};
            },
            [](Cursor &cursor,
               const SourceData &data) -> ::lexongine::Result<::lexongine::SourceEnd> {
                if (cursor.position != data.timeline.size()) {
                    return ::lexongine::diagnostic(
                        ::lexongine::FailureCode::SourceCoverage,
                        "daily cursor has not delivered its complete scope");
                }
                return ::lexongine::SourceEnd{data.metadata.validated_scope};
            },
            binding->pin};
        checked(resolver.register_provider(std::move(provider)));
        ::lexongine::OperationContract description{
            config.operation,
            build.implementation,
            {1, {}},
            {},
            {{"bar", type.policy.contract, ::lexongine::StorageKind::ImmutableOwned,
              contract("unsupported_bar_codec"), contract("quadeplus.sealed_daily_bars_v1"),
              false}},
            {::lexongine::SharingClass::StatefulDeclared,
             contract("source_cursor"),
             contract("same_step"),
             contract("none"),
             true,
             true,
             false,
             {"backtest_dataset"}},
            {"interval_v1"},
            {::lexongine::Alignment::SameStep},
            {::lexongine::MissingIntervalPolicy::Strict},
            {"count"},
            "Immutable sealed daily research Bars; source-service construction only"};
        checked(registry.register_definition(
            std::move(description), binding->pin,
            [binding](const ::lexongine::ParamSet &, const ::lexongine::PreparationFacts &)
                -> ::lexongine::Result<::lexongine::PreparedKernel> {
                (void)binding; // Retains the registration charge and all lifetime pins.
                return ::lexongine::diagnostic(
                    ::lexongine::FailureCode::UnsupportedContract,
                    "daily source is constructed through SourceResolver");
            }));
        return RegisteredDailySource{std::move(binding)};
    });
}
} // namespace quant::research
