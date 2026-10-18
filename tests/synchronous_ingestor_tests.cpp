#include "quant/ingestion/synchronous_ingestor.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace quant::data;
using namespace quant::ingestion;
namespace data = quant::data;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void require_throws(Function&& function, const std::string_view message) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}

Timestamp time(const std::int64_t nanos) {
    return Timestamp::from_unix_nanos(nanos);
}

TimeRange range(const std::int64_t start, const std::int64_t end) {
    return TimeRange{time(start), time(end)};
}

ProviderMappingSegment mapping(
    const std::int64_t start,
    const std::int64_t end,
    const std::string_view symbol,
    const std::optional<data::SourceId> source_id = std::nullopt,
    const data::VenueId venue_id = data::VenueId{7}) {
    return ProviderMappingSegment{
        InstrumentId{42},
        "test-provider",
        std::string{symbol},
        venue_id,
        std::nullopt,
        range(start, end),
        source_id,
    };
}

EventHeader header(
    const std::int64_t event_time,
    const std::uint64_t sequence = 1,
    const data::VenueId venue_id = data::VenueId{7},
    const data::SourceId source_id = data::SourceId{9}) {
    return EventHeader{
        InstrumentId{42},
        venue_id,
        time(event_time),
        time(event_time + 1),
        sequence,
        std::uint32_t{2},
        std::uint64_t{11},
        source_id,
    };
}

MboEvent add_event(
    const std::int64_t event_time,
    const std::uint64_t order_id,
    const std::uint64_t sequence = 1,
    const data::VenueId venue_id = data::VenueId{7},
    const data::SourceId source_id = data::SourceId{9}) {
    return MboEvent{
        header(event_time, sequence, venue_id, source_id),
        MboAction::Add,
        OrderId{order_id},
        Side::Buy,
        Price::from_integer(100),
        Quantity::from_integer(2),
    };
}

std::optional<MboStreamContext> scope(
    const data::VenueId venue_id = data::VenueId{7},
    const data::SourceId source_id = data::SourceId{9}) {
    return MboStreamContext{InstrumentId{42}, venue_id, source_id};
}

class RecordingRecoveryPolicy final : public RecoveryPolicy {
public:
    [[nodiscard]] RecoveryPlan decide(const RecoveryContext& context) const override {
        called = true;
        observed_query = context.query;
        observed_state = context.data_state;
        observed_segments.assign(context.state_segments.begin(), context.state_segments.end());
        observed_unresolved.assign(context.unresolved_ranges.begin(), context.unresolved_ranges.end());
        return RecoveryPlan{RecoveryAction::NoAction, {}};
    }

    mutable bool called = false;
    mutable std::optional<MarketDataQuery> observed_query;
    mutable DataState observed_state = DataState::Unknown;
    mutable std::vector<DataStateSegment> observed_segments;
    mutable std::vector<DataStateSegment> observed_unresolved;
};

class CompleteOnlyUsabilityPolicy final : public DataUsabilityPolicy {
public:
    [[nodiscard]] bool is_usable(const MarketDataQuery&, const DataStateSegment& segment) const override {
        return segment.state == DataState::Complete;
    }
};

