#pragma once

#include "quant/data/observations.hpp"
#include "quant/ingestion/query.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

namespace quant::ingestion {

enum class DataQualityKind {
    Complete,
    Degraded,
    Pending,
    Missing,
    Corrupt,
    SequenceGap,
};

struct DataQualityObservation {
    TimeRange range;
    DataQualityKind kind;
    std::optional<data::MboStreamContext> scope = std::nullopt;
};

enum class DataState {
    Unknown,
    Complete,
    Degraded,
    Missing,
    Corrupt,
};

struct DataStateSegment {
    TimeRange range;
    DataState state;
    std::optional<data::MboStreamContext> scope = std::nullopt;
};

class DataUsabilityPolicy {
public:
    virtual ~DataUsabilityPolicy() = default;
    [[nodiscard]] virtual bool is_usable(
        const MarketDataQuery& query,
        const DataStateSegment& segment) const = 0;
};

class DefaultDataUsabilityPolicy final : public DataUsabilityPolicy {
public:
    [[nodiscard]] bool is_usable(
        const MarketDataQuery&,
        const DataStateSegment& segment) const override {
        return segment.state == DataState::Complete || segment.state == DataState::Degraded;
    }
};

[[nodiscard]] inline bool same_stream_scope(
    const std::optional<data::MboStreamContext>& lhs,
    const std::optional<data::MboStreamContext>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    return lhs->instrument_id == rhs->instrument_id && lhs->venue_id == rhs->venue_id &&
           lhs->source_id == rhs->source_id;
}

[[nodiscard]] inline bool operator==(const DataStateSegment& lhs, const DataStateSegment& rhs) noexcept {
    return lhs.range == rhs.range && lhs.state == rhs.state && same_stream_scope(lhs.scope, rhs.scope);
}

namespace detail {

[[nodiscard]] inline bool stream_scope_less(
    const std::optional<data::MboStreamContext>& lhs,
    const std::optional<data::MboStreamContext>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return !lhs.has_value();
    }
    if (!lhs) {
        return false;
    }
    if (lhs->instrument_id != rhs->instrument_id) {
        return lhs->instrument_id < rhs->instrument_id;
    }
    if (lhs->venue_id != rhs->venue_id) {
        return lhs->venue_id < rhs->venue_id;
    }
    return lhs->source_id < rhs->source_id;
}

[[nodiscard]] inline DataState state_for_quality(const DataQualityKind kind) noexcept {
    switch (kind) {
    case DataQualityKind::Complete:
        return DataState::Complete;
    case DataQualityKind::Degraded:
    case DataQualityKind::SequenceGap:
        return DataState::Degraded;
    case DataQualityKind::Pending:
        return DataState::Unknown;
    case DataQualityKind::Missing:
        return DataState::Missing;
    case DataQualityKind::Corrupt:
        return DataState::Corrupt;
    }
    return DataState::Unknown;
}

[[nodiscard]] inline int state_severity(const DataState state) noexcept {
    switch (state) {
    case DataState::Unknown:
        return 0;
    case DataState::Complete:
        return 1;
    case DataState::Degraded:
        return 2;
    case DataState::Missing:
        return 3;
    case DataState::Corrupt:
        return 4;
    }
    return 0;
}

[[nodiscard]] inline DataState reduce_interval(
    const TimeRange interval,
    const std::optional<data::MboStreamContext>& scope,
    const std::span<const DataQualityObservation> observations) noexcept {
    auto state = DataState::Unknown;
    for (const auto& observation : observations) {
        if (!same_stream_scope(observation.scope, scope)) {
            continue;
        }
        if (observation.range.start <= interval.start && interval.end <= observation.range.end) {
            const auto candidate = state_for_quality(observation.kind);
            if (state_severity(candidate) > state_severity(state)) {
                state = candidate;
            }
        }
    }
    return state;
}

} // namespace detail

class DataStateReducer {
public:
    [[nodiscard]] static std::vector<DataStateSegment> reduce(
        const TimeRange requested,
        const std::initializer_list<DataQualityObservation> observations,
        const std::span<const std::optional<data::MboStreamContext>> expected_scopes = {}) {
        return reduce(
            requested,
            std::span<const DataQualityObservation>{observations.begin(), observations.size()},
            expected_scopes);
    }

