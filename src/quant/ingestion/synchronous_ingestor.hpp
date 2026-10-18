#pragma once

#include "quant/data/validation.hpp"
#include "quant/ingestion/cache.hpp"
#include "quant/ingestion/provider.hpp"
#include "quant/ingestion/query_plan.hpp"
#include "quant/ingestion/recovery.hpp"
#include "quant/ingestion/result.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quant::ingestion {

namespace detail {

[[nodiscard]] inline bool same_scope(
    const data::MboStreamContext& lhs,
    const data::MboStreamContext& rhs) noexcept {
    return lhs.instrument_id == rhs.instrument_id && lhs.venue_id == rhs.venue_id &&
           lhs.source_id == rhs.source_id;
}

inline void add_unique_scope(
    std::vector<std::optional<data::MboStreamContext>>& scopes,
    const std::optional<data::MboStreamContext>& scope) {
    if (std::find_if(scopes.begin(), scopes.end(), [&scope](const auto& existing) {
            return same_stream_scope(existing, scope);
        }) == scopes.end()) {
        scopes.push_back(scope);
    }
}

inline void add_unique_source(std::vector<data::SourceId>& source_ids, const data::SourceId source_id) {
    if (std::find(source_ids.begin(), source_ids.end(), source_id) == source_ids.end()) {
        source_ids.push_back(source_id);
    }
}

inline void append_event(std::vector<data::MboBuffer>& buffers, const data::MboEvent& event) {
    const data::MboStreamContext context{event.header.instrument_id, event.header.venue_id, event.header.source_id};
    auto buffer = std::find_if(buffers.begin(), buffers.end(), [&context](const auto& candidate) {
        return same_scope(candidate.context(), context);
    });
    if (buffer == buffers.end()) {
        buffers.emplace_back(context);
        buffer = std::prev(buffers.end());
    }
    buffer->append(event);
}

[[nodiscard]] inline std::optional<data::MboStreamContext> known_mapping_scope(
    const ProviderMappingSegment& segment) {
    if (!segment.venue_id || !segment.source_id) {
        return std::nullopt;
    }
    return data::MboStreamContext{segment.instrument_id, *segment.venue_id, *segment.source_id};
}

[[nodiscard]] inline std::optional<data::VenueId> effective_venue(
    const MarketDataQuery& query,
    const ProviderMappingSegment& segment) noexcept {
    return segment.venue_id ? segment.venue_id : query.venue_id;
}

[[nodiscard]] inline bool header_matches_request(
    const data::EventHeader& header,
    const MarketDataQuery& query,
    const QueryPlanSegment& plan_segment) noexcept {
    const auto& segment = plan_segment.mapping;
    const auto& filters = plan_segment.post_fetch_filters;
    return header.instrument_id == query.instrument_id &&
           (!query.venue_id || header.venue_id == *query.venue_id) &&
           (!segment.venue_id || header.venue_id == *segment.venue_id) &&
           (!segment.source_id || header.source_id == *segment.source_id) &&
           (!filters.event_time_range || contains(*filters.event_time_range, header.event_time)) &&
           (!filters.source_receive_time_range ||
            (header.source_receive_time &&
             contains(*filters.source_receive_time_range, *header.source_receive_time)));
}

[[nodiscard]] inline bool quality_scope_matches(
    const data::MboStreamContext& scope,
    const MarketDataQuery& query,
    const QueryPlanSegment& plan_segment) noexcept {
    const auto& segment = plan_segment.mapping;
    return scope.instrument_id == query.instrument_id &&
           (!query.venue_id || scope.venue_id == *query.venue_id) &&
           (!segment.venue_id || scope.venue_id == *segment.venue_id) &&
           (!segment.source_id || scope.source_id == *segment.source_id);
}

} // namespace detail

class SynchronousIngestor {
public:
    SynchronousIngestor(
        CachePort& cache,
        InstrumentMappingRegistry& mappings,
        ProviderPort& provider,
        RecoveryPolicy& recovery,
        DataUsabilityPolicy& usability_policy)
        : provider_(provider),
          recovery_(recovery),
          usability_policy_(usability_policy),
          planner_(cache, mappings, std::string{provider.provider_name()}) {}

