#include "quant/storage/parquet_backend.hpp"
#include "integrity_fixture.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quant;
using namespace quant::storage;
void require(const bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }

arrow::Status test_versions_and_replay(const std::filesystem::path& root) {
    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    const auto prepared = fixture::prepared();
    ARROW_ASSIGN_OR_RAISE(const auto first, backend.publish(prepared, "version-1"));
    require(first.fragments.size() == 3, "cross-kind partition publication incomplete");
    bool full_evidence = false;
    bool replay_evidence = false;
    ARROW_RETURN_NOT_OK(backend.read_batches(first.fragments.front(), BatchProjection::Full, [&](const ArrowBatch& batch) {
        full_evidence = batch->GetColumnByName("raw_evidence") != nullptr; return arrow::Status::OK(); }));
    ARROW_RETURN_NOT_OK(backend.read_batches(first.fragments.front(), BatchProjection::Replay, [&](const ArrowBatch& batch) {
        replay_evidence = batch->GetColumnByName("raw_evidence") != nullptr; return arrow::Status::OK(); }));
    require(full_evidence && !replay_evidence, "rich archive versus replay projection contract failed");
    ARROW_RETURN_NOT_OK(backend.select_current("version-1"));
    ARROW_ASSIGN_OR_RAISE(const auto selected, backend.current());
    require(selected.version_id == "version-1", "current selection did not resolve an exact version");
    require(!backend.publish(prepared, "version-1").ok(), "existing published destination was replaced");

    auto corrected_acquisition = fixture::acquisition("acquisition-corrected");
    fixture::put<std::int64_t>(corrected_acquisition.records[1].bytes, 24, 101'000'000'000);
    const auto corrected = integrity::prepare(corrected_acquisition, fixture::references(), data::StreamId{1},
        fixture::range(100, 200), ingestion::decode_xnas_record);
    ARROW_ASSIGN_OR_RAISE(const auto second, backend.publish(corrected, "version-2"));
    require(!backend.select_current("version-2", [] { throw std::runtime_error("selection interruption"); }).ok(),
            "selection interruption not exercised");
    ARROW_ASSIGN_OR_RAISE(const auto unchanged, backend.current());
    require(unchanged.version_id == "version-1", "failed selection changed old run input");
    ARROW_RETURN_NOT_OK(backend.select_current("version-2"));
    ARROW_ASSIGN_OR_RAISE(const auto changed, backend.current());
    require(changed.version_id == "version-2", "current correction not selected");

    for (const auto* version : {&first, &second}) {
        std::vector<integrity::BoundObservation> replay;
        ARROW_RETURN_NOT_OK(backend.stream_ordered_events(version->fragments, integrity::ConsumerRequirements::ordered_replay(),
            [&](const integrity::BoundObservation& row) { replay.push_back(row); return arrow::Status::OK(); }));
        require(replay.size() == 5 && replay[2].event.index() == 1 && replay[3].event.index() == 2 && replay[4].event.index() == 0,
                "Trade/Fill/Cancel association was lost after partitioning");
        for (std::size_t index = 0; index < replay.size(); ++index) { require(replay[index].ordinal == index, "ordinal succession changed"); }
        const auto expected_price = version == &first ? 100'000'000'000LL : 101'000'000'000LL;
        require(std::get<data::MboEvent>(replay[1].event).price->raw() == expected_price, "old fragment changed after correction");
        ARROW_ASSIGN_OR_RAISE(const auto metadata, backend.read_metadata(version->fragments.front()));
        require(metadata.stream.instrument_id == data::InstrumentId{42} && metadata.schema_version == supported_encoding,
                "stream/encoding metadata changed");
    }
    std::vector<DataFragment> mixed{first.fragments.front(), second.fragments[1]};
    std::size_t delivered = 0;
    require(!backend.stream_ordered_events(mixed, integrity::ConsumerRequirements::ordered_replay(),
        [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok() && delivered == 0,
            "independent acquisition ordinals were merged");
    return arrow::Status::OK();
}

arrow::Status test_invalid_candidates(const std::filesystem::path& root) {
    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    auto invalid = fixture::acquisition("bad-quantity");
    invalid.records[2] = fixture::record(2, 'T', 103, 8, 0, 0, 0);
    const auto prepared = integrity::prepare(invalid, fixture::references(), data::StreamId{1}, fixture::range(100, 200), ingestion::decode_xnas_record);
    require(!backend.publish(prepared, "bad-version").ok(), "invalid data published");
    RetainedCapture capture{root};
    const auto path = capture.path(invalid);
    const auto retained = RetainedCapture::load(path);
    require(retained.records == invalid.records, "failed candidate lost original provider bytes");
    require(std::filesystem::exists(path / "validation.qvalidation") && std::filesystem::exists(path / "canonical/mbo.parquet"),
            "failed candidate report/valid dependent rows not retained in staging");
    require(!backend.open_version("bad-version").ok(), "unresolved candidate available to consumers");

    auto interrupted_capture = fixture::acquisition("interrupted-capture");
    capture.begin(interrupted_capture); capture.metadata(interrupted_capture); capture.record(interrupted_capture.records.front());
    const auto capture_path = capture.path(interrupted_capture);
    files::write_bytes(capture_path / "raw.records", std::as_bytes(std::span{"tail", std::size_t{4}}), true);
    const auto prefix = RetainedCapture::inspect_unfinished(capture_path);
    require(prefix.records.size() == 1 && prefix.records.front() == interrupted_capture.records.front() &&
            prefix.outcome == integrity::AcquisitionOutcome::Failed, "interrupted acquisition lost its acknowledged byte prefix");
    const auto prefix_candidate = integrity::prepare(prefix, fixture::references(), data::StreamId{1}, fixture::range(100, 200), ingestion::decode_xnas_record);
    require(!backend.publish(prefix_candidate, "unfinished-capture-version").ok(), "unfinished capture became eligible");

    auto decode_failure = fixture::acquisition("decode-failure");
    fixture::put<std::uint64_t>(decode_failure.records[1].bytes, 8, std::numeric_limits<std::uint64_t>::max());
    const auto failed_decode = integrity::prepare(decode_failure, fixture::references(), data::StreamId{1}, fixture::range(100, 200), ingestion::decode_xnas_record);
    require(!backend.publish(failed_decode, "decode-failure-version").ok(), "unconstructible canonical timestamp published");
    require(RetainedCapture::load(capture.path(decode_failure)).records == decode_failure.records,
            "unconstructible primitive value was discarded instead of retained raw");

    auto edited = fixture::prepared("misassociated");
    edited.rows[1].ordinal = edited.rows[2].ordinal;
    require(!backend.publish(edited, "wrong-association").ok(), "wrong ordinal/event association passed publication");
    auto wrong_event = fixture::prepared("sorted-wrong-event");
    wrong_event.rows[1].event = wrong_event.rows[4].event; // Ordinals stay unique/sorted; original events do not.
    require(!backend.publish(wrong_event, "sorted-wrong-event-version").ok(), "sorted ordinals hid a wrong original event association");
    edited = fixture::prepared("wrong-scope");
    edited.binding.stream.instrument_id = data::InstrumentId{99};
    require(!backend.publish(edited, "wrong-scope-version").ok(), "wrong file scope was silently corrected/published");
    auto mislabeled = integrity::prepare(fixture::acquisition("wrong-range"), fixture::references(), data::StreamId{1},
        fixture::range(300, 400), ingestion::decode_xnas_record);
    require(!backend.publish(mislabeled, "wrong-range-version").ok(), "mislabeled event range published");

    auto missing = fixture::prepared("missing-reference"); missing.references.sources.clear();
    require(!backend.publish(missing, "missing-reference-version").ok(), "missing references bypassed publication");
    auto detached = fixture::prepared("detached-report"); detached.validation.ordering = integrity::Check::Unresolved;
    require(!backend.publish(detached, "detached-report-version").ok(), "detached caller validation was trusted");

    const auto good = fixture::prepared("identity-stability");
    ARROW_ASSIGN_OR_RAISE(const auto published, backend.publish(good, "stable-version"));
    static_cast<void>(published);
    auto changed = fixture::prepared("changed-source-definition"); changed.references.sources.front().dataset = "OTHER";
    require(!backend.publish(changed, "changed-source-version").ok(), "catalog reference definition reassigned");
    auto changed_input = fixture::prepared("identity-stability");
    fixture::put<std::int64_t>(changed_input.acquisition.records[1].bytes, 24, 102'000'000'000);
    changed_input.acquisition.attempt_id = "attempt-2";
    require(!backend.publish(changed_input, "changed-input-version").ok(), "new acquisition reused old raw identity");
    return arrow::Status::OK();
}

arrow::Status test_self_consistent_but_wrong_witness(const std::filesystem::path& root) {
    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    const auto prepared = fixture::prepared();
    ARROW_ASSIGN_OR_RAISE(const auto published, backend.publish(prepared, "witness-version"));
    const auto attempt = std::filesystem::path{published.package_locator} / ".attempt-attempt-1";
    const auto file = attempt / "consumer/mbo.parquet";
    ARROW_ASSIGN_OR_RAISE(const auto batches, publication_detail::read_arrow(file));
    arrow::BinaryBuilder altered;
    for (const auto ordinal : {std::uint64_t{0}, std::uint64_t{4}, std::uint64_t{4}}) {
        const auto& bytes = prepared.acquisition.records[ordinal].bytes;
        ARROW_RETURN_NOT_OK(altered.Append(reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::int32_t>(bytes.size())));
    }
    std::shared_ptr<arrow::Array> witness; ARROW_RETURN_NOT_OK(altered.Finish(&witness));
    ARROW_ASSIGN_OR_RAISE(const auto wrong, batches.front()->SetColumn(batches.front()->schema()->GetFieldIndex("raw_evidence"),
        arrow::field("raw_evidence", arrow::binary(), false), witness));
    // Test-only mutation of our temporary fixture: make file/manifest checksums internally
    // consistent so the regression specifically exercises original event association.
    const auto metadata = batches.front()->schema()->metadata()->Copy();
    std::filesystem::remove(file);
    ARROW_RETURN_NOT_OK(publication_detail::write_arrow(file, wrong, metadata));
    auto manifest = publication_detail::decode_manifest(files::read(attempt / "manifest.qmanifest"));
    for (auto& entry : manifest.files) { if (entry.path == "consumer/mbo.parquet") { entry.digest = files::sha256(files::read(file)); } }
    const auto text = publication_detail::encode_manifest(manifest);
    std::filesystem::remove(attempt / "manifest.qmanifest"); files::write_text(attempt / "manifest.qmanifest", text);
    std::filesystem::remove(std::filesystem::path{published.package_locator} / "COMPLETE");
    evidence::Writer marker; marker.text("quant-complete-v1"); marker.text("witness-version"); marker.text("attempt-1"); marker.text(files::sha256(text));
    files::write_text(std::filesystem::path{published.package_locator} / "COMPLETE", marker.str());
    std::size_t delivered = 0;
    auto fragment = published.fragments.front(); fragment.evidence_digest = files::sha256(text);
    require(!backend.stream_events(fragment, [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok() && delivered == 0,
            "compatible columns and consistent hashes hid wrong raw/event association");
    return arrow::Status::OK();
}

arrow::Status test_interrupted_publication(const std::filesystem::path& root) {
    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    unsigned index = 0;
    for (const auto point : {PublicationPoint::Raw, PublicationPoint::References, PublicationPoint::Canonical,
                            PublicationPoint::Consumer, PublicationPoint::Evidence, PublicationPoint::BeforeComplete}) {
        const auto name = "interrupted-" + std::to_string(index++);
        auto prepared = fixture::prepared(name);
        const auto result = backend.publish(prepared, name, integrity::ConsumerRequirements::ordered_replay(),
            [point](const PublicationPoint actual) { if (actual == point) { throw std::runtime_error("injected publication interruption"); } });
        require(!result.ok() && !std::filesystem::exists(root / "versions" / name / "COMPLETE") && !backend.open_version(name).ok(),
                "interrupted data/reference/evidence write exposed an eligible version");
        prepared.acquisition.attempt_id = "attempt-2";
        prepared.acquisition.retry_of = "attempt-1";
        ARROW_ASSIGN_OR_RAISE(const auto resumed, backend.publish(prepared, name));
        require(resumed.version_id == name, "unfinished staging was not explicitly retryable");
    }
    return arrow::Status::OK();
}

arrow::Status test_eligibility_and_partial(const std::filesystem::path& root) {
    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    auto bad_book = fixture::acquisition("gap-evidence");
    bad_book.records.back() = fixture::record(4, 'C', 103, 8, 132, 100, 1);
    const auto gap = integrity::prepare(bad_book, fixture::references(), data::StreamId{1}, fixture::range(100, 200), ingestion::decode_xnas_record);
    require(!backend.publish(gap, "gap-book").ok(), "Degraded default bypassed book/order gate");
    // A deliberately order-independent consumer may retain a structurally valid analytical view.
    ARROW_ASSIGN_OR_RAISE(const auto analytic, backend.publish(gap, "gap-analytic", {}));
    std::size_t delivered = 0;
    require(!backend.stream_events(analytic.fragments.front(), [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok() && delivered == 0,
            "published analytics bypassed ordered consumer checks");
    require(!backend.stream_ordered_events(analytic.fragments, {}, [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok() && delivered == 0,
            "weakened caller requirements bypassed mandatory replay checks");

    auto partial_input = fixture::acquisition("partial-evidence");
    partial_input.quality = {{fixture::range(100, 150), ingestion::DataQualityKind::Complete, std::nullopt},
                            {fixture::range(150, 200), ingestion::DataQualityKind::Missing, std::nullopt}};
    const auto partial = integrity::prepare(partial_input, fixture::references(), data::StreamId{1}, fixture::range(100, 150),
        ingestion::decode_xnas_record, true);
    require(!backend.publish(partial, "implicit-partial").ok(), "partial range silently claimed full coverage");
    auto requirements = integrity::ConsumerRequirements::ordered_replay(); requirements.explicit_partial = true;
    ARROW_ASSIGN_OR_RAISE(const auto explicit_version, backend.publish(partial, "explicit-partial", requirements));
    require(!backend.stream_events(explicit_version.fragments.front(), [&](const auto&) { return arrow::Status::OK(); }).ok(),
            "reader did not require explicit partial consumption");
    ARROW_RETURN_NOT_OK(backend.stream_ordered_events(explicit_version.fragments, requirements,
        [&](const auto&) { return arrow::Status::OK(); }));
    return arrow::Status::OK();
}

arrow::Status test_reader_failure_before_delivery(const std::filesystem::path& root) {
    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    ARROW_ASSIGN_OR_RAISE(const auto published, backend.publish(fixture::prepared(), "reader-version"));
    auto unknown = published.fragments.front(); unknown.schema_version = "canonical-v999";
    std::size_t delivered = 0;
    require(!backend.stream_events(unknown, [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok() && delivered == 0,
            "unsupported encoding delivered rows");
    auto incorrect_label = published.fragments.front(); incorrect_label.stream.instrument_id = data::InstrumentId{99};
    require(!backend.read_batches(incorrect_label, BatchProjection::Full, [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok(),
            "requested metadata label overrode original identity");
    const auto version_path = std::filesystem::path{published.package_locator};
    const auto attempt = version_path / ".attempt-attempt-1";
    // A required reference component changed after publication; no callback may see a prefix of rows.
    files::write_bytes(attempt / "references.qref", std::as_bytes(std::span{"changed", std::size_t{7}}), true);
    require(!backend.stream_events(published.fragments.front(), [&](const auto&) { ++delivered; return arrow::Status::OK(); }).ok() && delivered == 0,
            "changed reference snapshot did not fail before delivery");

    ARROW_ASSIGN_OR_RAISE(const auto movable, backend.publish(fixture::prepared("movable-input"), "movable-version"));
    const auto moved = root / "moved-version";
    std::filesystem::copy(movable.package_locator, moved, std::filesystem::copy_options::recursive);
    ARROW_ASSIGN_OR_RAISE(const auto reopened, backend.open_package(moved));
    ARROW_RETURN_NOT_OK(backend.stream_events(reopened.fragments.front(), [&](const auto&) { return arrow::Status::OK(); }));
    std::filesystem::remove(moved / ".attempt-attempt-1" / "references.qref");
    require(!backend.open_package(moved).ok(), "moved artifact missing references remained available");
    return arrow::Status::OK();
}

arrow::Status run_tests() {
    const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto root = parent / ("quant-integrity-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    ARROW_RETURN_NOT_OK(test_versions_and_replay(root / "versions-test"));
    ARROW_RETURN_NOT_OK(test_invalid_candidates(root / "invalid-test"));
    ARROW_RETURN_NOT_OK(test_interrupted_publication(root / "interruptions-test"));
    ARROW_RETURN_NOT_OK(test_eligibility_and_partial(root / "eligibility-test"));
    ARROW_RETURN_NOT_OK(test_reader_failure_before_delivery(root / "reader-test"));
    ARROW_RETURN_NOT_OK(test_self_consistent_but_wrong_witness(root / "association-test"));
    std::filesystem::remove_all(root);
    return arrow::Status::OK();
}
} // namespace
int main() {
    try { const auto status = run_tests(); if (!status.ok()) { throw std::runtime_error(status.ToString()); }
        std::cout << "storage_tests: passed\n"; }
    catch (const std::exception& error) { std::cerr << "storage_tests: failed: " << error.what() << '\n'; return 1; }
}
