#include "quant/ingestion/result.hpp"

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

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Timestamp time(const std::int64_t nanos) {
    return Timestamp::from_unix_nanos(nanos);
}

TimeRange range(const std::int64_t start, const std::int64_t end) {
    return TimeRange{time(start), time(end)};
}

EventHeader header() {
    return EventHeader{
        InstrumentId{42},
        VenueId{7},
        time(10),
        time(11),
        std::uint64_t{3},
        std::uint32_t{1},
        std::uint64_t{9},
        SourceId{5},
    };
}

void test_result_keeps_intent_and_outcome_distinct() {
    const MarketDataQuery query{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(10, 20)};
    MboBuffer buffer{MboStreamContext{InstrumentId{42}, VenueId{7}, SourceId{5}}};
    buffer.append(MboAdd{header(), OrderId{100}, Side::Buy, Price::from_integer(100), Quantity::from_integer(2)});

    IngestionMetadata metadata{
        query,
        {DataStateSegment{range(10, 15), DataState::Complete}},
        {DataStateSegment{range(15, 20), DataState::Missing}},
        {
            DataQualityObservation{range(10, 15), DataQualityKind::Complete},
            DataQualityObservation{range(15, 20), DataQualityKind::Missing},
        },
        {
            DataStateSegment{range(10, 15), DataState::Complete},
            DataStateSegment{range(15, 20), DataState::Missing},
        },
        DataState::Missing,
        "fake-provider",
        {SourceId{5}},
        {},
        time(1000),
        "fake-adapter-v1",
        "canonical-mbo-v1",
        "mapping-config-v1",
        {},
        {},
    };
    std::vector<MboBuffer> buffers;
    buffers.push_back(std::move(buffer));
    const IngestionResult result{std::move(buffers), metadata, {}, {}};

    require(
        result.metadata.query.primary_range() == range(10, 20),
        "metadata must retain the requested primary range");
    require(result.metadata.actual_coverage ==
                std::vector<DataStateSegment>{DataStateSegment{range(10, 15), DataState::Complete}},
            "metadata must retain actual coverage separately from the request");
    require(result.metadata.unresolved_ranges ==
                std::vector<DataStateSegment>{DataStateSegment{range(15, 20), DataState::Missing}},
            "metadata must retain unresolved ranges for partial results");
    require(result.metadata.data_state_segments.size() == 2,
            "metadata must retain range-local state segments");
    require(result.metadata.data_state == DataState::Missing, "metadata must retain reduced data state");
    require(result.metadata.acquisition_mode == AcquisitionMode::Historical,
            "ingestion metadata must identify historical acquisition");
    require(result.mbo_buffers.size() == 1, "result must expose the canonical MBO buffer when data is valid");
    require(result.mbo_buffers[0].size() == 1, "result MBO output must reuse the existing buffer representation");
    require(result.mbo_buffers[0].at(0).header().instrument_id == InstrumentId{42},
            "result MBO output must retain canonical identity");
}

void test_metadata_versions_and_optional_artifact() {
    const MarketDataQuery query{InstrumentId{42}, std::nullopt, MarketDataLevel::L3, range(0, 1)};
    IngestionMetadata metadata{
        query,
        {},
        {DataStateSegment{range(0, 1), DataState::Missing}},
        {DataQualityObservation{range(0, 1), DataQualityKind::Missing}},
        {DataStateSegment{range(0, 1), DataState::Missing}},
        DataState::Missing,
        "provider",
        {},
        {SourceArtifactProvenance{"artifact-checksum", range(0, 1)}},
        time(2),
        "adapter-2",
        "schema-3",
        "mapping-4",
        {},
        {},
    };

    require(metadata.source_artifacts ==
                std::vector<SourceArtifactProvenance>{{"artifact-checksum", range(0, 1)}},
            "source artifact provenance must retain identity and range metadata");
    require(metadata.adapter_version == "adapter-2", "adapter version must round-trip");
    require(metadata.canonical_schema_version == "schema-3", "canonical schema version must round-trip");
    require(metadata.mapping_version == "mapping-4", "mapping version must round-trip");
    require(metadata.ingestion_time == time(2), "ingestion timestamp must round-trip");
    metadata.acquisition_mode = AcquisitionMode::Live;
    require(metadata.acquisition_mode == AcquisitionMode::Live,
            "ingestion metadata must support explicit live acquisition provenance");
}

} // namespace

int main() {
    try {
        test_result_keeps_intent_and_outcome_distinct();
        test_metadata_versions_and_optional_artifact();
        std::cout << "ingestion_result_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "ingestion_result_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