    [[nodiscard]] QueryPlan explain(const MarketDataQuery& query) {
        validate(query);
        if (query.level != MarketDataLevel::L3) {
            throw std::invalid_argument("synchronous ingestion currently supports only L3/MBO queries");
        }
        return planner_.plan(query);
    }

    // Persistence stays behind the sink. Each planned provider request is a named,
    // independent acquisition; ordinals across those requests are not comparable.
    [[nodiscard]] std::vector<integrity::Acquisition> acquire_retained(
        const MarketDataQuery& query, const data::Timestamp acquired_at,
        const std::string& operation_id, const std::string& acquisition_prefix,
        const std::string& attempt_id, integrity::AcquisitionSink& sink) {
        integrity::validate_token(operation_id);
        integrity::validate_token(acquisition_prefix);
        integrity::validate_token(attempt_id);
        const auto plan = explain(query);
        if (!plan.unmapped_ranges.empty()) { throw std::invalid_argument("retained acquisition has unresolved mappings"); }
        std::vector<integrity::Acquisition> acquisitions;
        for (std::size_t index = 0; index < plan.segments.size(); ++index) {
            const auto& segment = plan.segments[index];
            const auto acquisition_id = acquisition_prefix + "-" + std::to_string(index);
            integrity::validate_token(acquisition_id);
            integrity::Acquisition request{operation_id, acquisition_id, attempt_id, acquired_at,
                segment.provider_query, segment.mapping, segment.provider_fetch_range};
            acquisitions.push_back(provider_.acquire(std::move(request), sink));
        }
        return acquisitions;
    }

