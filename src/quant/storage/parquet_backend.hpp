#pragma once

#include "quant/storage/parquet_metadata.hpp"
#include "quant/storage/retained_capture.hpp"
#include "quant/integrity/prepare.hpp"

#include <set>

namespace quant::storage {

enum class PublicationPoint { Raw, References, Canonical, Consumer, Evidence, BeforeComplete };
using PublicationFault = std::function<void(PublicationPoint)>;

struct PublishedVersion {
    std::string version_id;
    std::string package_locator;
    std::string evidence_digest;
    std::vector<DataFragment> fragments;
};

namespace publication_detail {

struct FileEvidence {
    std::string path;
    std::string digest;
    std::string stage;
    std::string parent;
    std::string parent_digest;
};
struct Manifest {
    std::string version;
    std::string attempt;
    std::string acquisition;
    std::uint64_t stream;
    ingestion::TimeRange range;
    bool partial;
    integrity::ConsumerRequirements requirements;
    std::vector<FileEvidence> files;
};

inline void put_requirements(evidence::Writer& writer, const integrity::ConsumerRequirements requirements) {
    for (const bool value : {requirements.ordering, requirements.groups, requirements.initial_state,
                            requirements.allow_degraded, requirements.explicit_partial}) { writer.number(value); }
}
inline integrity::ConsumerRequirements get_requirements(evidence::Reader& reader) {
    const auto order = reader.boolean(); const auto groups = reader.boolean(); const auto initial = reader.boolean();
    const auto degraded = reader.boolean(); const auto partial = reader.boolean();
    return {order, groups, initial, degraded, partial};
}
inline std::string encode_manifest(const Manifest& manifest) {
    evidence::Writer writer; writer.text("quant-evidence-v1"); writer.text(std::string{supported_encoding});
    writer.text(manifest.version); writer.text(manifest.attempt); writer.text(manifest.acquisition);
    writer.number(manifest.stream); evidence::put(writer, manifest.range); writer.number(manifest.partial);
    put_requirements(writer, manifest.requirements); writer.number(manifest.files.size());
    for (const auto& file : manifest.files) {
        writer.text(file.path); writer.text(file.digest); writer.text(file.stage); writer.text(file.parent); writer.text(file.parent_digest);
    }
    return writer.str();
}
inline void safe_relative(const std::string& path) {
    const auto value = std::filesystem::path{path};
    if (value.empty() || value.is_absolute()) { throw std::invalid_argument("invalid evidence path"); }
    for (const auto& component : value) {
        if (component == ".." || component == "." || component.empty()) { throw std::invalid_argument("unsafe evidence path"); }
    }
}
inline Manifest decode_manifest(const std::string& text) {
    evidence::Reader reader{text}; reader.expect("quant-evidence-v1"); reader.expect(std::string{supported_encoding});
    const auto version = reader.text(); const auto attempt = reader.text(); const auto acquisition = reader.text();
    integrity::validate_token(version); integrity::validate_token(attempt); integrity::validate_token(acquisition);
    const auto stream = reader.number<std::uint64_t>(); static_cast<void>(data::StreamId{stream});
    const auto range = evidence::get_range(reader); const auto partial = reader.boolean();
    const auto requirements = get_requirements(reader);
    Manifest manifest{version, attempt, acquisition, stream, range, partial, requirements, {}};
    const auto count = reader.count();
    std::set<std::string> paths;
    for (std::size_t index = 0; index < count; ++index) {
        const auto path = reader.text(); const auto digest = reader.text(); const auto stage = reader.text();
        const auto parent = reader.text(); const auto parent_digest = reader.text();
        safe_relative(path);
        if (!parent.empty()) { safe_relative(parent); }
        if (digest.size() != 64 || (!parent_digest.empty() && parent_digest.size() != 64) || !paths.insert(path).second) {
            throw std::invalid_argument("invalid/duplicate file evidence");
        }
        manifest.files.push_back({path, digest, stage, parent, parent_digest});
    }
    reader.finish(); return manifest;
}

inline arrow::Status write_arrow(const std::filesystem::path& path, const ArrowBatch& batch,
                                  const std::shared_ptr<arrow::KeyValueMetadata>& metadata) {
    if (!batch) { return arrow::Status::Invalid("null stage Arrow batch"); }
    ARROW_RETURN_NOT_OK(batch->ValidateFull());
    try { files::create_directories(path.parent_path()); files::write_bytes(path, {}); }
    catch (const std::exception& error) { return arrow::Status::IOError(error.what()); }
    ARROW_ASSIGN_OR_RAISE(const auto output, arrow::io::FileOutputStream::Open(path.string()));
    ARROW_ASSIGN_OR_RAISE(auto writer, parquet::arrow::FileWriter::Open(*batch->schema(), arrow::default_memory_pool(), output));
    ARROW_RETURN_NOT_OK(writer->AddKeyValueMetadata(metadata));
    ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
    ARROW_RETURN_NOT_OK(writer->Close()); ARROW_RETURN_NOT_OK(output->Close());
    try { files::sync(path); files::sync(path.parent_path()); }
    catch (const std::exception& error) { return arrow::Status::IOError(error.what()); }
    return arrow::Status::OK();
}
inline arrow::Result<std::vector<ArrowBatch>> read_arrow(const std::filesystem::path& path) {
    ARROW_ASSIGN_OR_RAISE(auto reader, parquet_detail::open_reader(path.string()));
    reader->set_use_threads(false); reader->set_batch_size(65'536);
    ARROW_ASSIGN_OR_RAISE(auto batches, reader->GetRecordBatchReader());
    std::vector<ArrowBatch> result;
    while (true) {
        ArrowBatch batch; ARROW_RETURN_NOT_OK(batches->ReadNext(&batch));
        if (!batch) { break; }
        ARROW_RETURN_NOT_OK(batch->ValidateFull()); result.push_back(batch);
    }
    return result;
}
inline arrow::Result<ArrowBatch> raw_arrow(const integrity::Acquisition& acquisition) {
    arrow::UInt64Builder ordinals; arrow::BinaryBuilder records;
    for (const auto& row : acquisition.records) {
        ARROW_RETURN_NOT_OK(ordinals.Append(row.ordinal));
        ARROW_RETURN_NOT_OK(records.Append(reinterpret_cast<const std::uint8_t*>(row.bytes.data()), static_cast<std::int32_t>(row.bytes.size())));
    }
    std::shared_ptr<arrow::Array> ordinal_array; std::shared_ptr<arrow::Array> record_array;
    ARROW_RETURN_NOT_OK(ordinals.Finish(&ordinal_array)); ARROW_RETURN_NOT_OK(records.Finish(&record_array));
    return arrow::RecordBatch::Make(arrow::schema({arrow::field("event_ordinal", arrow::uint64(), false),
        arrow::field("provider_record", arrow::binary(), false)}), static_cast<int64_t>(acquisition.records.size()), {ordinal_array, record_array});
}
inline arrow::Result<std::vector<integrity::RawRecord>> read_raw(const std::filesystem::path& path, const std::string& acquisition_id) {
    ARROW_ASSIGN_OR_RAISE(const auto batches, read_arrow(path));
    ARROW_ASSIGN_OR_RAISE(auto file_reader, parquet_detail::open_reader(path.string()));
    const auto metadata = file_reader->parquet_reader()->metadata()->key_value_metadata();
    if (!metadata || !metadata->Contains("quant.encoding")) { return arrow::Status::Invalid("missing raw encoding"); }
    ARROW_ASSIGN_OR_RAISE(const auto encoding, metadata->Get("quant.encoding"));
    if (encoding != "quant-provider-bytes-v1") { return arrow::Status::Invalid("unsupported raw encoding"); }
    ARROW_ASSIGN_OR_RAISE(const auto scope, metadata->Get("quant.acquisition_id"));
    if (scope != acquisition_id) { return arrow::Status::Invalid("raw acquisition scope mismatch"); }
    std::shared_ptr<arrow::Schema> schema; ARROW_RETURN_NOT_OK(file_reader->GetSchema(&schema));
    const auto expected_schema = arrow::schema({arrow::field("event_ordinal", arrow::uint64(), false), arrow::field("provider_record", arrow::binary(), false)});
    if (!schema->Equals(*expected_schema, false)) { return arrow::Status::Invalid("known raw encoding has incorrect columns"); }
    std::vector<integrity::RawRecord> result;
    for (const auto& batch : batches) {
        ARROW_ASSIGN_OR_RAISE(const auto ordinals, detail::column<arrow::UInt64Array>(*batch, "event_ordinal", arrow::Type::UINT64));
        ARROW_ASSIGN_OR_RAISE(const auto bytes, detail::column<arrow::BinaryArray>(*batch, "provider_record", arrow::Type::BINARY));
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            if (ordinals->IsNull(row) || bytes->IsNull(row) || ordinals->Value(row) != result.size()) {
                return arrow::Status::Invalid("raw stage ordinal/byte association mismatch");
            }
            const auto value = bytes->GetView(row);
            std::vector<std::byte> payload(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) { payload[index] = static_cast<std::byte>(static_cast<unsigned char>(value[index])); }
            result.push_back({ordinals->Value(row), std::move(payload)});
        }
    }
    return result;
}
inline std::vector<integrity::BoundObservation> rows_of(const integrity::PreparedSegment& segment, const DataKind kind) {
    std::vector<integrity::BoundObservation> rows;
    for (const auto& row : segment.rows) { if (kind_of(row.event) == kind) { rows.push_back(row); } }
    return rows;
}
inline std::vector<DataKind> kinds_of(const integrity::PreparedSegment& segment) {
    std::vector<DataKind> kinds;
    for (const auto kind : {DataKind::Mbo, DataKind::Trades, DataKind::OrderExecution}) {
        if (!rows_of(segment, kind).empty()) { kinds.push_back(kind); }
    }
    if (kinds.empty()) { kinds.push_back(DataKind::Mbo); }
    return kinds;
}
inline DatasetLocalMetadata local_metadata(const integrity::PreparedSegment& segment, const DataKind kind) {
    return {kind, segment.binding.stream, segment.range, segment.acquisition.mode, std::string{supported_encoding},
        segment.adapter_version, segment.references.identity, {"validation.qvalidation"},
        {{"raw:" + segment.acquisition.acquisition_id, *segment.acquisition.query.event_time_range}}, std::nullopt};
}

inline arrow::Result<ArrowBatch> archival_consumer(
    const ArrowBatch& canonical, const std::span<const integrity::BoundObservation> rows,
    const integrity::Acquisition& acquisition) {
    arrow::BinaryBuilder witness;
    for (const auto& row : rows) {
        if (row.ordinal >= acquisition.records.size() || acquisition.records[row.ordinal].ordinal != row.ordinal) {
            return arrow::Status::Invalid("archival row has no original raw association");
        }
        const auto& bytes = acquisition.records[row.ordinal].bytes;
        ARROW_RETURN_NOT_OK(witness.Append(reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::int32_t>(bytes.size())));
    }
    std::shared_ptr<arrow::Array> array; ARROW_RETURN_NOT_OK(witness.Finish(&array));
    return canonical->AddColumn(canonical->num_columns(), arrow::field("raw_evidence", arrow::binary(), false), array);
}

} // namespace publication_detail

class ParquetStorageBackend final : public StorageBackend {
public:
    // The provider decoder is injected; concrete source types/SDK APIs stay outside storage.
    explicit ParquetStorageBackend(std::filesystem::path root, integrity::Decoder decoder = {})
        : root_(std::filesystem::weakly_canonical(root)), decoder_(std::move(decoder)) {}