void test_full_miss_produces_result() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "TEST"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {add_event(3, 100)},
        {},
        {SourceId{9}},
        {},
        {},
        {},
        {},
        {},
    });
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)},
        time(1000),
        "adapter-v1",
        "schema-v1",
        "mapping-v1");

    require(provider.calls().size() == 1, "a full cache miss must produce one provider request");
    require(provider.calls()[0].range == range(0, 10), "provider request must use the canonical missing range");
    require(result.mbo_buffers.size() == 1 && result.mbo_buffers[0].size() == 1,
            "valid provider output must enter one scoped MBO buffer");
    require(result.metadata.actual_coverage.empty(),
            "valid provider output without coverage evidence must not claim actual coverage");
    require(result.metadata.unresolved_ranges ==
                std::vector<DataStateSegment>{DataStateSegment{range(0, 10), DataState::Unknown, scope()}},
            "evidence-free unknown output must remain unresolved");
    require(result.metadata.data_state == DataState::Unknown,
            "valid provider output without quality evidence must reduce to unknown state");
    require(result.metadata.provider == "test-provider", "result metadata must retain provider identity");
    require(result.metadata.source_ids == std::vector<SourceId>{SourceId{9}},
            "result metadata must retain source identity");
    require(result.metadata.adapter_version == "adapter-v1", "result must retain adapter version identity");
    require(result.metadata.canonical_schema_version == "schema-v1",
            "result must retain canonical schema version identity");
    require(result.metadata.mapping_version == "mapping-v1", "result must retain mapping version identity");
    require(result.metadata.acquisition_mode == AcquisitionMode::Historical,
            "synchronous historical results must identify their acquisition mode");
}

void test_event_mapping_and_source_receive_filter() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "TEST"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {add_event(2, 100), add_event(5, 101)},
        {},
        {SourceId{9}},
        {},
        {},
        {},
        {},
        {},
    });
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const MarketDataQuery query{
        InstrumentId{42},
        VenueId{7},
        MarketDataLevel::L3,
        std::optional<TimeRange>{range(0, 10)},
        std::optional<TimeRange>{range(0, 4)},
        TimeBasis::SourceReceiveTime,
        FetchPolicy::RequireExplicitRange,
    };
    const auto result = ingestor.ingest(query, time(1000));
    require(result.mbo_buffers.size() == 1 && result.mbo_buffers[0].size() == 1,
            "source receive-time constraints must filter records after event-time mapping");
    require(result.mbo_buffers[0].at(0).order_id()->value() == 100,
            "the record inside both event and source receive ranges must survive");

    require_throws(
        [&] {
            (void)ingestor.ingest(
                MarketDataQuery{
                    InstrumentId{42},
                    VenueId{7},
                    MarketDataLevel::L3,
                    std::nullopt,
                    std::optional<TimeRange>{range(0, 10)},
                    TimeBasis::SourceReceiveTime,
                    FetchPolicy::RequireExplicitRange,
                },
                time(1001));
        },
        "receive-only ingestion must be rejected without event-time mapping validity");
}

void test_out_of_scope_records_are_ignored_without_corruption() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "TEST"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {add_event(2, 100), add_event(20, 101)},
        {},
        {SourceId{9}},
        {},
        {},
        {},
        {},
        {},
    });
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(1002));
    require(result.mbo_buffers.size() == 1 && result.mbo_buffers[0].size() == 1,
            "records outside a mapping segment must not enter the result");
    require(result.metadata.data_state != DataState::Corrupt,
            "valid records outside a requested segment must not create corruption evidence");
}

void test_trade_and_execution_observations() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "TEST"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {},
        {DataQualityObservation{range(0, 10), DataQualityKind::Complete}},
        {SourceId{9}},
        {},
        {Trade{header(2), Price::from_integer(100), Quantity::from_integer(4), Side::Buy}},
        {OrderExecution{header(3), OrderId{100}, Quantity::from_integer(2), Price::from_integer(100)}},
        {},
        {},
    });
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(1500));

    require(result.mbo_buffers.empty(), "trade and execution observations must not create book mutations");
    require(result.trades.size() == 1 && result.trades[0].quantity == Quantity::from_integer(4),
            "provider trades must survive the synchronous ingestion result");
    require(
        result.order_executions.size() == 1 &&
            result.order_executions[0].resting_order_id == OrderId{100} &&
            result.order_executions[0].executed_quantity == Quantity::from_integer(2),
        "provider order executions must survive the synchronous ingestion result");
    require(result.metadata.data_state == DataState::Complete,
            "complete quality evidence must cover non-book observations");
}

