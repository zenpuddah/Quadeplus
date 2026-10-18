#include "../integration/storage/integrity_fixture.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quant;
void require(const bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
template <typename Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "expected rejection");
}
bool can_replay(const integrity::PreparedSegment& segment, const bool book = true) {
    ingestion::DefaultDataUsabilityPolicy policy;
    return integrity::eligible(segment.validation, integrity::ConsumerRequirements::ordered_replay(book),
                               segment.acquisition.query, policy);
}
integrity::PreparedSegment prepare(const integrity::Acquisition& acquisition, const bool partial = false,
                                   const ingestion::TimeRange range = fixture::range(100, 200)) {
    return integrity::prepare(acquisition, fixture::references(), data::StreamId{1}, range,
                              ingestion::decode_xnas_record, partial);
}
void tests() {
    static_assert(sizeof(data::MboRecord) == 64 && alignof(data::MboRecord) == 64);
    auto refs = fixture::references();
    refs.validate();
    rejects([&] { refs.catalog.add({data::StreamId{1}, {data::InstrumentId{99}, data::VenueId{7}, data::SourceId{9}}}); });
    rejects([&] { static_cast<void>(refs.catalog.at(data::StreamId{99})); });
    refs.catalog.add({data::StreamId{2}, {data::InstrumentId{42}, data::VenueId{8}, data::SourceId{9}}});
    require(refs.catalog.at(data::StreamId{2}).stream.venue_id == data::VenueId{8}, "venue identity collapsed");
    refs = fixture::references();
    refs.sources.clear();
    rejects([&] { refs.validate(); });

    auto input = fixture::acquisition();
    auto result = prepare(input);
    require(can_replay(result), "complete snapshot plus T/F/C must be replayable");
    require(result.rows.size() == 5 && result.rows[2].event.index() == 1 && result.rows[3].event.index() == 2,
            "cross-kind event associations lost");
    require(result.rows[2].ordinal == 2 && result.rows[4].ordinal == 4, "shared ordinals changed");

    input.records[2] = fixture::record(2, 'T', 103, 8, 0, 0, 0);
    result = prepare(input);
    require(!can_replay(result) && !result.validation.failures.empty(), "zero Trade bypassed validation");
    require(result.acquisition.records[2].bytes == input.records[2].bytes, "failed raw bytes discarded");
    require(result.rows.size() == 4, "otherwise valid staged rows discarded");
    input.records[2] = fixture::record(2, 'T', 103, 8, 0, 0, 1);
    input.acquisition_id = "corrected-acquisition"; // Changed provider input is a NEW acquisition, not an in-place repair.
    input.attempt_id = "attempt-2";
    input.retry_of = "attempt-1";
    require(can_replay(prepare(input)), "explicit recovery was not revalidated");

    input = fixture::acquisition();
    input.records[3] = fixture::record(3, 'F', 103, 8, 0, 100, 0);
    require(!can_replay(prepare(input)), "zero execution accepted");
    input.records[3] = fixture::record(3, 'A', 103, 8, 0, 0, 1);
    require(!can_replay(prepare(input)), "invalid MBO shape accepted");

    input = fixture::acquisition();
    input.records.erase(input.records.begin());
    for (std::size_t index = 0; index < input.records.size(); ++index) { input.records[index].ordinal = index; }
    result = prepare(input);
    require(!can_replay(result) && can_replay(result, false), "missing initialization not separately gated");
    input.empty_book = integrity::EmptyBookEvidence{
        fixture::references().catalog.at(data::StreamId{1}).stream, fixture::time(100), "accepted synthetic empty session"};
    require(can_replay(prepare(input)), "explicit initialization ignored");

    input = fixture::acquisition();
    input.records.back() = fixture::record(4, 'C', 103, 8, 0, 100, 1);
    require(!can_replay(prepare(input)), "incomplete group accepted");
    input = fixture::acquisition();
    input.records.back() = fixture::record(4, 'C', 103, 8, 132, 100, 1);
    result = prepare(input);
    require(!can_replay(result), "Degraded acceptance bypassed unresolved channel ordering");
    input.records.back() = fixture::record(4, 'C', 103, 8, 136, 100, 1);
    require(can_replay(prepare(input)), "timestamp-quality flag declared book corruption");
    input = fixture::acquisition();
    input.quality.push_back({fixture::range(100, 200), ingestion::DataQualityKind::SequenceGap, std::nullopt});
    require(!can_replay(prepare(input)), "scoped SequenceGap evidence bypassed unresolved-order gating without a raw gap flag");

    input = fixture::acquisition();
    input.records.back().ordinal = 3;
    require(!can_replay(prepare(input)), "duplicate ordinal accepted");
    input = fixture::acquisition();
    std::swap(input.records[2], input.records[3]);
    require(!can_replay(prepare(input)), "reordered ordinal associations accepted");

    input = fixture::acquisition();
    input.records.push_back(fixture::record(5, 'A', 102, 9)); // ts_event non-monotonic is allowed; ts_recv must be valid.
    fixture::put<std::uint64_t>(input.records.back().bytes, 40, 105);
    require(can_replay(prepare(input)), "event time was used as ordering authority");
    fixture::put<std::uint64_t>(input.records.back().bytes, 40, 102);
    require(!can_replay(prepare(input)), "unflagged receive regression accepted");
    input.records.back() = fixture::record(5, 'A', 104, 1);
    require(!can_replay(prepare(input)), "unexplained session/counter reset accepted");

    input = fixture::acquisition();
    input.records = {fixture::record(0, 'R', 100, 0, 32, 0, 0, 0, 'N'),
                     fixture::record(1, 'A', 100, 90, 32), fixture::record(2, 'A', 100, 2, 160, 101)};
    require(can_replay(prepare(input)), "snapshot ordering confused with ordinary sequence order");
    input.records = {fixture::record(0, 'R', 100, 0, 160, 0, 0, 0, 'N'),
                     fixture::record(1, 'C', 101, 5, 0), fixture::record(2, 'A', 101, 5, 128, 101),
                     fixture::record(3, 'F', 102, 9, 0), fixture::record(4, 'C', 102, 9)};
    require(can_replay(prepare(input)), "replacement/non-printable group rejected");

    input = fixture::acquisition();
    input.records.push_back(fixture::record(5, 'A', 200, 10));
    result = prepare(input);
    require(result.rows.size() == 5 && result.validation.filtered_ordinals == std::vector<std::uint64_t>{5},
            "half-open end/legitimate overfetch not recorded correctly");
    result = prepare(input, false, fixture::range(300, 400));
    require(!can_replay(result), "mislabeled fragment range accepted");
    input.query.source_receive_time_range = fixture::range(100, 104);
    require(prepare(input).validation.filtered_ordinals.size() == 4, "dual-time filtering not enforced");
    fixture::put<std::uint64_t>(input.records[1].bytes, 40, std::numeric_limits<std::uint64_t>::max());
    require(!can_replay(prepare(input)), "missing requested time evidence was silently treated as normal overfetch");

    input = fixture::acquisition();
    input.quality = {{fixture::range(100, 150), ingestion::DataQualityKind::Complete, std::nullopt},
                     {fixture::range(150, 200), ingestion::DataQualityKind::Missing, std::nullopt}};
    require(!can_replay(prepare(input)), "retained rows implied full requested coverage");
    require(can_replay(prepare(input, true, fixture::range(100, 150))), "explicit independent usable partial range blocked");
    input.outcome = integrity::AcquisitionOutcome::Failed;
    input.operation_failure = "synthetic interrupted acquisition";
    require(!can_replay(prepare(input, true, fixture::range(100, 150))), "operation failure became eligible data");

    integrity::SourceOrderChecker checker;
    integrity::ValidationReport report;
    input = fixture::acquisition();
    const auto first_chunk = std::span<const integrity::RawRecord>{input.records}.first(3);
    const auto second_chunk = std::span<const integrity::RawRecord>{input.records}.subspan(3);
    for (const auto& row : first_chunk) { checker.observe(ingestion::decode_xnas_record(row, input), row.ordinal, true, report); }
    // T ends the first chunk; F/C close its normalization group in the next chunk.
    for (const auto& row : second_chunk) { checker.observe(ingestion::decode_xnas_record(row, input), row.ordinal, true, report); }
    checker.finish(report);
    require(report.groups == integrity::Check::Passed, "checker lost group state across chunks");
    require(fixture::prepared("acquisition-2").acquisition.acquisition_id != fixture::prepared().acquisition.acquisition_id,
            "independent ordinal scopes collapsed");
}
} // namespace

int main() {
    try { tests(); std::cout << "data_integrity_tests: passed\n"; }
    catch (const std::exception& error) { std::cerr << "data_integrity_tests: failed: " << error.what() << '\n'; return 1; }
}
