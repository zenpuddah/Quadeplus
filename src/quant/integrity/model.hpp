#pragma once

#include "quant/data/reference.hpp"
#include "quant/data/validation.hpp"
#include "quant/ingestion/query_plan.hpp"
#include "quant/ingestion/result.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <variant>

namespace quant::integrity {

using Observation = std::variant<data::MboEvent, data::Trade, data::OrderExecution>;

struct StreamIdentity {
    data::InstrumentId instrument_id;
    data::VenueId venue_id;
    data::SourceId source_id;
    friend bool operator==(const StreamIdentity&, const StreamIdentity&) = default;
};

struct StreamBinding {
    data::StreamId id;
    StreamIdentity stream;
    friend bool operator==(const StreamBinding&, const StreamBinding&) = default;
};

inline void validate_token(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        throw std::invalid_argument("identity token must contain 1..128 safe ASCII characters");
    }
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            throw std::invalid_argument("identity token contains an unsafe character");
        }
    }
}

class StreamCatalog {
public:
    void add(const StreamBinding binding) {
        for (const auto& entry : entries_) {
            if (entry.id == binding.id) {
                if (entry.stream != binding.stream) {
                    throw std::invalid_argument("StreamId reassignment is forbidden");
                }
                return;
            }
            if (entry.stream == binding.stream) {
                throw std::invalid_argument("stream tuple already has a different StreamId");
            }
        }
        entries_.push_back(binding);
    }
    [[nodiscard]] const StreamBinding& at(const data::StreamId id) const {
        for (const auto& entry : entries_) {
            if (entry.id == id) { return entry; }
        }
        throw std::invalid_argument("unknown StreamId");
    }
    [[nodiscard]] const StreamBinding& resolve(const StreamIdentity stream) const {
        for (const auto& entry : entries_) {
            if (entry.stream == stream) { return entry; }
        }
        throw std::invalid_argument("unresolved stream tuple");
    }
    [[nodiscard]] const std::vector<StreamBinding>& entries() const noexcept { return entries_; }
private:
    std::vector<StreamBinding> entries_;
};

struct ReferenceSnapshot {
    std::string identity;
    StreamCatalog catalog;
    std::vector<data::ReferenceVersion> instruments;
    std::vector<data::VenueReference> venues;
    std::vector<data::SourceMetadata> sources;
    std::vector<ingestion::ProviderMappingSegment> mappings;

    void validate() const {
        validate_token(identity);
        if (catalog.entries().empty()) { throw std::invalid_argument("empty reference catalog"); }
        data::VenueReferenceTable venue_table;
        for (const auto& venue : venues) { venue_table.add(venue); }
        data::SourceMetadataTable source_table;
        for (const auto& source : sources) {
            if (source.dataset.empty() || source.schema.empty()) {
                throw std::invalid_argument("source definition must retain dataset and schema");
            }
            source_table.add(source);
        }
        for (const auto& binding : catalog.entries()) {
            if (!venue_table.find(binding.stream.venue_id) || !source_table.find(binding.stream.source_id)) {
                throw std::invalid_argument("missing retained venue/source definition");
            }
            data::ReferenceHistory history{binding.stream.instrument_id};
            for (const auto& instrument : instruments) {
                if (instrument.instrument_id == binding.stream.instrument_id) { history.append(instrument); }
            }
            if (history.size() == 0) { throw std::invalid_argument("missing instrument definition"); }
            bool mapped = false;
            for (const auto& mapping : mappings) {
                ingestion::validate(mapping);
                if (mapping.instrument_id == binding.stream.instrument_id &&
                    mapping.venue_id == binding.stream.venue_id && mapping.source_id == binding.stream.source_id) {
                    const auto* source = source_table.find(binding.stream.source_id);
                    if (source->provider != mapping.provider || (!mapping.symbol && !mapping.provider_id)) {
                        throw std::invalid_argument("inconsistent source/mapping definition");
                    }
                    mapped = true;
                }
            }
            if (!mapped) { throw std::invalid_argument("missing retained mapping snapshot"); }
        }
    }

    void validate_header(const data::EventHeader& header) const {
        static_cast<void>(catalog.resolve({header.instrument_id, header.venue_id, header.source_id}));
        data::ReferenceHistory history{header.instrument_id};
        for (const auto& instrument : instruments) {
            if (instrument.instrument_id == header.instrument_id) { history.append(instrument); }
        }
        if (!history.at(header.event_time)) { throw std::invalid_argument("reference unavailable at event time"); }
        for (const auto& mapping : mappings) {
            if (mapping.instrument_id == header.instrument_id && mapping.venue_id == header.venue_id &&
                mapping.source_id == header.source_id && ingestion::contains(mapping.range, header.event_time)) {
                return;
            }
        }
        throw std::invalid_argument("mapping unavailable at event time");
    }
};

enum class Check { Unresolved, Passed, Failed };
enum class AcquisitionOutcome { Complete, Failed };

struct RawRecord {
    std::uint64_t ordinal;
    std::vector<std::byte> bytes;
    friend bool operator==(const RawRecord&, const RawRecord&) = default;
};