void test_mapping_boundary_is_one_logical_result() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "OLD"));
    mappings.add(mapping(10, 20, "NEW"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{{add_event(9, 100, 1)}, {}, {SourceId{9}}, {}, {}, {}, {}, {}});
    provider.enqueue_response(ProviderBatch{{add_event(10, 101, 2)}, {}, {SourceId{9}}, {}, {}, {}, {}, {}});
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(5, 15)},
        time(2000));

    require(provider.calls().size() == 2, "a mapping boundary must produce two provider calls");
    require(provider.calls()[0].range == range(5, 10), "first provider call must use the first half-open segment");
    require(provider.calls()[1].range == range(10, 15), "second provider call must use the second half-open segment");
    require(result.mbo_buffers.size() == 1 && result.mbo_buffers[0].size() == 2,
            "mapping segments must share one logical scoped MBO result");
    require(result.mbo_buffers[0].at(0).order_id()->value() == 100, "first provider event must remain first");
    require(result.mbo_buffers[0].at(1).order_id()->value() == 101, "second provider event must remain second");
    require(result.mbo_buffers[0].at(0).header().instrument_id == InstrumentId{42},
            "normalized events must retain canonical instrument identity");
    require(result.metadata.data_state == DataState::Unknown,
            "mapping segments with events but no quality evidence must reduce to unknown state");
    require(result.metadata.unresolved_ranges ==
                std::vector<DataStateSegment>{DataStateSegment{range(5, 15), DataState::Unknown, scope()}},
            "mapping segments without evidence must remain unresolved as one merged state range");
}

void test_provider_quality_and_validation() {
    {
        NoopCache cache;
        InMemoryInstrumentMappingRegistry mappings;
        mappings.add(mapping(0, 10, "TEST"));
        FakeProvider provider{"test-provider"};
        provider.enqueue_response(ProviderBatch{
            {add_event(2, 100)},
            {DataQualityObservation{range(0, 10), DataQualityKind::Degraded}},
            {SourceId{9}},
            {},
            {},
            {},
            {},
            {},
        });
        NoopRecoveryPolicy recovery;
        DefaultDataUsabilityPolicy usability;
        SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
        const auto result = ingestor.ingest(
            MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(3));
        require(result.metadata.data_state == DataState::Degraded,
                "degraded provider evidence must reduce to degraded state");
        require(result.metadata.actual_coverage ==
                    std::vector<DataStateSegment>{DataStateSegment{range(0, 10), DataState::Degraded, scope()}},
                "the default usability policy must treat degraded data as usable coverage");
        require(result.metadata.unresolved_ranges.empty(),
                "the default usability policy must not mark degraded data unresolved");
    }

    {
        NoopCache cache;
        InMemoryInstrumentMappingRegistry mappings;
        mappings.add(mapping(0, 10, "TEST"));
        FakeProvider provider{"test-provider"};
        provider.enqueue_response(ProviderBatch{{}, {}, {}, {}, {}, {}, {}, {}});
        NoopRecoveryPolicy recovery;
        DefaultDataUsabilityPolicy usability;
        SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
        const auto result = ingestor.ingest(
            MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(3));
        require(result.metadata.data_state == DataState::Unknown,
                "zero provider events without quality evidence must reduce to unknown state");
        require(result.metadata.unresolved_ranges ==
                    std::vector<DataStateSegment>{DataStateSegment{range(0, 10), DataState::Unknown}},
                "zero provider events without quality evidence must remain unresolved");
        require(result.mbo_buffers.empty(), "zero provider events must not create an MBO buffer");
    }

    {
        NoopCache cache;
        InMemoryInstrumentMappingRegistry mappings;
        mappings.add(mapping(0, 10, "TEST"));
        FakeProvider provider{"test-provider"};
        provider.enqueue_response(ProviderBatch{
            {},
            {DataQualityObservation{range(0, 10), DataQualityKind::Complete}},
            {},
            {},
            {},
            {},
            {},
            {},
        });
        NoopRecoveryPolicy recovery;
        DefaultDataUsabilityPolicy usability;
        SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
        const auto result = ingestor.ingest(
            MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(3));
        require(result.metadata.data_state == DataState::Complete,
                "explicit complete evidence must reduce to complete state without events");
        require(result.metadata.actual_coverage ==
                    std::vector<DataStateSegment>{DataStateSegment{range(0, 10), DataState::Complete}},
                "explicit complete evidence must produce actual coverage without events");
        require(result.metadata.unresolved_ranges.empty(), "explicit complete evidence must be resolved");
        require(result.mbo_buffers.empty(), "explicit complete evidence without events must not create an MBO buffer");
    }

    {
        NoopCache cache;
        InMemoryInstrumentMappingRegistry mappings;
        mappings.add(mapping(0, 10, "TEST"));
        FakeProvider provider{"test-provider"};
        provider.enqueue_response(ProviderBatch{
            {},
            {DataQualityObservation{range(0, 10), DataQualityKind::Missing}},
            {},
            {},
            {},
            {},
            {},
            {},
        });
        NoopRecoveryPolicy recovery;
        DefaultDataUsabilityPolicy usability;
        SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
        const auto result = ingestor.ingest(
            MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(4));
        require(result.metadata.data_state == DataState::Missing,
                "missing provider evidence must reduce to missing state");
        require(result.mbo_buffers.empty(), "missing provider data must not create an MBO buffer");
    }

    {
        NoopCache cache;
        InMemoryInstrumentMappingRegistry mappings;
        mappings.add(mapping(0, 10, "TEST"));
        FakeProvider provider{"test-provider"};
        provider.enqueue_response(ProviderBatch{
            {MboEvent{header(2), MboAction::Add, std::nullopt, std::nullopt, std::nullopt, std::nullopt}},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        });
        NoopRecoveryPolicy recovery;
        DefaultDataUsabilityPolicy usability;
        SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
        const auto result = ingestor.ingest(
            MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(5));
        require(result.metadata.data_state == DataState::Corrupt,
                "validation failure must become corrupt quality evidence");
        require(result.mbo_buffers.empty(), "invalid MBO events must not enter the valid buffer");
        require(result.metadata.unresolved_ranges ==
                    std::vector<DataStateSegment>{DataStateSegment{range(0, 10), DataState::Corrupt, scope()}},
                "validation failure must leave its segment unresolved");
    }
}

