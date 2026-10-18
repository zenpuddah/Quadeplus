#pragma once

#include "quant/data/value_types.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace quant::ingestion {

struct TimeRange {
    data::Timestamp start;
    data::Timestamp end;

    TimeRange(const data::Timestamp start_time, const data::Timestamp end_time)
        : start(start_time), end(end_time) {
        if (end <= start) {
            throw std::invalid_argument("time range end must be after start");
        }
    }

    friend bool operator==(const TimeRange&, const TimeRange&) = default;
};

struct TimeMargin {
    std::int64_t before_nanos;
    std::int64_t after_nanos;

    TimeMargin(const std::int64_t before, const std::int64_t after)
        : before_nanos(before), after_nanos(after) {
        if (before_nanos < 0 || after_nanos < 0) {
            throw std::invalid_argument("time derivation margin must not be negative");
        }
    }

    friend bool operator==(const TimeMargin&, const TimeMargin&) = default;
};

[[nodiscard]] inline bool is_valid(const TimeRange& range) noexcept {
    return range.start < range.end;
}

inline void validate(const TimeRange& range) {
    if (!is_valid(range)) {
        throw std::invalid_argument("time range end must be after start");
    }
}

enum class MarketDataLevel {
    L1,
    L2,
    L3,
};

enum class TimeBasis {
    EventTime,
    SourceReceiveTime,
};

enum class FetchPolicy {
    DeriveProviderRange,
    RequireExplicitRange,
};

struct MarketDataQuery;
inline void validate(const MarketDataQuery& query);

struct MarketDataQuery {
    data::InstrumentId instrument_id;
    std::optional<data::VenueId> venue_id;
    MarketDataLevel level;
    std::optional<TimeRange> event_time_range;
    std::optional<TimeRange> source_receive_time_range; // Provider/source receive time, not local arrival time.
    TimeBasis primary_time_basis;
    FetchPolicy fetch_policy;
    std::optional<TimeMargin> derivation_margin;

    MarketDataQuery(
        const data::InstrumentId instrument,
        std::optional<data::VenueId> venue,
        const MarketDataLevel data_level,
        const TimeRange event_range)
        : MarketDataQuery(
              instrument,
              std::move(venue),
              data_level,
              std::optional<TimeRange>{event_range},
              std::nullopt) {}

    MarketDataQuery(
        const data::InstrumentId instrument,
        std::optional<data::VenueId> venue,
        const MarketDataLevel data_level,
        std::optional<TimeRange> event_range,
        std::optional<TimeRange> source_receive_range,
        const TimeBasis primary_basis = TimeBasis::EventTime,
        const FetchPolicy provider_fetch_policy = FetchPolicy::DeriveProviderRange,
        std::optional<TimeMargin> margin = std::nullopt)
        : instrument_id(instrument),
          venue_id(std::move(venue)),
          level(data_level),
          event_time_range(std::move(event_range)),
          source_receive_time_range(std::move(source_receive_range)),
          primary_time_basis(primary_basis),
          fetch_policy(provider_fetch_policy),
          derivation_margin(std::move(margin)) {
        validate(*this);
    }

    [[nodiscard]] TimeBasis effective_primary_time_basis() const noexcept {
        if (primary_time_basis == TimeBasis::EventTime && event_time_range) {
            return TimeBasis::EventTime;
        }
        if (primary_time_basis == TimeBasis::SourceReceiveTime && source_receive_time_range) {
            return TimeBasis::SourceReceiveTime;
        }
        return event_time_range ? TimeBasis::EventTime : TimeBasis::SourceReceiveTime;
    }

    [[nodiscard]] const TimeRange& primary_range() const {
        if (effective_primary_time_basis() == TimeBasis::EventTime) {
            return *event_time_range;
        }
        return *source_receive_time_range;
    }
};

inline void validate(const MarketDataQuery& query) {
    if (!query.event_time_range && !query.source_receive_time_range) {
        throw std::invalid_argument("market-data query requires an event or source receive time range");
    }
    if (query.event_time_range) {
        validate(*query.event_time_range);
    }
    if (query.source_receive_time_range) {
        validate(*query.source_receive_time_range);
    }
    if (query.derivation_margin &&
        (query.derivation_margin->before_nanos < 0 || query.derivation_margin->after_nanos < 0)) {
        throw std::invalid_argument("time derivation margin must not be negative");
    }
}

[[nodiscard]] inline bool contains(const TimeRange& range, const data::Timestamp time) noexcept {
    return range.start <= time && time < range.end;
}

[[nodiscard]] inline TimeRange expand(const TimeRange range, const TimeMargin margin) {
    validate(range);
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (range.start.unix_nanos() < minimum + margin.before_nanos) {
        throw std::overflow_error("time derivation margin underflows the time range");
    }
    if (range.end.unix_nanos() > maximum - margin.after_nanos) {
        throw std::overflow_error("time derivation margin overflows the time range");
    }
    return TimeRange{
        data::Timestamp::from_unix_nanos(range.start.unix_nanos() - margin.before_nanos),
        data::Timestamp::from_unix_nanos(range.end.unix_nanos() + margin.after_nanos),
    };
}

[[nodiscard]] inline bool matches_time(
    const MarketDataQuery& query,
    const data::Timestamp event_time,
    const std::optional<data::Timestamp>& source_receive_time) noexcept {
    if (query.event_time_range && !contains(*query.event_time_range, event_time)) {
        return false;
    }
    if (query.source_receive_time_range &&
        (!source_receive_time || !contains(*query.source_receive_time_range, *source_receive_time))) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool overlaps(const TimeRange lhs, const TimeRange rhs) {
    validate(lhs);
    validate(rhs);
    return lhs.start < rhs.end && rhs.start < lhs.end;
}

[[nodiscard]] inline std::optional<TimeRange> intersection(const TimeRange lhs, const TimeRange rhs) {
    validate(lhs);
    validate(rhs);

    const auto start = std::max(lhs.start, rhs.start);
    const auto end = std::min(lhs.end, rhs.end);
    if (end <= start) {
        return std::nullopt;
    }
    return TimeRange{start, end};
}

[[nodiscard]] inline std::vector<TimeRange> subtract(
    const TimeRange requested,
    const std::span<const TimeRange> covered) {
    validate(requested);

    std::vector<TimeRange> clipped;
    clipped.reserve(covered.size());
    for (const auto& covered_range : covered) {
        validate(covered_range);
        if (const auto overlap = intersection(requested, covered_range)) {
            clipped.push_back(*overlap);
        }
    }

    std::sort(clipped.begin(), clipped.end(), [](const TimeRange& lhs, const TimeRange& rhs) {
        if (lhs.start != rhs.start) {
            return lhs.start < rhs.start;
        }
        return lhs.end < rhs.end;
    });

    std::vector<TimeRange> missing;
    auto cursor = requested.start;
    for (const auto& covered_range : clipped) {
        if (covered_range.end <= cursor) {
            continue;
        }
        if (cursor < covered_range.start) {
            missing.emplace_back(cursor, covered_range.start);
        }
        if (cursor < covered_range.end) {
            cursor = covered_range.end;
        }
        if (cursor >= requested.end) {
            break;
        }
    }

    if (cursor < requested.end) {
        missing.emplace_back(cursor, requested.end);
    }
    return missing;
}

} // namespace quant::ingestion