    [[nodiscard]] arrow::Result<DataFragment> write(const DatasetLocalMetadata&, std::span<const ArrowBatch>) override {
        return arrow::Status::Invalid("unchecked/baseline writes are unsupported; prepare and publish a complete version");
    }

    [[nodiscard]] arrow::Result<PublishedVersion> publish(
        const integrity::PreparedSegment& supplied, const std::string& version,
        const integrity::ConsumerRequirements requirements = integrity::ConsumerRequirements::ordered_replay(),
        const PublicationFault& fault = {}) {
        auto result = publish_impl(supplied, version, requirements, fault);
        if (!result.ok()) {
            try {
                RetainedCapture capture{root_};
                const auto path = capture.path(supplied.acquisition);
                if (std::filesystem::exists(path)) {
                    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
                        const auto failure = path / ("publication-failure-" + std::to_string(attempt) + ".qfailure");
                        if (!std::filesystem::exists(failure)) {
                            files::write_text(failure, result.status().ToString()); files::sync(path); break;
                        }
                    }
                }
            } catch (const std::exception&) { /* A storage failure cannot authorize a COMPLETE marker. */ }
        }
        return result;
    }

private:
    [[nodiscard]] arrow::Result<PublishedVersion> publish_impl(
        const integrity::PreparedSegment& supplied, const std::string& version,
        const integrity::ConsumerRequirements requirements, const PublicationFault& fault) {
        using namespace publication_detail;
        try {
            integrity::validate_token(version);
            if (!decoder_) { return arrow::Status::Invalid("publication requires the registered preparation decoder"); }
            const auto version_path = root_ / "versions" / version;
            if (std::filesystem::exists(version_path / "COMPLETE")) { return arrow::Status::AlreadyExists("published version is immutable"); }
            RetainedCapture capture{root_}; capture.retain(supplied.acquisition);
            const auto retained = RetainedCapture::load(capture.path(supplied.acquisition));
            const auto prepared = integrity::prepare(retained, supplied.references, supplied.binding.id, supplied.range, decoder_, supplied.explicit_partial);
            const auto report_text = evidence::encode_report(prepared.validation);
            const auto stage = capture.path(retained);
            const auto report_path = stage / "validation.qvalidation";
            files::register_text(report_path, report_text);
            ARROW_RETURN_NOT_OK(stage_candidate(stage, prepared));
            if (prepared.binding != supplied.binding || prepared.adapter_version != supplied.adapter_version || prepared.transform_version != supplied.transform_version ||
                evidence::encode_report(supplied.validation) != report_text || supplied.rows.size() != prepared.rows.size()) {
                return arrow::Status::Invalid("detached/mismatched preparation evidence");
            }
            for (std::size_t index = 0; index < prepared.rows.size(); ++index) {
                if (supplied.rows[index].ordinal != prepared.rows[index].ordinal || supplied.rows[index].stream_id != prepared.rows[index].stream_id ||
                    !integrity::same_observation(supplied.rows[index].event, prepared.rows[index].event)) {
                    return arrow::Status::Invalid("canonical row is associated with the wrong retained input");
                }
            }
            ingestion::DefaultDataUsabilityPolicy usability;
            if ((prepared.explicit_partial && !requirements.explicit_partial) ||
                !integrity::eligible(prepared.validation, requirements, prepared.acquisition.query, usability)) {
                return arrow::Status::Invalid("candidate remains staged: consumer requirements are unresolved");
            }
            register_references(prepared.references);
            files::create_directories(version_path.parent_path());
            if (std::filesystem::exists(version_path)) {
                if (files::read(version_path / "OWNER") != prepared.acquisition.operation_id) {
                    return arrow::Status::Invalid("unfinished version belongs to another operation");
                }
            } else {
                std::filesystem::create_directory(version_path);
                files::write_text(version_path / "OWNER", prepared.acquisition.operation_id);
            }
            const auto attempt_path = version_path / (".attempt-" + prepared.acquisition.attempt_id);
            if (!std::filesystem::create_directory(attempt_path)) { return arrow::Status::AlreadyExists("publication attempt exists; use an explicit new attempt"); }
            Manifest manifest{version, prepared.acquisition.attempt_id, prepared.acquisition.acquisition_id,
                prepared.binding.id.value(), prepared.range, prepared.explicit_partial, requirements, {}};
            const auto checkpoint = [&](const PublicationPoint point) { if (fault) { fault(point); } };
            const auto add_file = [&](const std::string& path, const std::string& stage_name,
                                      const std::string& parent = {}, const std::string& parent_hash = {}) {
                manifest.files.push_back({path, files::sha256(files::read(attempt_path / path)), stage_name, parent, parent_hash});
            };
            ARROW_ASSIGN_OR_RAISE(const auto raw, raw_arrow(prepared.acquisition));
            ARROW_RETURN_NOT_OK(write_arrow(attempt_path / "raw.parquet", raw,
                arrow::KeyValueMetadata::Make({"quant.encoding", "quant.acquisition_id", "quant.stage"},
                    {"quant-provider-bytes-v1", manifest.acquisition, "raw"})));
            add_file("raw.parquet", "raw"); const auto raw_hash = manifest.files.back().digest; checkpoint(PublicationPoint::Raw);
            files::write_text(attempt_path / "references.qref", evidence::encode_references(prepared.references));
            add_file("references.qref", "references"); checkpoint(PublicationPoint::References);
            files::write_text(attempt_path / "acquisition.qacq", evidence::encode_acquisition(prepared.acquisition)); add_file("acquisition.qacq", "acquisition");
            files::write_text(attempt_path / "validation.qvalidation", report_text); add_file("validation.qvalidation", "validation", "raw.parquet", raw_hash);
            for (const auto kind : kinds_of(prepared)) {
                const auto rows = rows_of(prepared, kind);
                ARROW_ASSIGN_OR_RAISE(const auto batch, to_bound_arrow(kind, rows, prepared.binding, manifest.acquisition));
                const auto canonical_path = "canonical/" + std::string{data_kind_name(kind)} + ".parquet";
                auto canonical_metadata = parquet_detail::encode_metadata(local_metadata(prepared, kind));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.encoding", std::string{supported_encoding}));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.version_id", version));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.acquisition_id", manifest.acquisition));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.stream_id", std::to_string(manifest.stream)));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.stage", "canonical"));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.transform_version", prepared.adapter_version));
                ARROW_RETURN_NOT_OK(canonical_metadata->Set("quant.parent_sha256", raw_hash));
                ARROW_RETURN_NOT_OK(write_arrow(attempt_path / canonical_path, batch, canonical_metadata));
                add_file(canonical_path, "canonical", "raw.parquet", raw_hash); const auto canonical_hash = manifest.files.back().digest;
                checkpoint(PublicationPoint::Canonical);
                const auto consumer_path = "consumer/" + std::string{data_kind_name(kind)} + ".parquet";
                auto consumer_metadata = canonical_metadata->Copy();
                ARROW_RETURN_NOT_OK(consumer_metadata->Set("quant.stage", "consumer"));
                ARROW_RETURN_NOT_OK(consumer_metadata->Set("quant.transform_version", prepared.transform_version));
                ARROW_RETURN_NOT_OK(consumer_metadata->Set("quant.parent_sha256", canonical_hash));
                ARROW_ASSIGN_OR_RAISE(const auto archive, archival_consumer(batch, rows, prepared.acquisition));
                ARROW_RETURN_NOT_OK(write_arrow(attempt_path / consumer_path, archive, consumer_metadata));
                add_file(consumer_path, "consumer", canonical_path, canonical_hash); checkpoint(PublicationPoint::Consumer);
            }
            const auto manifest_text = encode_manifest(manifest);
            files::write_text(attempt_path / "manifest.qmanifest", manifest_text); checkpoint(PublicationPoint::Evidence);
            // Reopen/verify all required files, schemas and event associations BEFORE exposing COMPLETE.
            ARROW_RETURN_NOT_OK(verify_attempt(attempt_path, manifest, prepared));
            evidence::Writer marker; marker.text("quant-complete-v1"); marker.text(version); marker.text(manifest.attempt);
            marker.text(files::sha256(manifest_text));
            files::write_text(attempt_path / "complete.tmp", marker.str()); files::sync(attempt_path);
            files::sync(version_path); files::sync(version_path.parent_path()); checkpoint(PublicationPoint::BeforeComplete);
            try { files::publish_marker(attempt_path / "complete.tmp", version_path / "COMPLETE"); }
            catch (const std::exception&) { std::filesystem::remove(version_path / "COMPLETE"); throw; }
            return open_package(version_path);
        } catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
    }