void test_injected_usability_policy_does_not_change_data_state() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "TEST"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {},
        {DataQualityObservation{range(0, 10), DataQualityKind::Degraded}},
        {SourceId{9}},
        {},
        {},
        {},
        {},
        {},
    });
    NoopRecoveryPolicy recovery;
    CompleteOnlyUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(8));
    require(result.metadata.data_state == DataState::Degraded,
            "a usability policy must not change the factual data state");
    require(result.metadata.actual_coverage.empty(),
            "an injected complete-only policy must reject degraded coverage");
    require(result.metadata.unresolved_ranges ==
                std::vector<DataStateSegment>{DataStateSegment{range(0, 10), DataState::Degraded, scope()}},
            "an injected usability policy must control unresolved consumer ranges");
}

void test_multiple_stream_scopes() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "VENUE-1", SourceId{9}, VenueId{7}));
    mappings.add(mapping(0, 10, "VENUE-2", SourceId{10}, VenueId{8}));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {add_event(2, 100, 1, VenueId{7}, SourceId{9}), add_event(4, 101, 2, VenueId{7}, SourceId{9})},
        {DataQualityObservation{range(0, 10), DataQualityKind::Complete}},
        {SourceId{9}},
        {},
        {},
        {},
        {},
        {},
    });
    provider.enqueue_response(ProviderBatch{
        {add_event(3, 200, 1, VenueId{8}, SourceId{10}), add_event(5, 201, 2, VenueId{8}, SourceId{10})},
        {DataQualityObservation{range(0, 10), DataQualityKind::Complete}},
        {SourceId{10}},
        {},
        {},
        {},
        {},
        {},
    });
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, std::nullopt, MarketDataLevel::L3, range(0, 10)}, time(6));

    require(result.mbo_buffers.size() == 2, "one logical result must preserve one MBO buffer per stream scope");
    require(result.mbo_buffers[0].context().instrument_id == InstrumentId{42} &&
                result.mbo_buffers[0].context().venue_id == VenueId{7} &&
                result.mbo_buffers[0].context().source_id == SourceId{9},
            "first stream buffer must retain its venue/source context");
    require(result.mbo_buffers[1].context().instrument_id == InstrumentId{42} &&
                result.mbo_buffers[1].context().venue_id == VenueId{8} &&
                result.mbo_buffers[1].context().source_id == SourceId{10},
            "second stream buffer must retain its venue/source context");
    require(result.mbo_buffers[0].at(0).order_id()->value() == 100 &&
                result.mbo_buffers[0].at(1).order_id()->value() == 101,
            "provider order must be preserved within the first stream");
    require(result.mbo_buffers[1].at(0).order_id()->value() == 200 &&
                result.mbo_buffers[1].at(1).order_id()->value() == 201,
            "provider order must be preserved within the second stream");
    require(result.metadata.data_state == DataState::Complete,
            "complete evidence for all stream scopes must derive a complete summary");
    require(result.metadata.unresolved_ranges.empty(), "complete stream evidence must leave no unresolved ranges");
    require(result.metadata.data_state_segments.size() == 2,
            "overlapping stream scopes must retain independent state segments");
}

