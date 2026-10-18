#pragma once

#include "quant/storage/backend.hpp"

#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace quant::storage {

namespace parquet_detail {

inline constexpr std::string_view data_kind_key = "quant.data_kind";
inline constexpr std::string_view instrument_id_key = "quant.instrument_id";
inline constexpr std::string_view venue_id_key = "quant.venue_id";
inline constexpr std::string_view source_id_key = "quant.source_id";
inline constexpr std::string_view range_start_key = "quant.time_range.start_ns";
inline constexpr std::string_view range_end_key = "quant.time_range.end_ns";
inline constexpr std::string_view acquisition_mode_key = "quant.acquisition_mode";
inline constexpr std::string_view schema_version_key = "quant.schema_version";
inline constexpr std::string_view adapter_version_key = "quant.adapter_version";
inline constexpr std::string_view mapping_version_key = "quant.mapping_version";
inline constexpr std::string_view quality_reference_count_key = "quant.quality_reference_count";
inline constexpr std::string_view quality_reference_prefix = "quant.quality_reference.";
inline constexpr std::string_view source_artifact_count_key = "quant.source_artifact_count";
inline constexpr std::string_view source_artifact_prefix = "quant.source_artifact.";
inline constexpr std::string_view checksum_key = "quant.checksum";

[[nodiscard]] inline std::string acquisition_mode_name(const ingestion::AcquisitionMode mode) {
    switch (mode) {
    case ingestion::AcquisitionMode::Historical:
        return "historical";
    case ingestion::AcquisitionMode::Live:
        return "live";
    }
    return "unknown";
}

[[nodiscard]] inline arrow::Result<ingestion::AcquisitionMode> parse_acquisition_mode(
    const std::string& value) {
    if (value == "historical") {
        return ingestion::AcquisitionMode::Historical;
    }
    if (value == "live") {
        return ingestion::AcquisitionMode::Live;
    }
    return arrow::Status::Invalid("unknown dataset acquisition mode");
}

[[nodiscard]] inline arrow::Result<DataKind> parse_data_kind(const std::string& value) {
    if (value == data_kind_name(DataKind::Mbo)) {
        return DataKind::Mbo;
    }
    if (value == data_kind_name(DataKind::Trades)) {
        return DataKind::Trades;
    }
    if (value == data_kind_name(DataKind::OrderExecution)) {
        return DataKind::OrderExecution;
    }
    return arrow::Status::Invalid("unknown persisted data kind");
}

[[nodiscard]] inline arrow::Result<std::string> required_value(
    const arrow::KeyValueMetadata& metadata,
    const std::string_view key) {
    if (!metadata.Contains(key)) {
        return arrow::Status::Invalid("Parquet metadata is missing key: ", key);
    }
    return metadata.Get(key);
}

[[nodiscard]] inline arrow::Result<std::uint64_t> parse_uint64(
    const std::string& value,
    const std::string_view key) {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return arrow::Status::Invalid("Parquet metadata contains an invalid integer for key: ", key);
    }
    return parsed;
}

[[nodiscard]] inline arrow::Result<std::int64_t> parse_int64(
    const std::string& value,
    const std::string_view key) {
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return arrow::Status::Invalid("Parquet metadata contains an invalid integer for key: ", key);
    }
    return parsed;
}

[[nodiscard]] inline arrow::Result<std::uint32_t> parse_uint32(
    const std::string& value,
    const std::string_view key) {
    ARROW_ASSIGN_OR_RAISE(const auto parsed, parse_uint64(value, key));
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        return arrow::Status::Invalid("Parquet metadata integer exceeds uint32 range for key: ", key);
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] inline std::string indexed_key(const std::string_view prefix, const std::size_t index) {
    return std::string{prefix} + std::to_string(index);
}

