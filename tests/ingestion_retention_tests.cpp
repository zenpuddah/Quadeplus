#include "quant/ingestion/synchronous_ingestor.hpp"
#include "../integration/storage/integrity_fixture.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quant;
void require(const bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
struct RecordingSink final : integrity::AcquisitionSink {
    std::vector<integrity::RawRecord> rows;
    bool begun = false;
    bool metadata_retained = false;
    bool finished = false;
    void begin(const integrity::Acquisition&) override { begun = true; }
    void metadata(const integrity::Acquisition& acquisition) override {
        require(begun && !acquisition.provider_metadata.empty(), "metadata must follow operation start");
        metadata_retained = true;
    }
    void record(const integrity::RawRecord& row) override {
        require(metadata_retained && row.ordinal == rows.size(), "capture lost input succession");
        rows.push_back(row);
    }
    void finish(const integrity::Acquisition&) override { finished = true; }
};
void tests() {
    auto raw = fixture::acquisition();
    raw.records.push_back(fixture::record(5, 'A', 200, 12)); // Legitimate overfetch is retained before selection.
    ingestion::NoopCache cache;
    ingestion::InMemoryInstrumentMappingRegistry mappings;
    mappings.add(fixture::mapping());
    ingestion::FakeProvider provider{"databento"};
    provider.enqueue_acquisition(raw);
    ingestion::NoopRecoveryPolicy recovery;
    ingestion::DefaultDataUsabilityPolicy usability;
    ingestion::SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
    RecordingSink sink;
    const auto acquisitions = ingestor.acquire_retained(raw.query, fixture::time(500), "operation-test", "acquisition-test", "attempt-1", sink);
    require(acquisitions.size() == 1 && sink.rows == raw.records && sink.finished, "raw capture was filtered or lost");
    require(acquisitions.front().acquisition_id == "acquisition-test-0", "ordering scope is not explicit per request");
    const auto prepared = integrity::prepare(acquisitions.front(), fixture::references(), data::StreamId{1},
        fixture::range(100, 200), ingestion::decode_xnas_record);
    require(prepared.rows.size() == 5 && prepared.validation.filtered_ordinals == std::vector<std::uint64_t>{5},
            "selection must follow capture/ordinal assignment");
    raw.outcome = integrity::AcquisitionOutcome::Failed;
    raw.operation_failure = "synthetic provider transport interruption";
    provider.enqueue_acquisition(raw);
    RecordingSink failed_sink;
    const auto failed = ingestor.acquire_retained(raw.query, fixture::time(600), "operation-failed", "acquisition-failed", "attempt-1", failed_sink);
    require(failed_sink.rows == raw.records && failed.front().operation_failure == raw.operation_failure,
            "operation failure discarded fetched evidence");
    const auto failed_preparation = integrity::prepare(failed.front(), fixture::references(), data::StreamId{1},
        fixture::range(100, 200), ingestion::decode_xnas_record);
    require(!integrity::eligible(failed_preparation.validation, {}, raw.query, usability), "failed acquisition bypassed eligibility");
}
} // namespace
int main() {
    try { tests(); std::cout << "ingestion_retention_tests: passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
