#pragma once

#include "quant/ingestion/result.hpp"
#include "quant/integrity/model.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quant::storage {

enum class DataKind {
    Mbo,
    Trades,
    OrderExecution,
};

[[nodiscard]] inline std::string_view data_kind_name(const DataKind kind) noexcept {
    switch (kind) {
    case DataKind::Mbo:
        return "mbo";
    case DataKind::Trades:
        return "trades";
    case DataKind::OrderExecution:
        return "order_executions";
    }
    return "unknown";
}

using StreamIdentity = integrity::StreamIdentity;

struct DataFragment {
    DataKind data_kind;
    StreamIdentity stream;
    ingestion::TimeRange time_range;
    std::string schema_version;
    std::string locator;
    std::string version_id{};
    std::string acquisition_id{};
    std::string package_locator{};
    std::optional<data::StreamId> row_stream_id{};
    std::string evidence_digest{};

    friend bool operator==(const DataFragment&, const DataFragment&) = default;
};

struct DatasetLocalMetadata {
    DataKind data_kind;
    StreamIdentity stream;
    ingestion::TimeRange time_range;
    ingestion::AcquisitionMode acquisition_mode;
    std::string schema_version;
    std::string adapter_version;
    std::string mapping_version;
    std::vector<std::string> quality_references;
    std::vector<ingestion::SourceArtifactProvenance> source_artifacts;
    std::optional<std::string> checksum;

    friend bool operator==(const DatasetLocalMetadata&, const DatasetLocalMetadata&) = default;
};

inline void validate(const StreamIdentity& stream) {
    if (stream.instrument_id.value() == 0 || stream.venue_id.value() == 0 || stream.source_id.value() == 0) {
        throw std::invalid_argument("dataset stream identity must use non-zero ids");
    }
}

inline void validate(const DataFragment& fragment) {
    validate(fragment.time_range);
    validate(fragment.stream);
    if (data_kind_name(fragment.data_kind) == "unknown") {
        throw std::invalid_argument("data fragment must use a known data kind");
    }
    if (fragment.schema_version.empty()) {
        throw std::invalid_argument("data fragment schema version must not be empty");
    }
    if (fragment.locator.empty()) {
        throw std::invalid_argument("data fragment storage locator must not be empty");
    }
}

inline void validate(const DatasetLocalMetadata& metadata) {
    validate(metadata.time_range);
    validate(metadata.stream);
    if (data_kind_name(metadata.data_kind) == "unknown") {
        throw std::invalid_argument("dataset metadata must use a known data kind");
    }
    if (metadata.schema_version.empty()) {
        throw std::invalid_argument("dataset schema version must not be empty");
    }
    for (const auto& reference : metadata.quality_references) {
        if (reference.empty()) {
            throw std::invalid_argument("dataset quality references must not be empty");
        }
    }
    for (const auto& artifact : metadata.source_artifacts) {
        validate(artifact.range);
        if (artifact.artifact_identity.empty()) {
            throw std::invalid_argument("dataset source artifact identities must not be empty");
        }
    }
    if (metadata.checksum && metadata.checksum->empty()) {
        throw std::invalid_argument("dataset checksum must not be empty");
    }
}

[[nodiscard]] inline DataFragment fragment_from_metadata(
    const DatasetLocalMetadata& metadata,
    std::string locator) {
    validate(metadata);
    DataFragment fragment{
        metadata.data_kind,
        metadata.stream,
        metadata.time_range,
        metadata.schema_version,
        std::move(locator),
    };
    validate(fragment);
    return fragment;
}

} // namespace quant::storage
