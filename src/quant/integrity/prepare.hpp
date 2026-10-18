#pragma once

#include "quant/integrity/model.hpp"

#include <map>
#include <tuple>

namespace quant::integrity {

// One checker instance is deliberately shared by all chunks of one acquisition.
class SourceOrderChecker {
public:
    struct Group {
        bool open = false;
        bool all_selected = true;
        bool snapshot = false;
        bool snapshot_clear = false;
        std::uint64_t sequence = 0;
        std::optional<data::Timestamp> receive;
        std::optional<std::uint64_t> previous_sequence;
        bool selected_before = false;
    };

    void observe(const DecodedRecord& row, const std::uint64_t ordinal, const bool selected,
                 ValidationReport& report) {
        if (!row.header || !row.header->channel_id || !row.header->sequence) {
            report.ordering = Check::Unresolved;
            report.failures.push_back({ordinal, "ordering_evidence", "required sequence/channel/header is unavailable"});
            return;
        }
        const auto key = std::pair{row.native_instrument, *row.header->channel_id};
        auto& state = states_[key];
        const auto sequence = *row.header->sequence;
        if (!row.snapshot) {
            if (state.previous_sequence && sequence < *state.previous_sequence) {
                report.ordering = Check::Unresolved;
                report.failures.push_back({ordinal, "sequence_reset", "counter regression has no retained session/reset evidence"});
            }
            if (!row.bad_receive_time && row.header->source_receive_time) {
                if (state.receive && *row.header->source_receive_time < *state.receive) {
                    report.ordering = Check::Failed;
                    report.failures.push_back({ordinal, "receive_order", "unflagged ordinary receive time regressed"});
                }
                state.receive = row.header->source_receive_time;
            } else if (!row.bad_receive_time) {
                report.ordering = Check::Unresolved;
            }
            state.previous_sequence = sequence;
        }
        if (state.open && ((!row.snapshot && sequence != state.sequence) || row.snapshot != state.snapshot)) {
            report.groups = Check::Unresolved;
            report.failures.push_back({ordinal, "incomplete_group", "event changed before its F_LAST boundary"});
            state.open = false;
        }
        if (!state.open) {
            state.open = true;
            state.all_selected = true;
            state.snapshot = row.snapshot;
            state.sequence = sequence;
            state.snapshot_clear = selected && !state.selected_before && row.snapshot && row.event &&
                std::holds_alternative<data::MboEvent>(*row.event) &&
                std::get<data::MboEvent>(*row.event).action == data::MboAction::Clear;
        }
        state.all_selected = state.all_selected && selected;
        if (row.last) {
            if (!state.all_selected && (selected_in_group_[key] || selected)) {
                report.groups = Check::Unresolved;
                report.failures.push_back({ordinal, "filtered_group", "selection excluded part of a required event group"});
            }
            if (state.snapshot_clear && state.all_selected) { report.initial_state = Check::Passed; }
            state.open = false;
            selected_in_group_[key] = false;
        } else {
            selected_in_group_[key] = selected_in_group_[key] || selected;
        }
        if (selected && row.event) { state.selected_before = true; }
        if (row.bad_book && selected) {
            report.ordering = Check::Unresolved;
            report.failures.push_back({ordinal, "provider_channel_gap", "provider reports an unrecoverable channel gap"});
        }
    }

