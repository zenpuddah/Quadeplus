#pragma once

#include "quant/data/observations.hpp"
#include "quant/ingestion/quality.hpp"

#include <optional>
#include <string>
#include <vector>

namespace quant::ingestion {

struct SourceArtifactProvenance {
    std::string artifact_identity;
    TimeRange range;

    friend bool operator==(const SourceArtifactProvenance&, const SourceArtifactProvenance&) = default;
};

struct ProviderConditionEvidence {
    std::string date;
    std::string condition;
    std::optional<std::string> last_modified_date;

    friend bool operator==(const ProviderConditionEvidence&, const ProviderConditionEvidence&) = default;
};

struct ProviderBatchMetadata {
    std::string dataset;
    std::string schema;
    std::string provider_start;
    std::string provider_end;
    std::vector<ProviderConditionEvidence> conditions;
    std::vector<std::string> partial_symbols;
    std::vector<std::string> not_found_symbols;

    friend bool operator==(const ProviderBatchMetadata&, const ProviderBatchMetadata&) = default;
};

enum class AcquisitionMode {
    Historical,
    Live,
};

struct IngestionMetadata {
    MarketDataQuery query;
    std::vector<DataStateSegment> actual_coverage;
    std::vector<DataStateSegment> unresolved_ranges;
    std::vector<DataQualityObservation> quality_observations;
    std::vector<DataStateSegment> data_state_segments;
    DataState data_state;
    std::string provider;
    std::vector<data::SourceId> source_ids;
    std::vector<SourceArtifactProvenance> source_artifacts;
    data::Timestamp ingestion_time;
    std::string adapter_version;
    std::string canonical_schema_version;
    std::string mapping_version;
    std::vector<data::ProviderRecordMetadata> provider_records;
    std::vector<ProviderBatchMetadata> provider_batches;
    AcquisitionMode acquisition_mode = AcquisitionMode::Historical;
};

struct IngestionResult {
    std::vector<data::MboBuffer> mbo_buffers;
    IngestionMetadata metadata;
    std::vector<data::Trade> trades;
    std::vector<data::OrderExecution> order_executions;
};

} // namespace quant::ingestion
