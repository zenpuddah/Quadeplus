#pragma once

#include "quant/storage/evidence_codec.hpp"
#include "quant/storage/file_integrity.hpp"

namespace quant::storage {

namespace capture_detail {
inline std::vector<std::byte> frame(const integrity::RawRecord& record) {
    if (record.bytes.size() > 1024 * 1024) { throw std::invalid_argument("provider record exceeds capture bound"); }
    std::vector<std::byte> bytes(12);
    for (unsigned index = 0; index < 8; ++index) { bytes[index] = static_cast<std::byte>((record.ordinal >> (index * 8)) & 255); }
    const auto size = static_cast<std::uint32_t>(record.bytes.size());
    for (unsigned index = 0; index < 4; ++index) { bytes[8 + index] = static_cast<std::byte>((size >> (index * 8)) & 255); }
    bytes.insert(bytes.end(), record.bytes.begin(), record.bytes.end()); return bytes;
}
inline std::vector<integrity::RawRecord> unframe(const std::string& bytes, const bool recover_prefix = false) {
    std::vector<integrity::RawRecord> records;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 12) {
            if (recover_prefix) { break; }
            throw std::invalid_argument("unfinished raw journal frame");
        }
        std::uint64_t ordinal = 0; std::uint32_t size = 0;
        for (unsigned index = 0; index < 8; ++index) { ordinal |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8); }
        for (unsigned index = 0; index < 4; ++index) { size |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 8 + index])) << (index * 8); }
        offset += 12;
        if (size > 1024 * 1024 || size > bytes.size() - offset || ordinal != records.size()) {
            if (recover_prefix) { break; }
            throw std::invalid_argument("truncated/misassociated raw journal record");
        }
        std::vector<std::byte> payload(size);
        for (std::size_t index = 0; index < size; ++index) { payload[index] = static_cast<std::byte>(static_cast<unsigned char>(bytes[offset + index])); }
        records.push_back({ordinal, std::move(payload)}); offset += size;
    }
    return records;
}
inline std::string input_identity(const integrity::Acquisition& acquisition) {
    auto identity = acquisition;
    identity.attempt_id = "input-identity"; identity.retry_of.clear();
    std::string encoded = evidence::encode_acquisition(identity);
    for (const auto& record : acquisition.records) {
        const auto bytes = frame(record);
        encoded.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return files::sha256(encoded);
}
} // namespace capture_detail

// Bounded evidence capture, not a cache or a replay backend.
class RetainedCapture final : public integrity::AcquisitionSink {
public:
    explicit RetainedCapture(std::filesystem::path root) : root_(std::filesystem::weakly_canonical(root)) {}
    [[nodiscard]] std::filesystem::path path(const integrity::Acquisition& acquisition) const {
        integrity::validate_token(acquisition.operation_id); integrity::validate_token(acquisition.acquisition_id);
        integrity::validate_token(acquisition.attempt_id);
        return root_ / "staging" / acquisition.operation_id / acquisition.attempt_id / acquisition.acquisition_id;
    }
    void begin(const integrity::Acquisition& request) override {
        const auto directory = path(request);
        files::create_directories(directory.parent_path());
        if (!std::filesystem::create_directory(directory)) { throw std::invalid_argument("capture attempt already exists"); }
        files::create_directories(root_ / "acquisitions");
        // Network acquisition identity is create-only, including failed attempts.
        files::write_text(root_ / "acquisitions" / (request.acquisition_id + ".request"), evidence::encode_acquisition(request));
        active_ = directory; ordinal_ = 0;
        files::write_text(active_ / "request.qacq", evidence::encode_acquisition(request));
        files::write_bytes(active_ / "raw.records", {});
        files::sync(active_); files::sync(active_.parent_path());
    }
    void metadata(const integrity::Acquisition& acquisition) override {
        require_active(acquisition); files::write_text(active_ / "provider.qacq", evidence::encode_acquisition(acquisition));
    }
    void record(const integrity::RawRecord& record) override {
        if (active_.empty() || record.ordinal != ordinal_) { throw std::invalid_argument("capture ordinal/scope mismatch"); }
        const auto bytes = capture_detail::frame(record);
        files::write_bytes(active_ / "raw.records", bytes, true); // fsync before acknowledging delivered bytes.
        ++ordinal_;
    }
    void finish(const integrity::Acquisition& acquisition) override {
        require_active(acquisition);
        if (acquisition.records.size() != ordinal_) { throw std::invalid_argument("captured rows differ from returned acquisition"); }
        files::write_text(active_ / "outcome.qacq", evidence::encode_acquisition(acquisition));
        files::register_text(root_ / "acquisitions" / (acquisition.acquisition_id + ".sha256"), capture_detail::input_identity(acquisition));
        files::sync(active_); active_.clear();
    }
    // Explicit retained-input reprocessing creates a fresh attempt without a provider call.
    void retain(const integrity::Acquisition& acquisition) {
        const auto directory = path(acquisition);
        files::create_directories(root_ / "acquisitions");
        files::register_text(root_ / "acquisitions" / (acquisition.acquisition_id + ".sha256"), capture_detail::input_identity(acquisition));
        if (std::filesystem::exists(directory)) {
            const auto existing = load(directory);
            if (capture_detail::input_identity(existing) != capture_detail::input_identity(acquisition) ||
                existing.attempt_id != acquisition.attempt_id) { throw std::invalid_argument("retained attempt identity conflict"); }
            return;
        }
        files::create_directories(directory);
        files::write_text(directory / "request.qacq", evidence::encode_acquisition(acquisition));
        files::write_text(directory / "provider.qacq", evidence::encode_acquisition(acquisition));
        files::write_bytes(directory / "raw.records", {});
        for (const auto& record : acquisition.records) { files::write_bytes(directory / "raw.records", capture_detail::frame(record), true); }
        files::write_text(directory / "outcome.qacq", evidence::encode_acquisition(acquisition)); files::sync(directory);
    }
    [[nodiscard]] static integrity::Acquisition load(const std::filesystem::path& directory) {
        auto acquisition = evidence::decode_acquisition(files::read(directory / "outcome.qacq"));
        acquisition.records = capture_detail::unframe(files::read(directory / "raw.records"));
        return acquisition;
    }
    [[nodiscard]] static integrity::Acquisition inspect_unfinished(const std::filesystem::path& directory) {
        const auto metadata = std::filesystem::exists(directory / "provider.qacq") ? "provider.qacq" : "request.qacq";
        auto acquisition = evidence::decode_acquisition(files::read(directory / metadata));
        acquisition.records = capture_detail::unframe(files::read(directory / "raw.records"), true);
        acquisition.outcome = integrity::AcquisitionOutcome::Failed;
        acquisition.operation_failure = "capture did not finish; only complete journal prefix records are recovered; original journal retained";
        return acquisition;
    }
private:
    void require_active(const integrity::Acquisition& acquisition) const {
        if (active_.empty() || active_ != path(acquisition)) { throw std::invalid_argument("capture scope changed"); }
    }
    std::filesystem::path root_;
    std::filesystem::path active_;
    std::uint64_t ordinal_ = 0;
};

} // namespace quant::storage
