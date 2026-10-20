#include "backtest_source_checks.hpp"
#include "quant/research/daily_source.hpp"

#include <iostream>
#include <stdexcept>

namespace {
namespace lx = ::lexongine;
namespace qr = quant::research;
namespace ql = quant::lexongine;
void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}
template <class T, class E> T take(std::variant<T, E> result) {
    if (const auto *error = std::get_if<E>(&result)) {
        throw std::runtime_error(std::string{error->text()});
    }
    return std::get<T>(std::move(result));
}
void success(lx::Result<void> result) {
    (void)take(std::move(result));
}
template <class T> void rejects(std::variant<T, lx::Diagnostic> result, lx::FailureCode code) {
    const auto *error = std::get_if<lx::Diagnostic>(&result);
    require(error != nullptr, "expected source rejection");
    if (error->code != code) {
        throw std::runtime_error("unexpected source rejection: " + std::string{error->text()});
    }
}
template <class T> void rejects(std::variant<T, quant::backtest::BacktestError> result) {
    require(std::holds_alternative<quant::backtest::BacktestError>(result),
            "expected daily binding rejection");
}
std::shared_ptr<lx::ResourceAccount> run_account() {
    constexpr std::uint64_t mb = 1024 * 1024;
    return take(lx::ResourceAccount::create(
        {{8 * mb, 8 * mb, 8 * mb, 8 * mb, 8 * mb, 16 * mb, 16 * mb}, 1024, 1024, 16384, 128}));
}
qr::DailySourceConfig configuration(const qr::DailyDataset &dataset) {
    return {{5, dataset.owned_bytes(), 131072, 65536, 1024},
            std::string{qr::clock_profile},
            "quadeplus.daily_research",
            "five-synthetic-daily-bars",
            {"quadeplus", "daily_research", 1, "sealed-daily-source-v1"},
            "USD",
            "complete_only",
            "reject",
            "fail",
            std::string{qr::clock_profile},
            lx::MissingIntervalPolicy::Strict};
}
struct Setup {
    std::shared_ptr<lx::ResourceAccount> account;
    lx::Registry registry;
    ql::ComponentRegistration build;
    ql::ComponentTypes types;
    lx::SourceResolver resolver;
    qr::DailySourceConfig config;
    qr::RegisteredDailySource registered;
    lx::SourceValidationServices services;
    explicit Setup(const qr::DailyDataset &dataset)
        : account(dataset.account()), registry(take(lx::Registry::create())),
          build{{lx::Sha256{std::byte{1}}, lx::Sha256{std::byte{2}}, lx::Sha256{std::byte{3}},
                 "daily-source-synthetic-checks-v1"},
                std::make_shared<const int>(7),
                lx::MissingIntervalPolicy::Strict},
          types(take(ql::register_components(registry, build))),
          resolver(take(lx::SourceResolver::create(account, 65536, 4))),
          config(configuration(dataset)),
          registered(take(
              qr::register_daily_source(registry, resolver, types.bar, dataset, config, build))),
          services{
              account,
              {{5, 5, 0, 5}, 1024, 65536, 131072, 65536},
              lx::MissingIntervalPolicy::Strict,
              {dataset.owned_bytes(), 65536, 0, 8192, sizeof(quant::data::Bar) + 512, 262144, 0}} {
        success(registry.freeze());
        success(resolver.freeze());
    }
    lx::SourceRequest request(std::uint64_t begin = 0, std::uint64_t end = 5) {
        return take(registered.request({config.clock, begin, end}));
    }
    lx::SourceSnapshot prepare(std::uint64_t begin = 0, std::uint64_t end = 5) {
        auto selected = request(begin, end);
        auto candidate = take(resolver.inspect(selected, services));
        require(candidate.bounds().resources.source == registered.dataset().owned_bytes(),
                "dataset adoption lost its payload bound");
        return take(resolver.validate(std::move(candidate), selected, services));
    }
};
lx::UpdateContext current(const lx::TimingDescriptor &timing) {
    return {timing.key, timing.extent, timing.available_at_ns, timing.available_at_ns};
}
void same_bar(const quant::data::Bar &value, const quant::data::Bar &expected) {
    require(value.header == expected.header && value.interval_start == expected.interval_start &&
                value.interval_end == expected.interval_end && value.open == expected.open &&
                value.high == expected.high && value.low == expected.low &&
                value.close == expected.close && value.volume == expected.volume,
            "source publication changed the canonical immutable Bar");
}
void registration_and_requests(const qr::DailyDataset &dataset) {
    Setup setup{dataset};
    const auto definition = take(setup.registry.resolve(setup.config.operation));
    const auto &operation = definition->contract();
    auto snapshot = setup.prepare();
    require(operation.implementation == setup.build.implementation &&
                snapshot.metadata().provider_implementation == operation.implementation,
            "operation/provider build identities differ");
    require(operation.outputs.size() == 1 &&
                operation.outputs[0].type == setup.types.bar.policy.contract &&
                snapshot.output_contract().semantic == operation.outputs[0].type &&
                snapshot.output_contract().witness == typeid(quant::data::Bar) &&
                snapshot.metadata().witness.type == setup.types.bar.witness.type,
            "operation/provider Bar type witnesses differ");
    require(!operation.outputs[0].cacheable && !operation.sharing.cacheable &&
                operation.inputs.empty() &&
                operation.timing_protocols == std::vector<std::string>{"interval_v1"},
            "daily source unexpectedly gained a codec or numerical inputs");
    require(setup.registered.dataset().rows().data() == dataset.rows().data(),
            "source copied the dataset payload");
    require(snapshot.metadata().coverage.complete_keys.size() == 5 &&
                snapshot.metadata().coverage.gaps.empty(),
            "daily coverage is incomplete");
    require(snapshot.metadata().version == dataset.manifest_sha256() &&
                snapshot.metadata().content_digest != lx::Sha256{} &&
                snapshot.metadata().source_reference_digest != lx::Sha256{},
            "sealed source identities missing");
    lx::PreparationFacts facts{{setup.config.clock, 0, 5},
                               {{"test-prefix", 1, {1, {}}}, {setup.config.clock, 0, 0}, {1, {}}},
                               lx::MissingIntervalPolicy::Strict,
                               lx::Alignment::SameStep,
                               {0, 0, 0, 0, 0, 16384, 0},
                               setup.account};
    rejects(definition->prepare({}, facts), lx::FailureCode::UnsupportedContract);

    rejects(setup.registered.request({setup.config.clock, 2, 2}));
    rejects(setup.registered.request({setup.config.clock, 0, 6}));
    rejects(setup.registered.request({"another-clock", 0, 5}));
    auto request = setup.request();
    request.requested_version = "changed";
    rejects(setup.resolver.inspect(request, setup.services), lx::FailureCode::SourceChanged);
    request = setup.request();
    request.snapshot_name = "changed";
    rejects(setup.resolver.inspect(request, setup.services), lx::FailureCode::SourceChanged);
    request = setup.request();
    request.provider_name = "unregistered-provider";
    rejects(setup.resolver.inspect(request, setup.services), lx::FailureCode::UnknownDefinition);
    request = setup.request();
    request.required_scope = {setup.config.clock, 4, 6};
    rejects(setup.resolver.inspect(request, setup.services), lx::FailureCode::SourceCoverage);
    request = setup.request();
    request.interpretation.semantic_version = 2;
    rejects(setup.resolver.inspect(request, setup.services), lx::FailureCode::TypeMismatch);
    request = setup.request();
    std::get<std::string>(request.provider_policies.values.front().value) = "unknown-adjustment";
    rejects(setup.resolver.inspect(request, setup.services), lx::FailureCode::UnsupportedContract);
    request = setup.request();
    auto wrong_account = setup.services;
    wrong_account.account = run_account();
    rejects(setup.resolver.inspect(request, wrong_account), lx::FailureCode::InvalidConfig);
    auto candidate = take(setup.resolver.inspect(request, setup.services));
    rejects(setup.resolver.validate(std::move(candidate), request, wrong_account),
            lx::FailureCode::SourceChanged);
    auto wrong_policy = setup.services;
    wrong_policy.missing_intervals = lx::MissingIntervalPolicy::Skip;
    rejects(setup.resolver.inspect(request, wrong_policy), lx::FailureCode::UnsupportedContract);
}
void publications_and_cursors(const qr::DailyDataset &dataset) {
    const auto payload_bytes = dataset.account()->current(lx::ResourceKind::Source);
    Setup setup{dataset};
    auto snapshot = setup.prepare();
    require(dataset.account()->current(lx::ResourceKind::Source) == payload_bytes,
            "adoption charged dataset payload a second time");
    auto run = run_account();
    lx::PublicationAllocator allocator{run, 0};
    auto first = take(snapshot.cursor(allocator));
    auto second = take(snapshot.cursor(allocator));
    auto slot = take(lx::ResultSlot::create(run, {snapshot.output_contract()}, 0,
                                            snapshot.metadata().content_digest,
                                            snapshot.metadata().source_reference_digest));
    rejects(first.finish(), lx::FailureCode::SourceCoverage);
    auto activation = current(*first.descriptor());
    auto wrong_extent = activation;
    ++std::get<lx::Interval>(wrong_extent.extent).end_ns;
    rejects(first.next(wrong_extent), lx::FailureCode::UpdateProtocol);
    auto wrong_availability = activation;
    --wrong_availability.activation_available_at_ns;
    rejects(first.next(wrong_availability), lx::FailureCode::UpdateProtocol);
    auto premature = activation;
    --premature.decision_time_ns;
    rejects(first.next(premature), lx::FailureCode::FutureRead);
    require(first.descriptor()->key.sequence == 0, "rejected activation advanced the cursor");
    std::cout << "daily source key,interval_start_ns,interval_end_ns,available_at_ns\n";
    for (std::uint64_t key = 0; key < 5; ++key) {
        require(first.descriptor() != nullptr, "daily cursor ended before its fifth Bar");
        const auto descriptor = *first.descriptor();
        const auto context = current(descriptor);
        const auto &row = dataset.rows()[static_cast<std::size_t>(key)];
        require(descriptor.key == lx::UpdateKey{setup.config.clock, key} &&
                    descriptor.extent ==
                        lx::TemporalExtent{lx::Interval{row.bar.interval_start.unix_nanos(),
                                                        row.bar.interval_end.unix_nanos()}} &&
                    descriptor.available_at_ns == row.close_available_ns &&
                    std::get<lx::CompleteRecord>(descriptor.delivery).payload_index == key,
                "five-Bar key/extent/availability trace differs from the immutable dataset");
        auto event = take(first.next(context));
        require(std::holds_alternative<lx::PendingOutcome>(event),
                "Bar delivery was confused with finish or a gap");
        slot->commit(take(
            lx::validate_pending(std::get<lx::PendingOutcome>(std::move(event)), *slot, context)));
        auto value = take(slot->read<quant::data::Bar>(0, context));
        same_bar(value.value, row.bar);
        require(&value.value != &row.bar,
                "daily source exposed the dataset row instead of copying current output");
        require(value.publication().available_at_ns == row.close_available_ns,
                "publication became available too early");
        require(second.descriptor()->key.sequence == 0, "source cursors share mutable position");
        std::cout << key << ',' << row.bar.interval_start.unix_nanos() << ','
                  << row.bar.interval_end.unix_nanos() << ',' << row.close_available_ns << '\n';
    }
    require(first.descriptor() == nullptr, "cursor still advertises a sixth Bar");
    require(take(first.finish()).completed_scope == lx::SequenceScope{setup.config.clock, 0, 5},
            "finish lost exact completed scope");
    rejects(first.finish(), lx::FailureCode::AlreadyExecuted);
    for (std::uint64_t key = 0; key < 5; ++key) {
        require(second.descriptor()->key.sequence == key,
                "second cursor did not independently replay all keys");
        (void)take(second.next(current(*second.descriptor())));
    }
    require(take(second.finish()).completed_scope == snapshot.metadata().validated_scope,
            "independent cursor failed complete finish");
}
void selected_scope_and_lifetime(const qr::DailyDataset &dataset) {
    std::weak_ptr<const void> module;
    auto snapshot = [&]() {
        // The resolver, registry, registered source and this local dataset handle
        // all die before delivery. The snapshot then gives ownership to its cursor.
        auto local_dataset = dataset;
        Setup setup{local_dataset};
        module = setup.build.pin;
        auto all = setup.prepare();
        auto selected = setup.prepare(2, 5);
        require(all.metadata().content_digest == selected.metadata().content_digest &&
                    all.metadata().source_reference_digest ==
                        selected.metadata().source_reference_digest,
                "scope selection changed immutable snapshot identity");
        return selected;
    }();
    require(!module.expired(), "snapshot lost module lifetime pin after resolver teardown");
    auto run = run_account();
    lx::PublicationAllocator allocator{run, 0};
    std::optional<lx::SourceSnapshot> holder{std::move(snapshot)};
    auto slot = take(lx::ResultSlot::create(run, {holder->output_contract()}, 0,
                                            holder->metadata().content_digest,
                                            holder->metadata().source_reference_digest));
    std::optional<lx::SourceCursor> cursor{take(holder->cursor(allocator))};
    holder.reset();
    require(!module.expired(), "cursor lost module lifetime pin after snapshot teardown");
    for (std::uint64_t key = 2; key < 5; ++key) {
        const auto descriptor = *cursor->descriptor();
        require(descriptor.key.sequence == key &&
                    std::get<lx::CompleteRecord>(descriptor.delivery).payload_index == key - 2,
                "selected physical payload index replaced the original semantic key");
        const auto context = current(descriptor);
        auto event = take(cursor->next(context));
        slot->commit(take(
            lx::validate_pending(std::get<lx::PendingOutcome>(std::move(event)), *slot, context)));
        same_bar(take(slot->read<quant::data::Bar>(0, context)).value,
                 dataset.rows()[static_cast<std::size_t>(key)].bar);
    }
    require(take(cursor->finish()).completed_scope ==
                lx::SequenceScope{std::string{qr::clock_profile}, 2, 5},
            "selected finish widened the scope");
    cursor.reset();
    require(!module.expired(), "retained current publication lost its module pin");
    const auto &last = dataset.rows().back();
    const lx::UpdateContext retained{
        {std::string{qr::clock_profile}, 4},
        lx::Interval{last.bar.interval_start.unix_nanos(), last.bar.interval_end.unix_nanos()},
        last.close_available_ns,
        last.close_available_ns};
    same_bar(take(slot->read<quant::data::Bar>(0, retained)).value, last.bar);
    slot.reset();
    require(module.expired(), "source setup retained a module after its final owner was released");
}
void invalid_registration(const qr::DailyDataset &dataset) {
    auto registry = take(lx::Registry::create());
    ql::ComponentRegistration build{{lx::Sha256{std::byte{1}}, lx::Sha256{std::byte{2}},
                                     lx::Sha256{std::byte{3}}, "daily-invalid-checks"},
                                    {},
                                    lx::MissingIntervalPolicy::Strict};
    auto types = take(ql::register_components(registry, build));
    auto resolver = take(lx::SourceResolver::create(dataset.account(), 65536, 4));
    const auto config = configuration(dataset);
    for (int variant = 0; variant < 9; ++variant) {
        auto bad = config;
        auto bad_build = build;
        switch (variant) {
        case 0:
            bad.clock = "real-exchange-time";
            break;
        case 1:
            bad.currency = "EUR";
            break;
        case 2:
            bad.missing_intervals = lx::MissingIntervalPolicy::Skip;
            break;
        case 3:
            bad.provider_name.clear();
            break;
        case 4:
            bad.operation.semantic_version = 0;
            break;
        case 5:
            bad.limits.max_records = 4;
            break;
        case 6:
            bad.limits.metadata_bytes = 1;
            break;
        case 7:
            bad.limits.control_bytes = 1;
            break;
        case 8:
            bad_build.implementation.binary_digest = {};
            break;
        }
        rejects(qr::register_daily_source(registry, resolver, types.bar, dataset, bad, bad_build));
    }
    auto bad_type = types.bar;
    bad_type.policy.contract.semantic_version = 2;
    rejects(qr::register_daily_source(registry, resolver, bad_type, dataset, config, build));
    auto registered =
        take(qr::register_daily_source(registry, resolver, types.bar, dataset, config, build));
    require(registered.dataset().rows().size() == 5,
            "rejected configurations damaged a later valid registration");
}
} // namespace

void verify_daily_source(const quant::research::DailyDataset &dataset) {
    require(dataset.rows().size() == 5, "source checks require the five synthetic daily Bars");
    registration_and_requests(dataset);
    publications_and_cursors(dataset);
    selected_scope_and_lifetime(dataset);
    invalid_registration(dataset);
}