[[nodiscard]] inline std::shared_ptr<arrow::KeyValueMetadata> encode_metadata(
    const DatasetLocalMetadata& metadata) {
    std::vector<std::string> keys;
    std::vector<std::string> values;
    const auto add = [&keys, &values](const std::string_view key, std::string value) {
        keys.emplace_back(key);
        values.push_back(std::move(value));
    };

    add(data_kind_key, std::string{data_kind_name(metadata.data_kind)});
    add(instrument_id_key, std::to_string(metadata.stream.instrument_id.value()));
    add(venue_id_key, std::to_string(metadata.stream.venue_id.value()));
    add(source_id_key, std::to_string(metadata.stream.source_id.value()));
    add(range_start_key, std::to_string(metadata.time_range.start.unix_nanos()));
    add(range_end_key, std::to_string(metadata.time_range.end.unix_nanos()));
    add(acquisition_mode_key, acquisition_mode_name(metadata.acquisition_mode));
    add(schema_version_key, metadata.schema_version);
    add(adapter_version_key, metadata.adapter_version);
    add(mapping_version_key, metadata.mapping_version);
    add(quality_reference_count_key, std::to_string(metadata.quality_references.size()));
    for (std::size_t index = 0; index < metadata.quality_references.size(); ++index) {
        add(indexed_key(quality_reference_prefix, index), metadata.quality_references[index]);
    }
    add(source_artifact_count_key, std::to_string(metadata.source_artifacts.size()));
    for (std::size_t index = 0; index < metadata.source_artifacts.size(); ++index) {
        const auto& artifact = metadata.source_artifacts[index];
        const auto prefix = indexed_key(source_artifact_prefix, index);
        add(prefix + ".identity", artifact.artifact_identity);
        add(prefix + ".start_ns", std::to_string(artifact.range.start.unix_nanos()));
        add(prefix + ".end_ns", std::to_string(artifact.range.end.unix_nanos()));
    }
    if (metadata.checksum) {
        add(checksum_key, *metadata.checksum);
    }
    return arrow::KeyValueMetadata::Make(std::move(keys), std::move(values));
}

[[nodiscard]] inline arrow::Result<std::vector<std::string>> decode_references(
    const arrow::KeyValueMetadata& metadata,
    const std::string_view count_key,
    const std::string_view value_prefix) {
    ARROW_ASSIGN_OR_RAISE(const auto count_text, required_value(metadata, count_key));
    ARROW_ASSIGN_OR_RAISE(const auto count, parse_uint64(count_text, count_key));
    if (count > 1'000'000) {
        return arrow::Status::Invalid("Parquet metadata reference count is unreasonable");
    }

    std::vector<std::string> references;
    references.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto key = indexed_key(value_prefix, static_cast<std::size_t>(index));
        ARROW_ASSIGN_OR_RAISE(const auto reference, required_value(metadata, key));
        if (reference.empty()) {
            return arrow::Status::Invalid("Parquet metadata contains an empty dataset reference");
        }
        references.push_back(reference);
    }
    return references;
}

[[nodiscard]] inline arrow::Result<std::vector<ingestion::SourceArtifactProvenance>> decode_source_artifacts(
    const arrow::KeyValueMetadata& metadata) {
    ARROW_ASSIGN_OR_RAISE(const auto count_text, required_value(metadata, source_artifact_count_key));
    ARROW_ASSIGN_OR_RAISE(const auto count, parse_uint64(count_text, source_artifact_count_key));
    if (count > 1'000'000) {
        return arrow::Status::Invalid("Parquet metadata source-artifact count is unreasonable");
    }

    std::vector<ingestion::SourceArtifactProvenance> artifacts;
    artifacts.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto prefix = indexed_key(source_artifact_prefix, static_cast<std::size_t>(index));
        ARROW_ASSIGN_OR_RAISE(const auto identity, required_value(metadata, prefix + ".identity"));
        ARROW_ASSIGN_OR_RAISE(const auto start_text, required_value(metadata, prefix + ".start_ns"));
        ARROW_ASSIGN_OR_RAISE(const auto start, parse_int64(start_text, prefix + ".start_ns"));
        ARROW_ASSIGN_OR_RAISE(const auto end_text, required_value(metadata, prefix + ".end_ns"));
        ARROW_ASSIGN_OR_RAISE(const auto end, parse_int64(end_text, prefix + ".end_ns"));
        try {
            artifacts.push_back(ingestion::SourceArtifactProvenance{
                identity,
                ingestion::TimeRange{
                    data::Timestamp::from_unix_nanos(start),
                    data::Timestamp::from_unix_nanos(end),
                },
            });
        } catch (const std::exception&) {
            return arrow::Status::Invalid("Parquet metadata contains an invalid source artifact range");
        }
    }
    return artifacts;
}

