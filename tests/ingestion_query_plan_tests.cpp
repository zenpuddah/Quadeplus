#include "quant/ingestion/query_plan.hpp"
#include "quant/ingestion/synchronous_ingestor.hpp"

#include <cstddef>
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
    const std::string_view symbol) {
    return ProviderMappingSegment{
        InstrumentId{42},
        "test-provider",
        std::string{symbol},
        VenueId{7},
        std::nullopt,
        range(start, end),
        SourceId{9},
    };
}

MarketDataQuery query(const TimeRange event_range) {
    return MarketDataQuery{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, event_range};
}

void test_one_segment_plan() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 20, "TEST"));
    QueryPlanner planner{cache, mappings, "test-provider"};

    const auto result = planner.plan(query(range(10, 20)));
    require(result.missing_ranges == std::vector<TimeRange>{range(10, 20)},
            "a full cache miss must remain visible in the query plan");
    require(result.unmapped_ranges.empty(), "a covered mapping must not create unmapped ranges");
    require(result.segments.size() == 1, "one mapping interval must produce one plan segment");
    require(result.segments[0].mapping.range == range(10, 20),
            "the plan must retain the resolved event-time mapping range");
    require(result.segments[0].provider_query.event_time_range == std::optional<TimeRange>{range(10, 20)},
            "provider execution must receive the mapping-clipped event range");
    require(result.segments[0].provider_fetch_range ==
                ProviderFetchRange{TimeBasis::SourceReceiveTime, range(10, 20)},
            "event-only planning must derive the provider fetch range");
    require(result.segments[0].post_fetch_filters == PostFetchFilters{range(10, 20), std::nullopt},
            "the plan must retain the event-time post-fetch filter");
}

void test_mapping_boundary_plan() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "OLD"));
    mappings.add(mapping(10, 20, "NEW"));
    QueryPlanner planner{cache, mappings, "test-provider"};

    const auto result = planner.plan(query(range(5, 15)));
    require(result.segments.size() == 2, "a mapping boundary must produce two plan segments");
    require(result.segments[0].mapping.range == range(5, 10), "the first plan segment must be half-open at the boundary");
    require(result.segments[1].mapping.range == range(10, 15), "the second plan segment must start at the boundary");
    require(result.segments[0].provider_query.event_time_range == std::optional<TimeRange>{range(5, 10)},
            "the first provider query must be clipped to its mapping segment");
    require(result.segments[1].provider_query.event_time_range == std::optional<TimeRange>{range(10, 15)},
            "the second provider query must be clipped to its mapping segment");
}

void test_dual_ranges_and_provider_derivation() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 40, "TEST"));
    QueryPlanner planner{cache, mappings, "test-provider"};

    const MarketDataQuery dual_range_query{
        InstrumentId{42},
        VenueId{7},
        MarketDataLevel::L3,
        std::optional<TimeRange>{range(10, 30)},
        std::optional<TimeRange>{range(12, 32)},
        TimeBasis::EventTime,
        FetchPolicy::RequireExplicitRange,
    };
    const auto explicit_plan = planner.plan(dual_range_query);
    require(explicit_plan.segments.size() == 1, "dual ranges must still produce one mapping segment");
    require(explicit_plan.segments[0].provider_fetch_range ==
                ProviderFetchRange{TimeBasis::SourceReceiveTime, range(12, 32)},
            "an explicit source receive range must be the provider fetch range");
    require(explicit_plan.segments[0].post_fetch_filters == PostFetchFilters{range(10, 30), range(12, 32)},
            "dual ranges must remain post-fetch filters");

    const MarketDataQuery derived_query{
        InstrumentId{42},
        VenueId{7},
        MarketDataLevel::L3,
        std::optional<TimeRange>{range(10, 20)},
        std::nullopt,
        TimeBasis::EventTime,
        FetchPolicy::DeriveProviderRange,
        TimeMargin{2, 3},
    };
    const auto derived_plan = planner.plan(derived_query);
    require(derived_plan.segments[0].provider_fetch_range ==
                ProviderFetchRange{TimeBasis::SourceReceiveTime, range(8, 23)},
            "event-only planning must apply the configured provider-range margin");
}

void test_receive_only_and_strict_event_queries_are_rejected() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 20, "TEST"));
    QueryPlanner planner{cache, mappings, "test-provider"};

    require_throws(
        [&] {
            (void)planner.plan(MarketDataQuery{
                InstrumentId{42},
                VenueId{7},
                MarketDataLevel::L3,
                std::nullopt,
                std::optional<TimeRange>{range(10, 20)},
            });
        },
        "receive-only queries must be rejected because mappings require event time");
    require_throws(
        [&] {
            (void)planner.plan(MarketDataQuery{
                InstrumentId{42},
                VenueId{7},
                MarketDataLevel::L3,
                std::optional<TimeRange>{range(10, 20)},
                std::nullopt,
                TimeBasis::EventTime,
                FetchPolicy::RequireExplicitRange,
            });
        },
        "strict event-only queries must be rejected before provider execution");
}

void test_execution_uses_the_explained_plan() {
    NoopCache cache;
    InMemoryInstrumentMappingRegistry mappings;
    mappings.add(mapping(0, 10, "OLD"));
    mappings.add(mapping(10, 20, "NEW"));
    FakeProvider provider{"test-provider"};
    provider.enqueue_response(ProviderBatch{});
    provider.enqueue_response(ProviderBatch{});
    NoopRecoveryPolicy recovery;
    DefaultDataUsabilityPolicy usability;
    SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};

    const auto requested_query = query(range(5, 15));
    const auto plan = ingestor.explain(requested_query);
    require(provider.calls().empty(), "explain must not execute provider calls");
    (void)ingestor.ingest(requested_query, time(100));

    require(provider.calls().size() == plan.segments.size(),
            "execution must make exactly the calls represented by the query plan");
    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        require(provider.calls()[index].range == plan.segments[index].mapping.range,
                "provider mapping calls must follow the explained segment order");
        require(provider.queries()[index].event_time_range == plan.segments[index].provider_query.event_time_range,
                "provider queries must use the explained mapping-clipped event ranges");
    }
}

} // namespace

int main() {
    try {
        test_one_segment_plan();
        test_mapping_boundary_plan();
        test_dual_ranges_and_provider_derivation();
        test_receive_only_and_strict_event_queries_are_rejected();
        test_execution_uses_the_explained_plan();
        std::cout << "ingestion_query_plan_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "ingestion_query_plan_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
