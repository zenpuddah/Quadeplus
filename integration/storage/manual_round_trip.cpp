#include "quant/storage/parquet_backend.hpp"
#include "quant/ingestion/synchronous_ingestor.hpp"
#include "integrity_fixture.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quant;
using namespace quant::storage;
void require(const bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }

const char* action_name(const data::MboAction action) {
    switch (action) {
    case data::MboAction::Add: return "Add";
    case data::MboAction::Modify: return "Modify";
    case data::MboAction::Cancel: return "Cancel";
    case data::MboAction::Execute: return "Execute";
    case data::MboAction::Clear: return "Clear";
    }
    return "Unknown";
}

arrow::Status run(const std::filesystem::path& output) {
    const auto root = std::filesystem::weakly_canonical(output);
    std::cout << "canonical_record_bytes=" << sizeof(data::MboRecord) << '\n';
    ingestion::NoopCache cache;
    ingestion::InMemoryInstrumentMappingRegistry mappings;
    mappings.add(fixture::mapping());
    ingestion::FakeProvider provider{"databento"};
    provider.enqueue_acquisition(fixture::acquisition());
    ingestion::NoopRecoveryPolicy recovery;
    ingestion::DefaultDataUsabilityPolicy usability;
    ingestion::SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
    RetainedCapture capture{root};
    const auto acquired = ingestor.acquire_retained(fixture::acquisition().query, fixture::time(500),
        "demo-operation", "demo-good", "attempt-1", capture);
    require(acquired.size() == 1, "unexpected acquisition splitting");
    const auto requests = provider.retained_requests();
    const auto retained = RetainedCapture::load(capture.path(acquired.front()));
    const auto prepared = integrity::prepare(retained, fixture::references(), data::StreamId{1},
        fixture::range(100, 200), ingestion::decode_xnas_record);
    require(provider.retained_requests() == requests && prepared.rows.size() == 5, "retained reprocessing refetched or lost rows");
    std::cout << "captured acquisition=" << retained.acquisition_id << " raw_rows=" << retained.records.size() << '\n';
    std::cout << "reprocess_without_provider=true requests=" << requests << '\n';

    ParquetStorageBackend backend{root, ingestion::decode_xnas_record};
    ARROW_ASSIGN_OR_RAISE(const auto first, backend.publish(prepared, "demo-v1"));
    ARROW_RETURN_NOT_OK(backend.select_current("demo-v1"));
    std::cout << "version1=" << first.package_locator << '\n';
    std::cout << "encoding=" << supported_encoding << " stream=1 tuple=42/7/9 range=[100,200) partitions=" << first.fragments.size() << '\n';
    const MarketDataReader& reader = backend;
    std::size_t replay_index = 0;
    ARROW_RETURN_NOT_OK(reader.stream_ordered_events(first.fragments, integrity::ConsumerRequirements::ordered_replay(),
        [&](const integrity::BoundObservation& row) {
            require(row.ordinal == replay_index, "shared acquisition ordinal changed");
            std::cout << "replay[" << replay_index << "] ordinal=" << row.ordinal << " stream=" << row.stream_id.value();
            std::visit([](const auto& event) {
                using T = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<T, data::MboEvent>) {
                    std::cout << ' ' << action_name(event.action);
                    if (event.price) { std::cout << " price_raw=" << event.price->raw(); }
                } else if constexpr (std::is_same_v<T, data::Trade>) {
                    std::cout << " Trade price_raw=" << event.price.raw() << " quantity_raw=" << event.quantity.raw();
                } else {
                    std::cout << " OrderExecution order=" << event.resting_order_id.value() << " quantity_raw=" << event.executed_quantity.raw();
                }
                std::cout << " t=" << event.header.event_time.unix_nanos() << " seq=" << *event.header.sequence << '\n';
            }, row.event);
            ++replay_index; return arrow::Status::OK();
        }));
    require(replay_index == 5, "cross-kind replay is incomplete");

    bool archived = false;
    bool projected = false;
    ARROW_RETURN_NOT_OK(reader.read_batches(first.fragments.front(), BatchProjection::Full, [&](const ArrowBatch& batch) {
        archived = batch->GetColumnByName("raw_evidence") != nullptr; return arrow::Status::OK(); }));
    ARROW_RETURN_NOT_OK(reader.read_batches(first.fragments.front(), BatchProjection::Replay, [&](const ArrowBatch& batch) {
        projected = batch->GetColumnByName("raw_evidence") == nullptr; return arrow::Status::OK(); }));
    require(archived && projected, "archive/replay projection failed");
    std::cout << "full_has_raw_evidence=true replay_excludes_raw_evidence=true\n";

    auto bad = fixture::acquisition(); bad.records[2] = fixture::record(2, 'T', 103, 8, 0, 0, 0);
    provider.enqueue_acquisition(bad);
    const auto bad_acquired = ingestor.acquire_retained(bad.query, fixture::time(600), "demo-operation", "demo-failed", "attempt-1", capture);
    const auto failed = integrity::prepare(RetainedCapture::load(capture.path(bad_acquired.front())), fixture::references(),
        data::StreamId{1}, fixture::range(100, 200), ingestion::decode_xnas_record);
    require(!backend.publish(failed, "demo-failed-version").ok(), "failed candidate published");
    require(std::any_of(failed.validation.failures.begin(), failed.validation.failures.end(), [](const auto& failure) {
        return failure.ordinal == 2 && failure.rule == "zero_trade_quantity";
    }), "expected row-level financial failure not exercised");
    require(std::filesystem::exists(capture.path(bad_acquired.front()) / "validation.qvalidation"), "failure evidence was not retained");
    std::cout << "failed_candidate=staged failed_ordinal=2 rule=zero_trade_quantity\n";
    std::cout << "failed_stage=" << capture.path(bad_acquired.front()).string() << '\n';

    auto correction = fixture::acquisition(); fixture::put<std::int64_t>(correction.records[1].bytes, 24, 101'000'000'000);
    provider.enqueue_acquisition(correction);
    const auto corrected_acquired = ingestor.acquire_retained(correction.query, fixture::time(700),
        "demo-operation", "demo-corrected", "attempt-1", capture);
    const auto corrected = integrity::prepare(RetainedCapture::load(capture.path(corrected_acquired.front())), fixture::references(),
        data::StreamId{1}, fixture::range(100, 200), ingestion::decode_xnas_record);
    ARROW_ASSIGN_OR_RAISE(const auto second, backend.publish(corrected, "demo-v2"));
    ARROW_RETURN_NOT_OK(backend.select_current("demo-v2"));
    std::int64_t old_price = 0;
    ARROW_RETURN_NOT_OK(reader.stream_events(first.fragments.front(), [&](const CanonicalEvent& event) {
        const auto& mbo = std::get<data::MboEvent>(event);
        if (mbo.action == data::MboAction::Add) { old_price = mbo.price->raw(); }
        return arrow::Status::OK(); }));
    require(old_price == 100'000'000'000, "correction changed old published input");
    ARROW_ASSIGN_OR_RAISE(const auto current, backend.current());
    require(current.version_id == "demo-v2", "current selection did not change after completion");
    std::cout << "version2=" << second.package_locator << '\n';
    std::cout << "current=demo-v2 old_version_add_price_raw=" << old_price << '\n';
    std::cout << "manual_round_trip: passed\n";
    return arrow::Status::OK();
}
} // namespace

int main(const int argc, char** argv) {
    if (argc > 2) { std::cerr << "Usage: quant_storage_manual_round_trip [fresh-output-root]\n"; return 1; }
    try {
        const auto root = argc == 2 ? std::filesystem::path{argv[1]} : std::filesystem::canonical(std::filesystem::temp_directory_path()) /
            ("quant-integrity-demo-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto status = run(root);
        if (!status.ok()) { throw std::runtime_error(status.ToString()); }
    } catch (const std::exception& error) { std::cerr << "manual_round_trip: failed: " << error.what() << '\n'; return 1; }
}