    void finish(ValidationReport& report) const {
        for (const auto& [key, state] : states_) {
            static_cast<void>(key);
            if (state.open) {
                report.groups = Check::Unresolved;
                report.failures.push_back({0, "incomplete_group", "acquisition ended before F_LAST"});
            }
        }
    }
private:
    std::map<std::pair<std::uint32_t, std::uint32_t>, Group> states_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, bool> selected_in_group_;
};

[[nodiscard]] inline PreparedSegment prepare(
    const Acquisition& acquisition, const ReferenceSnapshot& references, const data::StreamId stream_id,
    const ingestion::TimeRange range, const Decoder& decoder, const bool explicit_partial = false) {
    validate_token(acquisition.operation_id);
    validate_token(acquisition.acquisition_id);
    validate_token(acquisition.attempt_id);
    references.validate();
    ingestion::validate(acquisition.query);
    ingestion::validate(acquisition.mapping);
    ingestion::validate(acquisition.provider_request.range);
    if (!decoder || !acquisition.query.event_time_range || acquisition.provider_metadata.empty()) {
        throw std::invalid_argument("preparation requires decoder, event-time query and retained provider metadata");
    }
    const auto& binding = references.catalog.at(stream_id);
    if (acquisition.mapping.instrument_id != binding.stream.instrument_id ||
        acquisition.mapping.venue_id != binding.stream.venue_id || acquisition.mapping.source_id != binding.stream.source_id) {
        throw std::invalid_argument("declared stream disagrees with original mapping");
    }
    if (explicit_partial && (range.start < acquisition.query.event_time_range->start ||
                             range.end > acquisition.query.event_time_range->end)) {
        throw std::invalid_argument("partial selection must be inside the original query");
    }
    PreparedSegment result{acquisition, references, binding, range, {}, {}};
    result.explicit_partial = explicit_partial;
    result.validation.assumptions = {
        "provider-returned succession is the acquisition input; exchange completeness is not independently proven",
        "provider-to-canonical mapping was explicitly supplied and checked against retained references",
    };
    if (acquisition.empty_book && acquisition.empty_book->stream == binding.stream &&
        acquisition.empty_book->time <= range.start && !acquisition.empty_book->reason.empty()) {
        result.validation.initial_state = Check::Passed;
        result.validation.assumptions.push_back("accepted empty-book initialization: " + acquisition.empty_book->reason);
    }
    if (acquisition.outcome != AcquisitionOutcome::Complete || !acquisition.operation_failure.empty()) {
        result.validation.structural = Check::Failed;
        result.validation.failures.push_back({0, "acquisition_failed", acquisition.operation_failure});
    }
    auto quality = acquisition.quality;
    const auto scope = data::MboStreamContext{binding.stream.instrument_id, binding.stream.venue_id, binding.stream.source_id};
    SourceOrderChecker ordering;
    std::uint64_t expected_ordinal = 0;
    for (const auto& raw : acquisition.records) {
        if (raw.ordinal != expected_ordinal) {
            result.validation.structural = Check::Failed;
            result.validation.ordering = Check::Failed;
            result.validation.failures.push_back({raw.ordinal, "ordinal_association", "raw ordinal does not match returned row position"});
        }
        if (expected_ordinal == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("acquisition ordinal exhausted");
        }
        ++expected_ordinal;
        const auto decoded = decoder(raw, acquisition);
        if (decoded.header && acquisition.query.event_time_range &&
            ingestion::contains(*acquisition.query.event_time_range, decoded.header->event_time) &&
            acquisition.query.source_receive_time_range && !decoded.header->source_receive_time) {
            if (result.validation.structural == Check::Passed) { result.validation.structural = Check::Unresolved; }
            result.validation.failures.push_back({raw.ordinal, "selection_timestamp", "required receive-time constraint cannot be evaluated"});
            ordering.observe(decoded, raw.ordinal, false, result.validation);
            continue;
        }
        const bool selected = decoded.header && ingestion::matches_time(acquisition.query,
            decoded.header->event_time, decoded.header->source_receive_time) &&
            (!explicit_partial || ingestion::contains(range, decoded.header->event_time));
        ordering.observe(decoded, raw.ordinal, selected, result.validation);
        if (decoded.header && !selected) {
            result.validation.filtered_ordinals.push_back(raw.ordinal);
            if (!decoded.error.empty()) { result.validation.failures.push_back({raw.ordinal, "excluded_decode_error", decoded.error}); }
            continue;
        }
        if (!decoded.error.empty() || !decoded.header) {
            result.validation.structural = Check::Failed;
            result.validation.failures.push_back({raw.ordinal, "provider_decode", decoded.error});
            continue;
        }
        if (!decoded.event) { continue; } // Native None remains in the raw stage.
        try {
            const auto& original = header(*decoded.event);
            references.validate_header(original);
            if (StreamIdentity{original.instrument_id, original.venue_id, original.source_id} != binding.stream) {
                throw std::invalid_argument("original observation does not match selected stream");
            }
            if (!ingestion::contains(range, original.event_time) ||
                !ingestion::matches_time(acquisition.query, original.event_time, original.source_receive_time)) {
                throw std::invalid_argument("selected row disagrees with declared range/query");
            }
            const auto issues = std::visit([](const auto& event) { return data::validate(event); }, *decoded.event);
            if (!issues.empty()) {
                for (const auto& issue : issues) {
                    result.validation.failures.push_back({raw.ordinal, issue.code, issue.message});
                }
                result.validation.structural = Check::Failed;
                continue;
            }
            result.rows.push_back({stream_id, raw.ordinal, *decoded.event});
            if (decoded.bad_receive_time) { quality.push_back({range, ingestion::DataQualityKind::Degraded, scope}); }
            if (decoded.bad_book) { quality.push_back({range, ingestion::DataQualityKind::SequenceGap, scope}); }
        } catch (const std::exception& error) {
            result.validation.structural = Check::Failed;
            result.validation.failures.push_back({raw.ordinal, "identity_or_range", error.what()});
        }
    }
    ordering.finish(result.validation);
    // Data quality is factual, distinct from operation failure and dependency gates.
    if (result.validation.structural == Check::Failed && acquisition.outcome == AcquisitionOutcome::Complete) {
        quality.push_back({range, ingestion::DataQualityKind::Corrupt, scope});
    }
    for (auto& observation : quality) {
        if (!observation.scope) { observation.scope = scope; }
        if (observation.kind == ingestion::DataQualityKind::SequenceGap &&
            ingestion::same_stream_scope(observation.scope, std::optional<data::MboStreamContext>{scope}) &&
            ingestion::intersection(range, observation.range) && result.validation.ordering == Check::Passed) {
            result.validation.ordering = Check::Unresolved;
            result.validation.failures.push_back({0, "scoped_sequence_gap", "retained scoped gap evidence has not been resolved by recovery"});
        }
    }
    const std::array<std::optional<data::MboStreamContext>, 1> scopes{scope};
    result.validation.data_states = ingestion::DataStateReducer::reduce(range, quality, scopes);
    return result;
}

} // namespace quant::integrity