[[nodiscard]] inline arrow::Result<DatasetLocalMetadata> decode_metadata(
    const arrow::KeyValueMetadata& metadata) {
    ARROW_ASSIGN_OR_RAISE(const auto kind_text, required_value(metadata, data_kind_key));
    ARROW_ASSIGN_OR_RAISE(const auto kind, parse_data_kind(kind_text));
    ARROW_ASSIGN_OR_RAISE(const auto instrument_text, required_value(metadata, instrument_id_key));
    ARROW_ASSIGN_OR_RAISE(const auto instrument_id, parse_uint64(instrument_text, instrument_id_key));
    ARROW_ASSIGN_OR_RAISE(const auto venue_text, required_value(metadata, venue_id_key));
    ARROW_ASSIGN_OR_RAISE(const auto venue_id, parse_uint32(venue_text, venue_id_key));
    ARROW_ASSIGN_OR_RAISE(const auto source_text, required_value(metadata, source_id_key));
    ARROW_ASSIGN_OR_RAISE(const auto source_id, parse_uint32(source_text, source_id_key));
    ARROW_ASSIGN_OR_RAISE(const auto start_text, required_value(metadata, range_start_key));
    ARROW_ASSIGN_OR_RAISE(const auto start, parse_int64(start_text, range_start_key));
    ARROW_ASSIGN_OR_RAISE(const auto end_text, required_value(metadata, range_end_key));
    ARROW_ASSIGN_OR_RAISE(const auto end, parse_int64(end_text, range_end_key));
    ARROW_ASSIGN_OR_RAISE(const auto mode_text, required_value(metadata, acquisition_mode_key));
    ARROW_ASSIGN_OR_RAISE(const auto mode, parse_acquisition_mode(mode_text));
    ARROW_ASSIGN_OR_RAISE(const auto schema_version, required_value(metadata, schema_version_key));
    ARROW_ASSIGN_OR_RAISE(const auto adapter_version, required_value(metadata, adapter_version_key));
    ARROW_ASSIGN_OR_RAISE(const auto mapping_version, required_value(metadata, mapping_version_key));
    ARROW_ASSIGN_OR_RAISE(
        const auto quality_references,
        decode_references(metadata, quality_reference_count_key, quality_reference_prefix));
    ARROW_ASSIGN_OR_RAISE(const auto source_artifacts, decode_source_artifacts(metadata));

    std::optional<std::string> checksum;
    if (metadata.Contains(checksum_key)) {
        ARROW_ASSIGN_OR_RAISE(const auto checksum_value, metadata.Get(checksum_key));
        if (checksum_value.empty()) {
            return arrow::Status::Invalid("Parquet metadata checksum must not be empty");
        }
        checksum = checksum_value;
    }

    try {
        DatasetLocalMetadata result{
            kind,
            StreamIdentity{data::InstrumentId{instrument_id}, data::VenueId{venue_id}, data::SourceId{source_id}},
            ingestion::TimeRange{
                data::Timestamp::from_unix_nanos(start),
                data::Timestamp::from_unix_nanos(end),
            },
            mode,
            schema_version,
            adapter_version,
            mapping_version,
            quality_references,
            source_artifacts,
            checksum,
        };
        validate(result);
        return result;
    } catch (const std::exception&) {
        return arrow::Status::Invalid("Parquet metadata contains an invalid dataset identity");
    }
}