public:
    [[nodiscard]] arrow::Result<PublishedVersion> open_version(const std::string& version) const {
        try { integrity::validate_token(version); }
        catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
        return open_package(root_ / "versions" / version);
    }
    [[nodiscard]] arrow::Result<PublishedVersion> open_package(const std::filesystem::path& path) const {
        ARROW_ASSIGN_OR_RAISE(const auto package, load_package(path));
        return describe(path, package.manifest, package.prepared, package.digest);
    }
    [[nodiscard]] arrow::Status select_current(const std::string& version, const std::function<void()>& before_switch = {}) {
        ARROW_ASSIGN_OR_RAISE(const auto published, open_version(version));
        try {
            evidence::Writer selection; selection.text("quant-selection-v1"); selection.text(version); selection.text(published.evidence_digest);
            auto temporary = root_ / (".current-" + version);
            for (unsigned index = 0; std::filesystem::exists(temporary); ++index) {
                if (index > 1000) { throw std::runtime_error("current selection attempt bound exceeded"); }
                temporary = root_ / (".current-" + version + "-" + std::to_string(index));
            }
            files::write_text(temporary, selection.str()); files::no_symlink(root_ / "current");
            if (before_switch) { before_switch(); }
            std::filesystem::rename(temporary, root_ / "current"); files::sync(root_);
            return arrow::Status::OK();
        } catch (const std::exception& error) { return arrow::Status::IOError(error.what()); }
    }
    [[nodiscard]] arrow::Result<PublishedVersion> current() const {
        try {
            evidence::Reader selection{files::read(root_ / "current")}; selection.expect("quant-selection-v1");
            const auto version = selection.text(); const auto hash = selection.text(); selection.finish();
            ARROW_ASSIGN_OR_RAISE(const auto published, open_version(version));
            if (published.evidence_digest != hash) { return arrow::Status::Invalid("current selection lost exact input identity"); }
            return published;
        } catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
    }
    [[nodiscard]] arrow::Result<DatasetLocalMetadata> read_metadata(const DataFragment& fragment) const override {
        ARROW_ASSIGN_OR_RAISE(const auto package, package_for(fragment));
        return publication_detail::local_metadata(package.prepared, fragment.data_kind);
    }
    arrow::Status read_batches(const DataFragment& fragment, const BatchProjection projection, const BatchConsumer& consumer) const override {
        return read_batches_checked(fragment, projection, {}, consumer);
    }
    arrow::Status read_batches_checked(const DataFragment& fragment, const BatchProjection projection,
        const integrity::ConsumerRequirements requirements, const BatchConsumer& consumer) const override {
        if (!consumer) { return arrow::Status::Invalid("empty batch consumer"); }
        ARROW_ASSIGN_OR_RAISE(const auto package, package_for(fragment));
        ARROW_RETURN_NOT_OK(check_eligibility(package.prepared, requirements));
        const auto& batches = package.consumer_batches.at(fragment.data_kind);
        for (const auto& batch : batches) {
            if (projection == BatchProjection::Full) { ARROW_RETURN_NOT_OK(consumer(batch)); }
            else {
                std::vector<int> indices;
                for (const auto& name : canonical_column_names(fragment.data_kind)) { indices.push_back(batch->schema()->GetFieldIndex(name)); }
                indices.push_back(batch->schema()->GetFieldIndex("stream_id")); indices.push_back(batch->schema()->GetFieldIndex("event_ordinal"));
                ARROW_ASSIGN_OR_RAISE(const auto replay, batch->SelectColumns(indices)); ARROW_RETURN_NOT_OK(consumer(replay));
            }
        }
        return arrow::Status::OK();
    }
    arrow::Status stream_events(const DataFragment& fragment, const EventConsumer& consumer) const override {
        if (!consumer) { return arrow::Status::Invalid("empty event consumer"); }
        const auto requirements = integrity::ConsumerRequirements::ordered_replay(fragment.data_kind == DataKind::Mbo);
        return stream_ordered_events(std::span<const DataFragment>{&fragment, 1}, requirements,
            [&](const integrity::BoundObservation& row) { return consumer(row.event); });
    }
    arrow::Status stream_ordered_events(const std::span<const DataFragment> fragments,
        integrity::ConsumerRequirements requirements, const SequencedEventConsumer& consumer) const override {
        if (fragments.empty() || !consumer) { return arrow::Status::Invalid("empty scoped replay request"); }
        requirements.ordering = true;
        requirements.groups = true;
        for (const auto& fragment : fragments) { requirements.initial_state = requirements.initial_state || fragment.data_kind == DataKind::Mbo; }
        ARROW_ASSIGN_OR_RAISE(const auto package, package_for(fragments.front()));
        ARROW_RETURN_NOT_OK(check_eligibility(package.prepared, requirements));
        std::set<DataKind> requested;
        for (const auto& fragment : fragments) {
            if (fragment.package_locator != fragments.front().package_locator || fragment.acquisition_id != fragments.front().acquisition_id ||
                fragment.evidence_digest != fragments.front().evidence_digest || !requested.insert(fragment.data_kind).second) {
                return arrow::Status::Invalid("independent acquisitions or duplicate partitions need an explicit merge contract");
            }
            const auto described = describe(fragment.package_locator, package.manifest, package.prepared, package.digest);
            if (std::find(described.fragments.begin(), described.fragments.end(), fragment) == described.fragments.end()) {
                return arrow::Status::Invalid("requested replay partition does not match the verified package");
            }
        }
        // The ledger was verified against every file before ANY callback. Its source succession
        // merges the requested kind partitions without manufacturing timestamp order.
        for (const auto& row : package.prepared.rows) { if (requested.contains(kind_of(row.event))) { ARROW_RETURN_NOT_OK(consumer(row)); } }
        return arrow::Status::OK();
    }