void test_artifact_provenance_and_recovery_context() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "TEST", SourceId{9}));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{
        {add_event(2, 100)},
        {},
        {SourceId{9}},
        {
            SourceArtifactProvenance{"artifact-a", range(0, 4)},
            SourceArtifactProvenance{"artifact-b", range(4, 10)},
        },
        {},
        {},
        {},
        {},
    });
    RecordingRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto result = ingestor.ingest(
        MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(0, 10)}, time(7));

    require(result.metadata.source_artifacts == std::vector<SourceArtifactProvenance>{
                                                      SourceArtifactProvenance{"artifact-a", range(0, 4)},
                                                      SourceArtifactProvenance{"artifact-b", range(4, 10)},
                                                  },
            "result metadata must retain plural range-aware artifact provenance");
    require(recovery.called, "ingestion must consult the injected recovery policy");
    require(recovery.observed_query && recovery.observed_query->primary_range() == range(0, 10),
            "recovery context must retain the requested query");
    require(recovery.observed_state == DataState::Unknown,
            "recovery context must retain the derived data state");
    require(recovery.observed_segments == result.metadata.data_state_segments,
            "recovery context must expose segmented state");
    require(recovery.observed_unresolved == result.metadata.unresolved_ranges,
            "recovery context must expose scoped unresolved ranges");
}

void test_explicit_unsupported_levels() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    FakeProvider provider{"test-provider"};
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    require_throws(
        [&] {
            (void)ingestor.ingest(
                MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L1, range(0, 10)}, time(1));
        },
        "L1 queries must fail explicitly");
    require_throws(
        [&] {
            (void)ingestor.ingest(
                MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L2, range(0, 10)}, time(1));
        },
        "L2 queries must fail explicitly");
    require(provider.calls().empty(), "unsupported levels must not reach the provider");
}

} // namespace

int main() {
    try {
        test_full_miss_produces_result();
        test_event_mapping_and_source_receive_filter();
        test_out_of_scope_records_are_ignored_without_corruption();
        test_trade_and_execution_observations();
        test_mapping_boundary_is_one_logical_result();
        test_provider_quality_and_validation();
        test_injected_usability_policy_does_not_change_data_state();
        test_multiple_stream_scopes();
        test_artifact_provenance_and_recovery_context();
        test_explicit_unsupported_levels();
        std::cout << "synchronous_ingestor_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "synchronous_ingestor_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
