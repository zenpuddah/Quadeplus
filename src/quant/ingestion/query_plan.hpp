#pragma once

#include "quant/ingestion/cache.hpp"
#include "quant/ingestion/instrument_mapping.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quant::ingestion {

struct ProviderFetchRange {
    TimeBasis basis;
    TimeRange range;

    friend bool operator==(const ProviderFetchRange&, const ProviderFetchRange&) = default;
};

struct PostFetchFilters {
    std::optional<TimeRange> event_time_range;
    std::optional<TimeRange> source_receive_time_range;

    friend bool operator==(const PostFetchFilters&, const PostFetchFilters&) = default;
};

struct QueryPlanSegment {
    ProviderMappingSegment mapping;
    MarketDataQuery provider_query;
    ProviderFetchRange provider_fetch_range;
    PostFetchFilters post_fetch_filters;
};

struct QueryPlan {
    MarketDataQuery original_query;
    std::vector<TimeRange> missing_ranges;
    std::vector<TimeRange> unmapped_ranges;
    std::vector<QueryPlanSegment> segments;
};

[[nodiscard]] inline ProviderFetchRange resolve_provider_fetch_range(const MarketDataQuery& query) {
    validate(query);
    if (!query.event_time_range) {
        throw std::invalid_argument("query planning requires an event-time range for mapping resolution");
    }
    if (query.source_receive_time_range) {
        return ProviderFetchRange{TimeBasis::SourceReceiveTime, *query.source_receive_time_range};
    }
    if (query.fetch_policy == FetchPolicy::RequireExplicitRange) {
        throw std::invalid_argument(
            "provider fetch planning requires an explicit source receive-time range under strict fetch policy");
    }
    return ProviderFetchRange{
        TimeBasis::SourceReceiveTime,
        expand(*query.event_time_range, query.derivation_margin.value_or(TimeMargin{0, 0}))};
}

class QueryPlanner final {
public:
    QueryPlanner(
        CachePort& cache,
        InstrumentMappingRegistry& mappings,
        std::string provider_name)
        : cache_(cache), mappings_(mappings), provider_name_(std::move(provider_name)) {
        if (provider_name_.empty()) {
            throw std::invalid_argument("query planner provider name must not be empty");
        }
    }

    [[nodiscard]] QueryPlan plan(const MarketDataQuery& query) {
        validate(query);
        if (!query.event_time_range) {
            throw std::invalid_argument("query planning requires an event-time range for mapping resolution");
        }

        const auto cache_lookup = cache_.lookup(query);
        if (cache_lookup.status != CacheStatus::Miss || !cache_lookup.covered.empty() ||
            cache_lookup.missing != std::vector<TimeRange>{*query.event_time_range}) {
            throw std::logic_error("query planning currently requires a full NoopCache miss");
        }

        QueryPlan result{query, cache_lookup.missing, {}, {}};
        for (const auto& missing_range : result.missing_ranges) {
            validate(missing_range);
            auto mapping_segments = mappings_.resolve(
                query.instrument_id,
                provider_name_,
                missing_range,
                query.venue_id);

            std::vector<ProviderMappingSegment> resolved_segments;
            resolved_segments.reserve(mapping_segments.size());
            std::vector<TimeRange> mapped_ranges;
            mapped_ranges.reserve(mapping_segments.size());
            for (const auto& mapping_segment : mapping_segments) {
                validate(mapping_segment);
                if (mapping_segment.instrument_id != query.instrument_id ||
                    mapping_segment.provider != provider_name_) {
                    throw std::invalid_argument("mapping segment does not match the query plan");
                }
                if (query.venue_id && mapping_segment.venue_id != query.venue_id) {
                    throw std::invalid_argument("mapping segment does not match the requested venue");
                }

                const auto clipped = intersection(missing_range, mapping_segment.range);
                if (!clipped) {
                    continue;
                }
                auto resolved_segment = mapping_segment;
                resolved_segment.range = *clipped;
                resolved_segments.push_back(std::move(resolved_segment));
                mapped_ranges.push_back(*clipped);
            }

            std::sort(
                resolved_segments.begin(),
                resolved_segments.end(),
                [](const ProviderMappingSegment& lhs, const ProviderMappingSegment& rhs) {
                    if (lhs.range.start != rhs.range.start) {
                        return lhs.range.start < rhs.range.start;
                    }
                    if (lhs.range.end != rhs.range.end) {
                        return lhs.range.end < rhs.range.end;
                    }
                    if (lhs.venue_id != rhs.venue_id) {
                        return lhs.venue_id < rhs.venue_id;
                    }
                    if (lhs.source_id != rhs.source_id) {
                        return lhs.source_id < rhs.source_id;
                    }
                    return lhs.provider_id < rhs.provider_id;
                });

            for (std::size_t first = 0; first < resolved_segments.size(); ++first) {
                for (std::size_t second = first + 1; second < resolved_segments.size(); ++second) {
                    if (same_mapping_scope(resolved_segments[first], resolved_segments[second]) &&
                        overlaps(resolved_segments[first].range, resolved_segments[second].range)) {
                        throw std::logic_error("mapping resolution returned overlapping provider ranges");
                    }
                }
            }

            const auto unmapped_ranges = subtract(missing_range, mapped_ranges);
            result.unmapped_ranges.insert(
                result.unmapped_ranges.end(), unmapped_ranges.begin(), unmapped_ranges.end());

            for (const auto& mapping_segment : resolved_segments) {
                const MarketDataQuery provider_query{
                    query.instrument_id,
                    query.venue_id,
                    query.level,
                    std::optional<TimeRange>{mapping_segment.range},
                    query.source_receive_time_range,
                    query.primary_time_basis,
                    query.fetch_policy,
                    query.derivation_margin,
                };
                result.segments.push_back(QueryPlanSegment{
                    mapping_segment,
                    provider_query,
                    resolve_provider_fetch_range(provider_query),
                    PostFetchFilters{mapping_segment.range, query.source_receive_time_range},
                });
            }
        }
        return result;
    }

private:
    [[nodiscard]] static bool same_mapping_scope(
        const ProviderMappingSegment& lhs,
        const ProviderMappingSegment& rhs) noexcept {
        return lhs.instrument_id == rhs.instrument_id && lhs.provider == rhs.provider &&
               lhs.venue_id == rhs.venue_id && lhs.source_id == rhs.source_id;
    }

    CachePort& cache_;
    InstrumentMappingRegistry& mappings_;
    std::string provider_name_;
};

} // namespace quant::ingestion