private:
    struct LoadedPackage {
        publication_detail::Manifest manifest;
        integrity::PreparedSegment prepared;
        std::string digest;
        std::map<DataKind, std::vector<ArrowBatch>> consumer_batches;
    };
    [[nodiscard]] arrow::Status stage_candidate(const std::filesystem::path& path,
                                                const integrity::PreparedSegment& prepared) const {
        using namespace publication_detail;
        try {
            files::register_text(path / "references.qref", evidence::encode_references(prepared.references));
            if (!std::filesystem::exists(path / "raw.parquet")) {
                ARROW_ASSIGN_OR_RAISE(const auto raw, raw_arrow(prepared.acquisition));
                ARROW_RETURN_NOT_OK(write_arrow(path / "raw.parquet", raw, arrow::KeyValueMetadata::Make(
                    {"quant.encoding", "quant.acquisition_id", "quant.stage"},
                    {"quant-provider-bytes-v1", prepared.acquisition.acquisition_id, "raw"})));
            }
            for (const auto kind : kinds_of(prepared)) {
                const auto file = path / "canonical" / (std::string{data_kind_name(kind)} + ".parquet");
                if (std::filesystem::exists(file)) { continue; }
                ARROW_ASSIGN_OR_RAISE(const auto batch, to_bound_arrow(kind, rows_of(prepared, kind), prepared.binding,
                    prepared.acquisition.acquisition_id));
                auto metadata = parquet_detail::encode_metadata(local_metadata(prepared, kind));
                ARROW_RETURN_NOT_OK(metadata->Set("quant.stage", "canonical_candidate"));
                ARROW_RETURN_NOT_OK(metadata->Set("quant.encoding", std::string{supported_encoding}));
                ARROW_RETURN_NOT_OK(write_arrow(file, batch, metadata));
            }
            return arrow::Status::OK();
        } catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
    }
    [[nodiscard]] arrow::Status check_eligibility(const integrity::PreparedSegment& segment,
                                                const integrity::ConsumerRequirements requirements) const {
        ingestion::DefaultDataUsabilityPolicy policy;
        if ((segment.explicit_partial && !requirements.explicit_partial) ||
            !integrity::eligible(segment.validation, requirements, segment.acquisition.query, policy)) {
            return arrow::Status::Invalid("consumer-required checks/partial selection unresolved");
        }
        return arrow::Status::OK();
    }
    void register_references(const integrity::ReferenceSnapshot& references) const {
        files::create_directories(root_ / "catalog");
        for (const auto& binding : references.catalog.entries()) {
            evidence::Writer entry; entry.text("quant-stream-binding-v1"); entry.number(binding.id.value()); evidence::put(entry, binding.stream);
            files::register_text(root_ / "catalog" / ("stream-" + std::to_string(binding.id.value()) + ".qref"), entry.str());
        }
        for (const auto& venue : references.venues) {
            evidence::Writer entry; entry.text("quant-venue-v1"); entry.number(venue.id.value()); entry.text(venue.code);
            files::register_text(root_ / "catalog" / ("venue-" + std::to_string(venue.id.value()) + ".qref"), entry.str());
        }
        for (const auto& source : references.sources) {
            evidence::Writer entry; entry.text("quant-source-v1"); entry.number(source.id.value());
            entry.text(source.provider); entry.text(source.dataset); entry.text(source.schema);
            files::register_text(root_ / "catalog" / ("source-" + std::to_string(source.id.value()) + ".qref"), entry.str());
        }
    }
    [[nodiscard]] arrow::Status verify_attempt(const std::filesystem::path& path,
        const publication_detail::Manifest& manifest, const integrity::PreparedSegment& prepared,
        std::map<DataKind, std::vector<ArrowBatch>>* verified_batches = nullptr) const {
        using namespace publication_detail;
        try {
            std::set<std::string> required{"raw.parquet", "references.qref", "acquisition.qacq", "validation.qvalidation"};
            for (const auto kind : kinds_of(prepared)) {
                required.insert("canonical/" + std::string{data_kind_name(kind)} + ".parquet");
                required.insert("consumer/" + std::string{data_kind_name(kind)} + ".parquet");
            }
            if (manifest.files.size() != required.size()) { return arrow::Status::Invalid("incomplete evidence inventory"); }
            for (const auto& file : manifest.files) {
                if (!required.erase(file.path) || files::sha256(files::read(path / file.path)) != file.digest) {
                    return arrow::Status::Invalid("required artifact missing/changed");
                }
                if (!file.parent.empty()) {
                    const auto parent = std::find_if(manifest.files.begin(), manifest.files.end(), [&](const auto& candidate) { return candidate.path == file.parent; });
                    if (parent == manifest.files.end() || parent->digest != file.parent_digest) { return arrow::Status::Invalid("unresolved exact stage parent"); }
                }
                const bool canonical = file.path.starts_with("canonical/");
                const bool consumer = file.path.starts_with("consumer/");
                const auto expected_stage = canonical ? "canonical" : consumer ? "consumer" :
                    file.path == "raw.parquet" ? "raw" : file.path == "references.qref" ? "references" :
                    file.path == "acquisition.qacq" ? "acquisition" : "validation";
                const auto expected_parent = canonical || file.path == "validation.qvalidation" ? "raw.parquet" :
                    consumer ? "canonical/" + file.path.substr(std::string{"consumer/"}.size()) : "";
                if (file.stage != expected_stage || file.parent != expected_parent ||
                    (expected_parent.empty() && !file.parent_digest.empty())) {
                    return arrow::Status::Invalid("incorrect stage/parent transformation contract");
                }
            }
            if (!required.empty() || evidence::encode_report(prepared.validation) != files::read(path / "validation.qvalidation")) {
                return arrow::Status::Invalid("incomplete/detached validation evidence");
            }
            for (const auto kind : kinds_of(prepared)) {
                const auto expected = rows_of(prepared, kind);
                for (const std::string stage : {"canonical", "consumer"}) {
                    const auto file = path / stage / (std::string{data_kind_name(kind)} + ".parquet");
                    ARROW_ASSIGN_OR_RAISE(auto reader, parquet_detail::open_reader(file.string()));
                    ARROW_ASSIGN_OR_RAISE(const auto local, parquet_detail::metadata_from_reader(*reader));
                    if (local != local_metadata(prepared, kind)) { return arrow::Status::Invalid("file metadata disagrees with original evidence"); }
                    const auto footer = reader->parquet_reader()->metadata()->key_value_metadata();
                    const auto relative = stage + "/" + std::string{data_kind_name(kind)} + ".parquet";
                    const auto evidence_entry = std::find_if(manifest.files.begin(), manifest.files.end(), [&](const auto& entry) { return entry.path == relative; });
                    if (evidence_entry == manifest.files.end()) { return arrow::Status::Invalid("untracked canonical/consumer output"); }
                    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
                        {"quant.encoding", std::string{supported_encoding}}, {"quant.version_id", manifest.version},
                        {"quant.acquisition_id", manifest.acquisition}, {"quant.stream_id", std::to_string(manifest.stream)}, {"quant.stage", stage},
                        {"quant.transform_version", stage == "canonical" ? prepared.adapter_version : prepared.transform_version},
                        {"quant.parent_sha256", evidence_entry->parent_digest}}) {
                        ARROW_ASSIGN_OR_RAISE(const auto actual, parquet_detail::required_value(*footer, key));
                        if (actual != value) { return arrow::Status::Invalid("unsupported/mismatched artifact contract: ", key); }
                    }
                    std::shared_ptr<arrow::Schema> schema; ARROW_RETURN_NOT_OK(reader->GetSchema(&schema));
                    ARROW_RETURN_NOT_OK(validate_bound_schema(*schema, kind));
                    if (stage == "consumer") {
                        const auto archive = schema->GetFieldIndex("raw_evidence");
                        if (archive < 0 || schema->field(archive)->type()->id() != arrow::Type::BINARY || schema->field(archive)->nullable()) {
                            return arrow::Status::Invalid("known archival consumer encoding lacks its evidence column");
                        }
                    }
                    ARROW_ASSIGN_OR_RAISE(const auto batches, read_arrow(file));
                    std::size_t index = 0;
                    for (const auto& batch : batches) {
                        ARROW_ASSIGN_OR_RAISE(const auto rows, bound_from_arrow(*batch, kind, prepared.binding));
                        std::shared_ptr<arrow::BinaryArray> witness;
                        if (stage == "consumer") { ARROW_ASSIGN_OR_RAISE(witness, detail::column<arrow::BinaryArray>(*batch, "raw_evidence", arrow::Type::BINARY)); }
                        int64_t batch_index = 0;
                        for (const auto& row : rows) {
                            if (index >= expected.size() || row.ordinal != expected[index].ordinal || row.stream_id != expected[index].stream_id ||
                                !integrity::same_observation(row.event, expected[index].event)) {
                                return arrow::Status::Invalid("persisted event differs from original ordinal association");
                            }
                            if (witness) {
                                const auto bytes = witness->GetView(batch_index);
                                const auto& original = prepared.acquisition.records.at(row.ordinal).bytes;
                                if (witness->IsNull(batch_index) || bytes.size() != original.size() ||
                                    !std::equal(original.begin(), original.end(), bytes.begin(), [](const std::byte lhs, const char rhs) {
                                        return std::to_integer<unsigned char>(lhs) == static_cast<unsigned char>(rhs);
                                    })) { return arrow::Status::Invalid("archival evidence is attached to the wrong event ordinal"); }
                            }
                            ++batch_index;
                            ++index;
                        }
                    }
                    if (index != expected.size()) { return arrow::Status::Invalid("persisted partition row count changed"); }
                    if (verified_batches && stage == "consumer") { (*verified_batches)[kind] = batches; }
                }
            }
            return check_eligibility(prepared, manifest.requirements);
        } catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
    }
    [[nodiscard]] arrow::Result<LoadedPackage> load_package(const std::filesystem::path& path) const {
        using namespace publication_detail;
        try {
            if (!decoder_) { return arrow::Status::Invalid("reader requires a registered preparation decoder"); }
            evidence::Reader marker{files::read(path / "COMPLETE")}; marker.expect("quant-complete-v1");
            const auto version = marker.text(); const auto attempt = marker.text(); const auto digest = marker.text(); marker.finish();
            integrity::validate_token(version); integrity::validate_token(attempt);
            const auto directory = path / (".attempt-" + attempt);
            const auto text = files::read(directory / "manifest.qmanifest");
            if (files::sha256(text) != digest) { return arrow::Status::Invalid("manifest changed after publication"); }
            const auto manifest = decode_manifest(text);
            if (manifest.version != version || manifest.attempt != attempt) { return arrow::Status::Invalid("completion identity conflict"); }
            // Validate the complete inventory/digests before parsing any required evidence or reading rows.
            for (const auto& file : manifest.files) {
                if (files::sha256(files::read(directory / file.path)) != file.digest) { return arrow::Status::Invalid("required evidence changed/missing"); }
            }
            const auto refs = evidence::decode_references(files::read(directory / "references.qref"));
            auto acquisition = evidence::decode_acquisition(files::read(directory / "acquisition.qacq"));
            if (acquisition.acquisition_id != manifest.acquisition || acquisition.attempt_id != manifest.attempt) {
                return arrow::Status::Invalid("acquisition/attempt evidence is misbound");
            }
            ARROW_ASSIGN_OR_RAISE(acquisition.records, read_raw(directory / "raw.parquet", manifest.acquisition));
            const auto prepared = integrity::prepare(acquisition, refs, data::StreamId{manifest.stream}, manifest.range, decoder_, manifest.partial);
            std::map<DataKind, std::vector<ArrowBatch>> verified;
            ARROW_RETURN_NOT_OK(verify_attempt(directory, manifest, prepared, &verified));
            return LoadedPackage{manifest, prepared, digest, std::move(verified)};
        } catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
    }
    [[nodiscard]] PublishedVersion describe(const std::filesystem::path& package_path,
        const publication_detail::Manifest& manifest, const integrity::PreparedSegment& prepared, const std::string& digest) const {
        PublishedVersion published{manifest.version, package_path.string(), digest, {}};
        for (const auto kind : publication_detail::kinds_of(prepared)) {
            auto fragment = fragment_from_metadata(publication_detail::local_metadata(prepared, kind),
                (package_path / (".attempt-" + manifest.attempt) / "consumer" / (std::string{data_kind_name(kind)} + ".parquet")).string());
            fragment.version_id = manifest.version; fragment.acquisition_id = manifest.acquisition;
            fragment.package_locator = package_path.string(); fragment.row_stream_id = prepared.binding.id; fragment.evidence_digest = digest;
            published.fragments.push_back(std::move(fragment));
        }
        return published;
    }
    [[nodiscard]] arrow::Result<LoadedPackage> package_for(const DataFragment& fragment) const {
        if (fragment.schema_version != supported_encoding || fragment.package_locator.empty() || fragment.evidence_digest.empty()) {
            return arrow::Status::Invalid("unsupported/unpinned baseline artifact");
        }
        ARROW_ASSIGN_OR_RAISE(const auto package, load_package(fragment.package_locator));
        const auto described = describe(fragment.package_locator, package.manifest, package.prepared, package.digest);
        if (std::find(described.fragments.begin(), described.fragments.end(), fragment) == described.fragments.end()) {
            return arrow::Status::Invalid("requested fragment disagrees with pinned package/version/reference evidence");
        }
        return package;
    }
    std::filesystem::path root_;
    integrity::Decoder decoder_;
};

} // namespace quant::storage