    [[nodiscard]] IngestionResult ingest(
        const MarketDataQuery& query,
        const data::Timestamp ingestion_time,
        std::string adapter_version = {},
        std::string canonical_schema_version = {},
        std::string mapping_version = {}) {
        validate(query);
        if (query.level != MarketDataLevel::L3) {
            throw std::invalid_argument("synchronous ingestion currently supports only L3/MBO queries");
        }
        if (!query.event_time_range) {
            throw std::invalid_argument("synchronous ingestion requires an event-time range for mapping resolution");
        }

        const std::string provider_name{provider_.provider_name()};
        if (provider_name.empty()) {
            throw std::invalid_argument("provider port name must not be empty");
        }

        const auto query_plan = planner_.plan(query);

        std::vector<DataQualityObservation> quality_observations;
        std::vector<data::SourceId> source_ids;
        std::vector<SourceArtifactProvenance> source_artifacts;
        std::vector<std::optional<data::MboStreamContext>> expected_scopes;
        std::vector<data::MboBuffer> mbo_buffers;
        std::vector<data::Trade> trades;
        std::vector<data::OrderExecution> order_executions;
        std::vector<data::ProviderRecordMetadata> provider_records;
        std::vector<ProviderBatchMetadata> provider_batches;

        for (const auto& unmapped : query_plan.unmapped_ranges) {
            quality_observations.push_back(DataQualityObservation{unmapped, DataQualityKind::Missing, std::nullopt});
        }

        for (const auto& plan_segment : query_plan.segments) {
            const auto& mapping_segment = plan_segment.mapping;
            if (const auto scope = detail::known_mapping_scope(mapping_segment)) {
                detail::add_unique_scope(expected_scopes, scope);
            }
            if (mapping_segment.source_id) {
                detail::add_unique_source(source_ids, *mapping_segment.source_id);
            }

            ProviderBatch response = provider_.fetch(plan_segment.provider_query, mapping_segment);
                for (const auto& batch_metadata : response.batch_metadata) {
                    if (batch_metadata.dataset.empty() || batch_metadata.schema.empty()) {
                        throw std::invalid_argument("provider batch metadata must identify its dataset and schema");
                    }
                    provider_batches.push_back(batch_metadata);
                }
                for (const auto source_id : response.source_ids) {
                    detail::add_unique_source(source_ids, source_id);
                    if (const auto venue = detail::effective_venue(query, mapping_segment)) {
                        detail::add_unique_scope(
                            expected_scopes,
                            data::MboStreamContext{query.instrument_id, *venue, source_id});
                    }
                }

                for (const auto& artifact : response.source_artifacts) {
                    if (artifact.artifact_identity.empty()) {
                        throw std::invalid_argument("provider source artifact identity must not be empty");
                    }
                    validate(artifact.range);
                    const auto clipped = intersection(mapping_segment.range, artifact.range);
                    if (!clipped) {
                        throw std::invalid_argument("provider source artifact is outside its requested range");
                    }
                    source_artifacts.push_back(SourceArtifactProvenance{artifact.artifact_identity, *clipped});
                }

                std::vector<std::optional<data::MboStreamContext>> response_scopes;
                std::vector<std::optional<data::MboStreamContext>> invalid_event_scopes;
                bool validation_failed = false;
                for (const auto& record : response.provider_records) {
                    if (!detail::header_matches_request(record.header, query, plan_segment)) {
                        continue;
                    }
                    detail::add_unique_source(source_ids, record.header.source_id);
                    const auto record_context = std::optional<data::MboStreamContext>{data::MboStreamContext{
                        record.header.instrument_id,
                        record.header.venue_id,
                        record.header.source_id,
                    }};
                    detail::add_unique_scope(response_scopes, record_context);
                    detail::add_unique_scope(expected_scopes, record_context);
                    provider_records.push_back(record);
                }

                for (const auto& event : response.mbo_events) {
                    const bool in_requested_scope = detail::header_matches_request(event.header, query, plan_segment);
                    if (!in_requested_scope) {
                        continue;
                    }
                    detail::add_unique_source(source_ids, event.header.source_id);
                    const bool structurally_valid = data::is_valid(data::validate(event));
                    const auto event_context = std::optional<data::MboStreamContext>{data::MboStreamContext{
                        event.header.instrument_id,
                        event.header.venue_id,
                        event.header.source_id,
                    }};
                    if (!structurally_valid) {
                        validation_failed = true;
                        detail::add_unique_scope(invalid_event_scopes, event_context);
                        detail::add_unique_scope(response_scopes, event_context);
                        detail::add_unique_scope(expected_scopes, event_context);
                        continue;
                    }

                    detail::add_unique_scope(response_scopes, event_context);
                    detail::add_unique_scope(expected_scopes, event_context);
                    detail::append_event(mbo_buffers, event);
                }

                for (const auto& trade : response.trades) {
                    const bool in_requested_scope = detail::header_matches_request(trade.header, query, plan_segment);
                    if (!in_requested_scope) {
                        continue;
                    }
                    detail::add_unique_source(source_ids, trade.header.source_id);
                    const bool structurally_valid = data::is_valid(data::validate(trade));
                    const auto trade_context = std::optional<data::MboStreamContext>{data::MboStreamContext{
                        trade.header.instrument_id,
                        trade.header.venue_id,
                        trade.header.source_id,
                    }};
                    if (!structurally_valid) {
                        validation_failed = true;
                        detail::add_unique_scope(invalid_event_scopes, trade_context);
                        detail::add_unique_scope(response_scopes, trade_context);
                        detail::add_unique_scope(expected_scopes, trade_context);
                        continue;
                    }
                    detail::add_unique_scope(response_scopes, trade_context);
                    detail::add_unique_scope(expected_scopes, trade_context);
                    trades.push_back(trade);
                }

                for (const auto& execution : response.order_executions) {
                    const bool in_requested_scope =
                        detail::header_matches_request(execution.header, query, plan_segment);
                    if (!in_requested_scope) {
                        continue;
                    }
                    detail::add_unique_source(source_ids, execution.header.source_id);
                    const bool structurally_valid = data::is_valid(data::validate(execution));
                    const auto execution_context = std::optional<data::MboStreamContext>{data::MboStreamContext{
                        execution.header.instrument_id,
                        execution.header.venue_id,
                        execution.header.source_id,
                    }};
                    if (!structurally_valid) {
                        validation_failed = true;
                        detail::add_unique_scope(invalid_event_scopes, execution_context);
                        detail::add_unique_scope(response_scopes, execution_context);
                        detail::add_unique_scope(expected_scopes, execution_context);
                        continue;
                    }
                    detail::add_unique_scope(response_scopes, execution_context);
                    detail::add_unique_scope(expected_scopes, execution_context);
                    order_executions.push_back(execution);
                }

                const auto observation_scopes = [&]() {
                    std::vector<std::optional<data::MboStreamContext>> scopes;
                    if (!response_scopes.empty()) {
                        scopes = response_scopes;
                    } else if (const auto scope = detail::known_mapping_scope(mapping_segment)) {
                        scopes.push_back(scope);
                    } else if (const auto venue = detail::effective_venue(query, mapping_segment)) {
                        for (const auto source_id : response.source_ids) {
                            detail::add_unique_scope(
                                scopes,
                                data::MboStreamContext{query.instrument_id, *venue, source_id});
                        }
                    }
                    return scopes;
                };

                for (const auto& observation : response.quality_observations) {
                    validate(observation.range);
                    const auto clipped = intersection(mapping_segment.range, observation.range);
                    if (!clipped) {
                        throw std::invalid_argument("provider quality evidence is outside its requested range");
                    }

                    std::vector<std::optional<data::MboStreamContext>> scopes;
                    if (observation.scope) {
                        if (!detail::quality_scope_matches(*observation.scope, query, plan_segment)) {
                            throw std::invalid_argument("provider quality evidence scope does not match its request");
                        }
                        scopes.push_back(observation.scope);
                    } else {
                        scopes = observation_scopes();
                        if (scopes.empty()) {
                            scopes.push_back(std::nullopt);
                        }
                    }

                    for (const auto& scope : scopes) {
                        detail::add_unique_scope(expected_scopes, scope);
                        quality_observations.push_back(DataQualityObservation{*clipped, observation.kind, scope});
                    }
                }

                if (validation_failed) {
                    auto corrupt_scopes = invalid_event_scopes;
                    if (corrupt_scopes.empty()) {
                        corrupt_scopes = observation_scopes();
                    }
                    if (corrupt_scopes.empty()) {
                        corrupt_scopes.push_back(std::nullopt);
                    }
                    for (const auto& scope : corrupt_scopes) {
                        detail::add_unique_scope(expected_scopes, scope);
                        quality_observations.push_back(
                            DataQualityObservation{mapping_segment.range, DataQualityKind::Corrupt, scope});
                    }
                }
        }

        const auto data_state_segments = DataStateReducer::reduce(
            *query.event_time_range,
            quality_observations,
            expected_scopes);
        const auto data_state = summarize_data_state(data_state_segments);
        std::vector<DataStateSegment> actual_coverage;
        std::vector<DataStateSegment> unresolved_ranges;
        for (const auto& segment : data_state_segments) {
            if (usability_policy_.is_usable(query, segment)) {
                actual_coverage.push_back(segment);
            } else {
                unresolved_ranges.push_back(segment);
            }
        }

        const RecoveryContext recovery_context{
            query,
            std::span<const DataStateSegment>{data_state_segments},
            std::span<const DataStateSegment>{unresolved_ranges},
            data_state,
        };
        static_cast<void>(recovery_.decide(recovery_context));

        IngestionMetadata metadata{
            query,
            std::move(actual_coverage),
            std::move(unresolved_ranges),
            std::move(quality_observations),
            data_state_segments,
            data_state,
            provider_name,
            std::move(source_ids),
            std::move(source_artifacts),
            ingestion_time,
            std::move(adapter_version),
            std::move(canonical_schema_version),
            std::move(mapping_version),
            std::move(provider_records),
            std::move(provider_batches),
            AcquisitionMode::Historical,
        };
        return IngestionResult{
            std::move(mbo_buffers),
            std::move(metadata),
            std::move(trades),
            std::move(order_executions),
        };
    }

private:
    ProviderPort& provider_;
    RecoveryPolicy& recovery_;
    DataUsabilityPolicy& usability_policy_;
    QueryPlanner planner_;
};

} // namespace quant::ingestion
