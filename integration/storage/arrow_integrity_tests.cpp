#include "quant/storage/arrow_interchange.hpp"
#include "quant/storage/evidence_codec.hpp"
#include "quant/storage/file_integrity.hpp"
#include "integrity_fixture.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quant;
void require(const bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
arrow::Status tests() {
    const auto prepared = fixture::prepared();
    std::vector<integrity::BoundObservation> mbo;
    for (const auto& row : prepared.rows) { if (row.event.index() == 0) { mbo.push_back(row); } }
    ARROW_ASSIGN_OR_RAISE(const auto batch, storage::to_bound_arrow(storage::DataKind::Mbo, mbo,
        prepared.binding, prepared.acquisition.acquisition_id));
    ARROW_ASSIGN_OR_RAISE(const auto restored, storage::bound_from_arrow(*batch, storage::DataKind::Mbo, prepared.binding));
    require(restored.size() == mbo.size(), "bound row count changed");
    for (std::size_t index = 0; index < mbo.size(); ++index) {
        require(restored[index].ordinal == mbo[index].ordinal && integrity::same_observation(restored[index].event, mbo[index].event),
                "original ordinal/event association changed");
    }
    auto wrong = prepared.binding;
    wrong.stream.instrument_id = data::InstrumentId{99};
    require(!storage::to_bound_arrow(storage::DataKind::Mbo, mbo, wrong, prepared.acquisition.acquisition_id).ok(),
            "original MBO context was discarded before stream binding check");
    require(!storage::bound_from_arrow(*batch, storage::DataKind::Mbo, wrong).ok(),
            "persisted StreamId was relabeled with an inconsistent tuple");
    auto duplicate = mbo;
    duplicate[1].ordinal = duplicate[0].ordinal;
    require(!storage::to_bound_arrow(storage::DataKind::Mbo, duplicate, prepared.binding, prepared.acquisition.acquisition_id).ok(),
            "duplicate ordinal accepted");
    std::vector<data::Trade> mixed{
        std::get<data::Trade>(prepared.rows[2].event), std::get<data::Trade>(prepared.rows[2].event)};
    mixed[1].header.instrument_id = data::InstrumentId{99};
    require(!storage::to_arrow(std::span<const data::Trade>{mixed}).ok(), "mixed Trade input lost identity");
    auto zero = std::get<data::Trade>(prepared.rows[2].event);
    zero.quantity = data::Quantity::from_raw(0);
    require(!storage::to_arrow(std::span<const data::Trade>{&zero, 1}).ok(), "zero Trade bypassed codec validation");
    arrow::UInt32Builder corrupt_control;
    for (int64_t row = 0; row < batch->num_rows(); ++row) { ARROW_RETURN_NOT_OK(corrupt_control.Append(0xFFFF'FFFF)); }
    std::shared_ptr<arrow::Array> controls;
    ARROW_RETURN_NOT_OK(corrupt_control.Finish(&controls));
    ARROW_ASSIGN_OR_RAISE(const auto bad, batch->SetColumn(batch->schema()->GetFieldIndex("control"),
        arrow::field("control", arrow::uint32(), false), controls));
    require(!storage::bound_from_arrow(*bad, storage::DataKind::Mbo, prepared.binding).ok(), "malformed control accepted");

    const auto refs_text = storage::evidence::encode_references(prepared.references);
    const auto refs = storage::evidence::decode_references(refs_text);
    require(storage::evidence::encode_references(refs) == refs_text, "reference definitions did not round trip");
    const auto acquisition_text = storage::evidence::encode_acquisition(prepared.acquisition);
    const auto acquired = storage::evidence::decode_acquisition(acquisition_text);
    require(storage::evidence::encode_acquisition(acquired) == acquisition_text, "query/provider/recovery evidence changed");
    require(storage::files::sha256(std::string{}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "SHA-256 empty known-answer mismatch");
    require(storage::files::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 known-answer mismatch");
    require(storage::files::sha256(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
            "SHA-256 multi-block known-answer mismatch");
    return arrow::Status::OK();
}
} // namespace
int main() {
    try { const auto status = tests(); if (!status.ok()) { throw std::runtime_error(status.ToString()); }
        std::cout << "arrow_integrity_tests: passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
