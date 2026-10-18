#pragma once

#include "quant/ingestion/query.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quant::ingestion {

struct ProviderMappingSegment {
    data::InstrumentId instrument_id;
    std::string provider;
    std::optional<std::string> symbol;
    std::optional<data::VenueId> venue_id;
    std::optional<std::string> provider_id;
    TimeRange range; // Provider-identity validity is expressed in event time.
    std::optional<data::SourceId> source_id = std::nullopt;
};

inline void validate(const ProviderMappingSegment& segment) {
    validate(segment.range);
    if (segment.provider.empty()) {
        throw std::invalid_argument("provider mapping name must not be empty");
    }
    if (segment.symbol && segment.symbol->empty()) {
        throw std::invalid_argument("provider mapping symbol must not be empty");
    }
    if (segment.provider_id && segment.provider_id->empty()) {
        throw std::invalid_argument("provider mapping provider id must not be empty");
    }
}

class InstrumentMappingRegistry {
public:
    virtual ~InstrumentMappingRegistry() = default;
    virtual std::vector<ProviderMappingSegment> resolve(
        data::InstrumentId instrument_id,
        std::string_view provider,
        TimeRange event_time_range,
        std::optional<data::VenueId> venue_id = std::nullopt) const = 0;
};

class InMemoryInstrumentMappingRegistry final : public InstrumentMappingRegistry {
public:
    void add(ProviderMappingSegment mapping) {
        validate(mapping);
        for (const auto& existing : mappings_) {
            if (same_scope(existing, mapping) && overlaps(existing.range, mapping.range)) {
                throw std::invalid_argument("provider mapping intervals must not overlap");
            }
        }
        mappings_.push_back(std::move(mapping));
    }

    [[nodiscard]] std::vector<ProviderMappingSegment> resolve(
        const data::InstrumentId instrument_id,
        const std::string_view provider,
        const TimeRange event_time_range,
        const std::optional<data::VenueId> venue_id = std::nullopt) const override {
        validate(event_time_range);
        if (provider.empty()) {
            throw std::invalid_argument("provider mapping name must not be empty");
        }

        std::vector<ProviderMappingSegment> resolved;
        for (const auto& mapping : mappings_) {
            if (mapping.instrument_id != instrument_id || mapping.provider != provider) {
                continue;
            }
            if (venue_id && mapping.venue_id != venue_id) {
                continue;
            }
            if (const auto overlap = intersection(mapping.range, event_time_range)) {
                resolved.push_back(ProviderMappingSegment{
                    mapping.instrument_id,
                    mapping.provider,
                    mapping.symbol,
                    mapping.venue_id,
                    mapping.provider_id,
                    *overlap,
                    mapping.source_id,
                });
            }
        }

        std::sort(resolved.begin(), resolved.end(), [](const ProviderMappingSegment& lhs, const ProviderMappingSegment& rhs) {
            if (lhs.range.start != rhs.range.start) {
                return lhs.range.start < rhs.range.start;
            }
            if (lhs.range.end != rhs.range.end) {
                return lhs.range.end < rhs.range.end;
            }
            if (lhs.venue_id != rhs.venue_id) {
                return lhs.venue_id < rhs.venue_id;
            }
            if (lhs.source_id != rhs.source_id) {
                return lhs.source_id < rhs.source_id;
            }
            return lhs.provider_id < rhs.provider_id;
        });
        return resolved;
    }

private:
    [[nodiscard]] static bool same_scope(
        const ProviderMappingSegment& lhs,
        const ProviderMappingSegment& rhs) noexcept {
        return lhs.instrument_id == rhs.instrument_id && lhs.provider == rhs.provider &&
               lhs.venue_id == rhs.venue_id && lhs.source_id == rhs.source_id;
    }

    std::vector<ProviderMappingSegment> mappings_;
};

} // namespace quant::ingestion
