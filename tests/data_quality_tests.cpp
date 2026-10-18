#include "quant/ingestion/quality.hpp"

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

DataState reduce(
    const TimeRange requested,
    const std::vector<DataQualityObservation>& observations) {
    return summarize_data_state(DataStateReducer::reduce(requested, observations));
}

void test_quality_reduction() {
    const auto requested = range(10, 20);
    require(
        reduce(requested, {{requested, DataQualityKind::Complete}}) == DataState::Complete,
        "complete evidence must reduce to complete state");
    require(
        reduce(requested, {{requested, DataQualityKind::Degraded}}) == DataState::Degraded,
        "degraded evidence must reduce to degraded state");
    require(
        reduce(requested, {{requested, DataQualityKind::Missing}}) == DataState::Missing,
        "missing evidence must reduce to missing state");
    require(
        reduce(requested, {{requested, DataQualityKind::SequenceGap}}) == DataState::Degraded,
        "sequence-gap evidence must reduce to degraded state");
    require(
        reduce(requested, {{requested, DataQualityKind::Pending}}) == DataState::Unknown,
        "pending provider evidence must reduce to unknown state");
    require(
        reduce(requested, {{requested, DataQualityKind::Complete}, {range(15, 18), DataQualityKind::Corrupt}}) ==
            DataState::Corrupt,
        "corrupt evidence must take precedence over overlapping complete evidence");
}

void test_unknown_and_coverage() {
    const auto requested = range(10, 20);
    require(
        reduce(requested, {}) == DataState::Unknown,
        "no evidence must reduce to an explicit unknown state");
    const auto partial = DataStateReducer::reduce(requested, {{range(10, 15), DataQualityKind::Complete}});
    require(
        summarize_data_state(partial) == DataState::Unknown,
        "partial evidence must not pretend the uncovered range is complete");
    require(
        partial == std::vector<DataStateSegment>{
            DataStateSegment{range(10, 15), DataState::Complete},
            DataStateSegment{range(15, 20), DataState::Unknown},
        },
        "partial evidence must preserve the complete and unknown intervals");

    const auto adjacent = DataStateReducer::reduce(
        requested,
        {{range(10, 15), DataQualityKind::Complete}, {range(15, 20), DataQualityKind::Missing}});
    require(
        summarize_data_state(adjacent) == DataState::Missing,
        "adjacent quality evidence must cover the requested range honestly");
    require(
        adjacent == std::vector<DataStateSegment>{
            DataStateSegment{range(10, 15), DataState::Complete},
            DataStateSegment{range(15, 20), DataState::Missing},
        },
        "adjacent quality evidence must remain range-local");

    require(
        reduce(requested, {{range(0, 10), DataQualityKind::Complete}, {range(20, 30), DataQualityKind::Complete}}) ==
            DataState::Unknown,
        "endpoint-adjacent evidence must not cover a half-open request");
}

void test_scoped_segments_and_lookup() {
    const auto requested = range(10, 20);
    const auto first_scope = std::optional<MboStreamContext>{MboStreamContext{InstrumentId{42}, VenueId{1}, SourceId{9}}};
    const auto second_scope = std::optional<MboStreamContext>{MboStreamContext{InstrumentId{42}, VenueId{2}, SourceId{9}}};
    const std::vector<std::optional<MboStreamContext>> expected_scopes{first_scope, second_scope};
    const auto segments = DataStateReducer::reduce(
        requested,
        {
            DataQualityObservation{requested, DataQualityKind::Complete, first_scope},
            DataQualityObservation{range(10, 15), DataQualityKind::Complete, second_scope},
        },
        expected_scopes);

    require(segments.size() == 3, "overlapping stream scopes must remain separate state segments");
    const auto* first_state = lookup_data_state(segments, first_scope, time(17));
    require(first_state && first_state->state == DataState::Complete, "point lookup must select the first stream state");
    const auto* second_state = lookup_data_state(segments, second_scope, time(17));
    require(second_state && second_state->state == DataState::Unknown, "point lookup must retain the second stream gap");

    const auto second_range = lookup_data_state(segments, second_scope, range(12, 18));
    require(
        second_range == std::vector<DataStateSegment>{
            DataStateSegment{range(12, 15), DataState::Complete, second_scope},
            DataStateSegment{range(15, 18), DataState::Unknown, second_scope},
        },
        "range lookup must clip and preserve scoped state segments");

    const auto merged = DataStateReducer::reduce(
        requested,
        {{range(10, 15), DataQualityKind::Complete}, {range(15, 20), DataQualityKind::Complete}});
    require(merged.size() == 1 && merged[0].range == requested && merged[0].state == DataState::Complete,
            "adjacent equivalent segments must merge");
}

class CompleteOnlyUsabilityPolicy final : public DataUsabilityPolicy {
public:
    [[nodiscard]] bool is_usable(const MarketDataQuery&, const DataStateSegment& segment) const override {
        return segment.state == DataState::Complete;
    }
};

void test_usability_policy_is_consumer_choice() {
    const MarketDataQuery query{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(10, 20)};
    const DataStateSegment complete{range(10, 15), DataState::Complete};
    const DataStateSegment degraded{range(15, 20), DataState::Degraded};
    const DataStateSegment unknown{range(10, 20), DataState::Unknown};
    const DataStateSegment missing{range(10, 20), DataState::Missing};
    const DataStateSegment corrupt{range(10, 20), DataState::Corrupt};

    DefaultDataUsabilityPolicy default_policy;
    require(default_policy.is_usable(query, complete), "complete data must be usable by the default policy");
    require(default_policy.is_usable(query, degraded), "degraded data must be usable by the default policy");
    require(!default_policy.is_usable(query, unknown), "unknown data must be unusable by the default policy");
    require(!default_policy.is_usable(query, missing), "missing data must be unusable by the default policy");
    require(!default_policy.is_usable(query, corrupt), "corrupt data must be unusable by the default policy");

    CompleteOnlyUsabilityPolicy conservative_policy;
    require(!conservative_policy.is_usable(query, degraded), "consumer policy must be able to reject degraded data");
    require(degraded.state == DataState::Degraded, "usability decisions must not change factual data state");
}

} // namespace

int main() {
    try {
        test_quality_reduction();
        test_unknown_and_coverage();
        test_scoped_segments_and_lookup();
        test_usability_policy_is_consumer_choice();
        std::cout << "data_quality_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "data_quality_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