[[nodiscard]] inline arrow::Result<std::unique_ptr<parquet::arrow::FileReader>> open_reader(
    const std::string& locator) {
    parquet::arrow::FileReaderBuilder builder;
    ARROW_RETURN_NOT_OK(builder.OpenFile(locator));
    return builder.Build();
}

[[nodiscard]] inline arrow::Result<DatasetLocalMetadata> metadata_from_reader(
    const parquet::arrow::FileReader& reader) {
    const auto& metadata = reader.parquet_reader()->metadata()->key_value_metadata();
    if (!metadata) {
        return arrow::Status::Invalid("Parquet artifact has no dataset-local metadata");
    }
    return decode_metadata(*metadata);
}

[[nodiscard]] inline arrow::Status validate_fragment_metadata(
    const DataFragment& fragment,
    const DatasetLocalMetadata& metadata) {
    if (fragment.data_kind != metadata.data_kind || fragment.stream != metadata.stream ||
        fragment.time_range != metadata.time_range || fragment.schema_version != metadata.schema_version) {
        return arrow::Status::Invalid("requested data fragment does not match persisted dataset metadata");
    }
    return arrow::Status::OK();
}

[[nodiscard]] inline std::string safe_token(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_') {
            result.push_back(character);
        } else {
            result.push_back('_');
        }
    }
    return result;
}

[[nodiscard]] inline std::filesystem::path fragment_path(
    const std::filesystem::path& root,
    const DatasetLocalMetadata& metadata) {
    const auto& range = metadata.time_range;
    std::string filename = "instrument_" + std::to_string(metadata.stream.instrument_id.value()) +
                           "_venue_" + std::to_string(metadata.stream.venue_id.value()) +
                           "_source_" + std::to_string(metadata.stream.source_id.value()) +
                           "_" + std::to_string(range.start.unix_nanos()) + "_" +
                           std::to_string(range.end.unix_nanos()) + "_" +
                           safe_token(metadata.schema_version);
    if (metadata.checksum) {
        filename += "_" + safe_token(*metadata.checksum);
    }
    filename += ".parquet";
    return root / "data" / std::string{data_kind_name(metadata.data_kind)} / filename;
}

template <typename Consumer>
[[nodiscard]] inline arrow::Status consume_batches(
    parquet::arrow::FileReader& file_reader,
    const DataKind kind,
    const BatchProjection projection,
    Consumer&& consumer) {
    file_reader.set_use_threads(false);
    file_reader.set_batch_size(65'536);

    std::unique_ptr<arrow::RecordBatchReader> batch_reader;
    if (projection == BatchProjection::Full) {
        ARROW_ASSIGN_OR_RAISE(batch_reader, file_reader.GetRecordBatchReader());
    } else {
        std::shared_ptr<arrow::Schema> schema;
        ARROW_RETURN_NOT_OK(file_reader.GetSchema(&schema));
        std::vector<int> column_indices;
        for (const auto& name : canonical_column_names(kind)) {
            const auto index = schema->GetFieldIndex(name);
            if (index < 0) {
                return arrow::Status::Invalid("Parquet artifact is missing replay column: ", name);
            }
            column_indices.push_back(index);
        }
        std::vector<int> row_group_indices(static_cast<std::size_t>(file_reader.num_row_groups()));
        std::iota(row_group_indices.begin(), row_group_indices.end(), 0);
        ARROW_ASSIGN_OR_RAISE(
            batch_reader,
            file_reader.GetRecordBatchReader(row_group_indices, column_indices));
    }

    while (true) {
        ArrowBatch batch;
        ARROW_RETURN_NOT_OK(batch_reader->ReadNext(&batch));
        if (!batch) {
            break;
        }
        ARROW_RETURN_NOT_OK(validate_batch_schema(*batch->schema(), kind));
        ARROW_RETURN_NOT_OK(consumer(batch));
    }
    return arrow::Status::OK();
}

} // namespace parquet_detail

} // namespace quant::storage