    [[nodiscard]] static std::vector<DataStateSegment> reduce(
        const TimeRange requested,
        const std::span<const DataQualityObservation> observations,
        const std::span<const std::optional<data::MboStreamContext>> expected_scopes = {}) {
        validate(requested);

        std::vector<std::optional<data::MboStreamContext>> scopes;
        scopes.reserve(observations.size() + expected_scopes.size());
        const auto add_scope = [&scopes](const std::optional<data::MboStreamContext>& scope) {
            if (std::find_if(scopes.begin(), scopes.end(), [&scope](const auto& existing) {
                    return same_stream_scope(existing, scope);
                }) == scopes.end()) {
                scopes.push_back(scope);
            }
        };

        for (const auto& scope : expected_scopes) {
            add_scope(scope);
        }

        for (const auto& observation : observations) {
            validate(observation.range);
            if (intersection(requested, observation.range)) {
                add_scope(observation.scope);
            }
        }

        if (scopes.empty()) {
            scopes.emplace_back(std::nullopt);
        }
        std::sort(scopes.begin(), scopes.end(), detail::stream_scope_less);

        std::vector<DataStateSegment> segments;
        for (const auto& scope : scopes) {
            std::vector<data::Timestamp> boundaries{requested.start, requested.end};
            for (const auto& observation : observations) {
                if (!same_stream_scope(observation.scope, scope)) {
                    continue;
                }
                if (const auto clipped = intersection(requested, observation.range)) {
                    boundaries.push_back(clipped->start);
                    boundaries.push_back(clipped->end);
                }
            }
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

            for (std::size_t index = 1; index < boundaries.size(); ++index) {
                const TimeRange interval{boundaries[index - 1], boundaries[index]};
                const auto state = detail::reduce_interval(interval, scope, observations);
                if (!segments.empty() && same_stream_scope(segments.back().scope, scope) &&
                    segments.back().state == state && segments.back().range.end == interval.start) {
                    segments.back().range.end = interval.end;
                } else {
                    segments.push_back(DataStateSegment{interval, state, scope});
                }
            }
        }
        return segments;
    }

    [[nodiscard]] std::vector<DataStateSegment> operator()(
        const TimeRange requested,
        const std::span<const DataQualityObservation> observations,
        const std::span<const std::optional<data::MboStreamContext>> expected_scopes = {}) const {
        return reduce(requested, observations, expected_scopes);
    }
};

[[nodiscard]] inline std::vector<DataStateSegment> reduce_data_state(
    const TimeRange requested,
    const std::span<const DataQualityObservation> observations,
    const std::span<const std::optional<data::MboStreamContext>> expected_scopes = {}) {
    return DataStateReducer::reduce(requested, observations, expected_scopes);
}

[[nodiscard]] inline std::vector<DataStateSegment> reduce_data_state(
    const TimeRange requested,
    const std::initializer_list<DataQualityObservation> observations,
    const std::span<const std::optional<data::MboStreamContext>> expected_scopes = {}) {
    return DataStateReducer::reduce(requested, observations, expected_scopes);
}

[[nodiscard]] inline const DataStateSegment* lookup_data_state(
    const std::span<const DataStateSegment> segments,
    const std::optional<data::MboStreamContext>& scope,
    const data::Timestamp time) noexcept {
    const auto first = std::lower_bound(
        segments.begin(),
        segments.end(),
        scope,
        [](const DataStateSegment& segment, const auto& target_scope) {
            return detail::stream_scope_less(segment.scope, target_scope);
        });
    const auto last = std::upper_bound(
        first,
        segments.end(),
        scope,
        [](const auto& target_scope, const DataStateSegment& segment) {
            return detail::stream_scope_less(target_scope, segment.scope);
        });
    const auto candidate = std::upper_bound(
        first,
        last,
        time,
        [](const data::Timestamp lookup_time, const DataStateSegment& segment) {
            return lookup_time < segment.range.start;
        });
    if (candidate == first) {
        return nullptr;
    }
    const auto& segment = *(candidate - 1);
    return segment.range.start <= time && time < segment.range.end ? &segment : nullptr;
}

[[nodiscard]] inline std::vector<DataStateSegment> lookup_data_state(
    const std::span<const DataStateSegment> segments,
    const std::optional<data::MboStreamContext>& scope,
    const TimeRange requested) {
    validate(requested);
    const auto first = std::lower_bound(
        segments.begin(),
        segments.end(),
        scope,
        [](const DataStateSegment& segment, const auto& target_scope) {
            return detail::stream_scope_less(segment.scope, target_scope);
        });
    const auto last = std::upper_bound(
        first,
        segments.end(),
        scope,
        [](const auto& target_scope, const DataStateSegment& segment) {
            return detail::stream_scope_less(target_scope, segment.scope);
        });

    auto iterator = std::lower_bound(
        first,
        last,
        requested.start,
        [](const DataStateSegment& segment, const data::Timestamp time) {
            return segment.range.end <= time;
        });
    if (iterator != first && (iterator - 1)->range.end > requested.start) {
        --iterator;
    }

    std::vector<DataStateSegment> result;
    for (; iterator != last && iterator->range.start < requested.end; ++iterator) {
        if (const auto clipped = intersection(requested, iterator->range)) {
            result.push_back(DataStateSegment{*clipped, iterator->state, iterator->scope});
        }
    }
    return result;
}

[[nodiscard]] inline DataState summarize_data_state(const std::span<const DataStateSegment> segments) noexcept {
    auto summary = DataState::Unknown;
    auto severity = 0;
    for (const auto& segment : segments) {
        int candidate_severity = 0;
        switch (segment.state) {
        case DataState::Complete:
            candidate_severity = 1;
            break;
        case DataState::Unknown:
            candidate_severity = 2;
            break;
        case DataState::Degraded:
            candidate_severity = 3;
            break;
        case DataState::Missing:
            candidate_severity = 4;
            break;
        case DataState::Corrupt:
            candidate_severity = 5;
            break;
        }
        if (candidate_severity > severity) {
            severity = candidate_severity;
            summary = segment.state;
        }
    }
    return summary;
}

} // namespace quant::ingestion