struct NativeBinding {
    std::uint32_t instrument_id;
    ingestion::TimeRange range;
};

struct EmptyBookEvidence {
    StreamIdentity stream;
    data::Timestamp time;
    std::string reason;
};

struct Acquisition {
    std::string operation_id;
    std::string acquisition_id;
    std::string attempt_id;
    data::Timestamp acquired_at;
    ingestion::MarketDataQuery query;
    ingestion::ProviderMappingSegment mapping;
    ingestion::ProviderFetchRange provider_request;
    std::string provider_encoding = "dbn-mbo-56-le-v1";
    std::string provider_metadata{};
    std::vector<NativeBinding> native_bindings{};
    std::vector<ingestion::ProviderBatchMetadata> provider_batches{};
    std::vector<ingestion::DataQualityObservation> quality{};
    std::vector<RawRecord> records{};
    AcquisitionOutcome outcome = AcquisitionOutcome::Complete;
    std::string operation_failure{};
    std::string retry_of{};
    std::optional<EmptyBookEvidence> empty_book{};
    ingestion::AcquisitionMode mode = ingestion::AcquisitionMode::Historical;
};

class AcquisitionSink {
public:
    virtual ~AcquisitionSink() = default;
    virtual void begin(const Acquisition& request) = 0;
    virtual void metadata(const Acquisition& acquisition) = 0;
    virtual void record(const RawRecord& record) = 0;
    virtual void finish(const Acquisition& acquisition) = 0;
};

struct RowFailure {
    std::uint64_t ordinal;
    std::string rule;
    std::string reason;
    friend bool operator==(const RowFailure&, const RowFailure&) = default;
};

struct BoundObservation {
    data::StreamId stream_id;
    std::uint64_t ordinal;
    Observation event;
};

[[nodiscard]] inline const data::EventHeader& header(const Observation& observation) {
    return std::visit([](const auto& event) -> const data::EventHeader& { return event.header; }, observation);
}

[[nodiscard]] inline bool same_observation(const Observation& lhs, const Observation& rhs) {
    if (lhs.index() != rhs.index() || header(lhs) != header(rhs)) { return false; }
    return std::visit([&rhs](const auto& left) {
        using T = std::decay_t<decltype(left)>;
        const auto& right = std::get<T>(rhs);
        if constexpr (std::is_same_v<T, data::MboEvent>) {
            return left.action == right.action && left.order_id == right.order_id && left.side == right.side &&
                   left.price == right.price && left.quantity == right.quantity;
        } else if constexpr (std::is_same_v<T, data::Trade>) {
            return left.price == right.price && left.quantity == right.quantity && left.aggressor_side == right.aggressor_side;
        } else {
            return left.resting_order_id == right.resting_order_id && left.executed_quantity == right.executed_quantity &&
                   left.price == right.price;
        }
    }, lhs);
}

struct ValidationReport {
    Check structural = Check::Passed;
    Check ordering = Check::Passed;
    Check groups = Check::Passed;
    Check initial_state = Check::Unresolved;
    std::vector<RowFailure> failures;
    std::vector<std::uint64_t> filtered_ordinals;
    std::vector<ingestion::DataStateSegment> data_states;
    std::vector<std::string> assumptions;
};

struct ConsumerRequirements {
    bool ordering = false;
    bool groups = false;
    bool initial_state = false;
    bool allow_degraded = true;
    bool explicit_partial = false;
    [[nodiscard]] static ConsumerRequirements ordered_replay(const bool book = true) {
        return {true, true, book, true, false};
    }
};

[[nodiscard]] inline bool eligible(
    const ValidationReport& report, const ConsumerRequirements requirements,
    const ingestion::MarketDataQuery& query, const ingestion::DataUsabilityPolicy& usability) {
    if (report.structural != Check::Passed || (requirements.ordering && report.ordering != Check::Passed) ||
        (requirements.groups && report.groups != Check::Passed) ||
        (requirements.initial_state && report.initial_state != Check::Passed) || report.data_states.empty()) { return false; }
    for (const auto& segment : report.data_states) {
        if (!usability.is_usable(query, segment) || (!requirements.allow_degraded && segment.state != ingestion::DataState::Complete)) {
            return false;
        }
    }
    return true;
}

struct DecodedRecord {
    std::optional<data::EventHeader> header;
    std::optional<Observation> event;
    std::uint32_t native_instrument = 0;
    bool snapshot = false;
    bool last = false;
    bool bad_receive_time = false;
    bool bad_book = false;
    std::string error;
};

using Decoder = std::function<DecodedRecord(const RawRecord&, const Acquisition&)>;

struct PreparedSegment {
    Acquisition acquisition;
    ReferenceSnapshot references;
    StreamBinding binding;
    ingestion::TimeRange range;
    ValidationReport validation;
    std::vector<BoundObservation> rows;
    std::string adapter_version = "xnas-canonical-v2";
    std::string transform_version = "archival-consumer-v1";
    bool explicit_partial = false;
};

} // namespace quant::integrity
