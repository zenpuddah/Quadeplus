#pragma once

#include "quant/ingestion/provider.hpp"
#include "quant/ingestion/query_plan.hpp"
#include "quant/ingestion/databento_integrity.hpp"

#include <databento/constants.hpp>
#include <databento/historical.hpp>
#include <databento/record.hpp>

#include <chrono>
#include <cstring>
#include <sstream>
#include <cstdint>
#include <date/date.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace quant::ingestion {

namespace databento_detail {

[[nodiscard]] inline std::optional<data::OrderId> order_id(const std::uint64_t raw_order_id) {
    if (raw_order_id == 0) {
        return std::nullopt;
    }
    return data::OrderId{raw_order_id};
}

[[nodiscard]] inline std::optional<data::Price> price(const std::int64_t raw_price) noexcept {
    if (raw_price == ::databento::kUndefPrice) {
        return std::nullopt;
    }
    return data::Price::from_raw(raw_price);
}

[[nodiscard]] inline std::optional<data::Quantity> quantity(const std::uint32_t raw_quantity) {
    if (raw_quantity == ::databento::kUndefOrderSize) {
        return std::nullopt;
    }
    return data::Quantity::from_integer(static_cast<std::int64_t>(raw_quantity));
}

[[nodiscard]] inline std::optional<data::Side> side(const ::databento::Side raw_side) noexcept {
    switch (raw_side) {
    case ::databento::Side::Bid:
        return data::Side::Buy;
    case ::databento::Side::Ask:
        return data::Side::Sell;
    case ::databento::Side::None:
        return std::nullopt;
    }
    return std::nullopt;
}

template <typename TimePoint>
[[nodiscard]] inline std::optional<data::Timestamp> timestamp(const TimePoint value) {
    const auto raw_nanos = value.time_since_epoch().count();
    using RawNanos = decltype(raw_nanos);
    if constexpr (std::is_signed_v<RawNanos>) {
        if (raw_nanos < 0) {
            throw std::invalid_argument("Databento timestamp is negative");
        }
    }
    const auto unsigned_nanos = static_cast<std::uint64_t>(raw_nanos);
    if (unsigned_nanos == ::databento::kUndefTimestamp) {
        return std::nullopt;
    }
    if (unsigned_nanos > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("Databento timestamp is outside the canonical range");
    }
    return data::Timestamp::from_unix_nanos(static_cast<std::int64_t>(unsigned_nanos));
}

template <typename TimePoint>
[[nodiscard]] inline data::Timestamp required_timestamp(const TimePoint value, const char* field_name) {
    const auto result = timestamp(value);
    if (!result) {
        throw std::invalid_argument(std::string("Databento ") + field_name + " timestamp is undefined");
    }
    return *result;
}

[[nodiscard]] inline data::EventHeader header(
    const ::databento::MboMsg& message,
    const ProviderMappingSegment& segment) {
    if (!segment.venue_id || !segment.source_id) {
        throw std::invalid_argument("Databento mappings require canonical venue and source identities");
    }
    return data::EventHeader{
        segment.instrument_id,
        *segment.venue_id,
        required_timestamp(message.hd.ts_event, "event"),
        timestamp(message.ts_recv),
        std::uint64_t{message.sequence},
        std::uint32_t{message.channel_id},
        std::uint64_t{static_cast<std::uint8_t>(message.flags)},
        *segment.source_id,
    };
}

[[nodiscard]] inline std::optional<data::MboStreamContext> scope(
    const ProviderMappingSegment& segment) {
    if (!segment.venue_id || !segment.source_id) {
        return std::nullopt;
    }
    return data::MboStreamContext{segment.instrument_id, *segment.venue_id, *segment.source_id};
}

[[nodiscard]] inline std::optional<std::uint32_t> provider_instrument_id(
    const ProviderMappingSegment& segment) {
    if (!segment.provider_id) {
        return std::nullopt;
    }

    std::size_t parsed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(*segment.provider_id, &parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument("Databento provider IDs must be unsigned instrument IDs");
    }
    if (parsed != segment.provider_id->size() || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Databento provider IDs must be unsigned 32-bit instrument IDs");
    }
    return static_cast<std::uint32_t>(value);
}

inline void mark_corrupt(ProviderBatch& batch, const ProviderMappingSegment& segment) {
    batch.quality_observations.push_back(
        DataQualityObservation{segment.range, DataQualityKind::Corrupt, scope(segment)});
}

[[nodiscard]] inline ProviderBatch translate_mbo(
    const ::databento::MboMsg& message,
    const ProviderMappingSegment& segment) {
    ProviderBatch batch;
    const auto event_header = header(message, segment);
    batch.provider_records.push_back(data::ProviderRecordMetadata{
        event_header,
        message.hd.instrument_id,
        message.hd.publisher_id,
        static_cast<std::uint8_t>(message.action),
    });

    const auto message_order_id = order_id(message.order_id);
    const auto message_side = side(message.side);
    const auto message_price = price(message.price);
    const auto message_quantity = quantity(message.size);

    switch (message.action) {
    case ::databento::Action::Add:
        batch.mbo_events.push_back(
            data::MboEvent{event_header, data::MboAction::Add, message_order_id, message_side, message_price, message_quantity});
        break;
    case ::databento::Action::Modify:
        batch.mbo_events.push_back(
            data::MboEvent{event_header, data::MboAction::Modify, message_order_id, message_side, message_price, message_quantity});
        break;
    case ::databento::Action::Cancel:
        batch.mbo_events.push_back(
            data::MboEvent{event_header, data::MboAction::Cancel, message_order_id, std::nullopt, std::nullopt, message_quantity});
        break;
    case ::databento::Action::Clear:
        batch.mbo_events.push_back(
            data::MboEvent{event_header, data::MboAction::Clear, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
        break;
    case ::databento::Action::Trade:
        if (!message_price || !message_quantity) {
            mark_corrupt(batch, segment);
            break;
        }
        batch.trades.push_back(data::Trade{event_header, *message_price, *message_quantity, message_side});
        break;
    case ::databento::Action::Fill:
        if (!message_order_id || !message_quantity) {
            mark_corrupt(batch, segment);
            break;
        }
        batch.order_executions.push_back(
            data::OrderExecution{event_header, *message_order_id, *message_quantity, message_price});
        break;
    case ::databento::Action::None:
        // Preserve the raw no-op as provider metadata, without creating a book mutation.
        break;
    default:
        mark_corrupt(batch, segment);
        break;
    }
    return batch;
}

[[nodiscard]] inline bool matches_segment(
    const data::EventHeader& event_header,
    const MarketDataQuery& query,
    const ProviderMappingSegment& segment) noexcept {
    return event_header.instrument_id == query.instrument_id &&
           (!query.venue_id || event_header.venue_id == *query.venue_id) &&
           (!segment.venue_id || event_header.venue_id == *segment.venue_id) &&
           (!segment.source_id || event_header.source_id == *segment.source_id) &&
           matches_time(query, event_header.event_time, event_header.source_receive_time) &&
           contains(segment.range, event_header.event_time);
}

[[nodiscard]] inline date::sys_time<std::chrono::nanoseconds> date_time(const data::Timestamp timestamp) {
    if (timestamp.unix_nanos() < 0) {
        throw std::invalid_argument("Databento requests require non-negative UNIX timestamps");
    }
    return date::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{timestamp.unix_nanos()}};
}

[[nodiscard]] inline ::databento::UnixNanos unix_nanos(const data::Timestamp timestamp) {
    if (timestamp.unix_nanos() < 0) {
        throw std::invalid_argument("Databento requests require non-negative UNIX timestamps");
    }
    return ::databento::UnixNanos{
        ::databento::UnixNanos::duration{static_cast<std::uint64_t>(timestamp.unix_nanos())}};
}

[[nodiscard]] inline std::string format_date(const date::sys_days day) {
    return date::format("%F", date::year_month_day{day});
}

[[nodiscard]] inline ::databento::DateRange date_range(const TimeRange range) {
    validate(range);
    const auto start_time = date_time(range.start);
    const auto end_time = date_time(range.end);
    const auto start_day = date::floor<date::days>(start_time);
    const auto end_date = date::floor<date::days>(end_time - std::chrono::nanoseconds{1});
    return ::databento::DateRange{format_date(start_day), format_date(end_date)};
}

[[nodiscard]] inline TimeRange fetch_time_range(const MarketDataQuery& query) {
    return resolve_provider_fetch_range(query).range;
}

[[nodiscard]] inline ::databento::DateTimeRange<::databento::UnixNanos> provider_range(
    const MarketDataQuery& query) {
    const auto fetch_range = fetch_time_range(query);
    return ::databento::DateTimeRange<::databento::UnixNanos>{
        unix_nanos(fetch_range.start),
        unix_nanos(fetch_range.end),
    };
}

[[nodiscard]] inline std::optional<DataQualityKind> condition_kind(
    const std::vector<::databento::DatasetConditionDetail>& conditions) noexcept {
    if (conditions.empty()) {
        return std::nullopt;
    }
    bool degraded = false;
    bool pending = false;
    for (const auto& condition : conditions) {
        switch (condition.condition) {
        case ::databento::DatasetCondition::Missing:
            return DataQualityKind::Missing;
        case ::databento::DatasetCondition::Pending:
            pending = true;
            break;
        case ::databento::DatasetCondition::Degraded:
            degraded = true;
            break;
        case ::databento::DatasetCondition::Available:
            break;
        }
    }
    if (pending) {
        return DataQualityKind::Pending;
    }
    return degraded ? std::optional<DataQualityKind>{DataQualityKind::Degraded}
                    : std::optional<DataQualityKind>{DataQualityKind::Complete};
}

[[nodiscard]] inline std::string condition_name(const ::databento::DatasetCondition condition) {
    switch (condition) {
    case ::databento::DatasetCondition::Available:
        return "available";
    case ::databento::DatasetCondition::Degraded:
        return "degraded";
    case ::databento::DatasetCondition::Pending:
        return "pending";
    case ::databento::DatasetCondition::Missing:
        return "missing";
    }
    return "unknown";
}

[[nodiscard]] inline data::Timestamp day_timestamp(const date::sys_days day) {
    constexpr std::int64_t nanos_per_day = 86'400'000'000'000LL;
    const auto days = day.time_since_epoch().count();
    if (days > std::numeric_limits<std::int64_t>::max() / nanos_per_day ||
        days < std::numeric_limits<std::int64_t>::min() / nanos_per_day) {
        throw std::invalid_argument("provider UTC day exceeds canonical timestamp range");
    }
    return data::Timestamp::from_unix_nanos(static_cast<std::int64_t>(days) * nanos_per_day);
}

[[nodiscard]] inline std::vector<DataQualityObservation> day_conditions(
    const std::vector<::databento::DatasetConditionDetail>& conditions,
    const ProviderMappingSegment& segment) {
    std::vector<DataQualityObservation> observations;
    for (const auto& condition : conditions) {
        date::year_month_day day;
        std::istringstream input{condition.date};
        input >> date::parse("%F", day);
        if (input.fail() || !day.ok() || condition.date.size() != 10) {
            throw std::invalid_argument("invalid provider condition UTC date");
        }
        const auto start = date::sys_days{day};
        const TimeRange day_range{day_timestamp(start), day_timestamp(start + date::days{1})};
        const auto clipped = intersection(segment.range, day_range);
        if (!clipped) { continue; }
        const auto kind = condition_kind({condition});
        if (kind) { observations.push_back({*clipped, *kind, scope(segment)}); }
    }
    return observations;
}

[[nodiscard]] inline std::vector<integrity::NativeBinding> native_bindings(
    const ::databento::Metadata& metadata, const ProviderMappingSegment& segment) {
    if (const auto id = provider_instrument_id(segment)) { return {{*id, segment.range}}; }
    std::vector<integrity::NativeBinding> bindings;
    for (const auto& mapping : metadata.mappings) {
        if (!segment.symbol || mapping.raw_symbol != *segment.symbol) { continue; }
        for (const auto& interval : mapping.intervals) {
            std::uint32_t id = 0;
            const auto [end, error] = std::from_chars(interval.symbol.data(), interval.symbol.data() + interval.symbol.size(), id);
            if (error != std::errc{} || end != interval.symbol.data() + interval.symbol.size() || id == 0) {
                throw std::invalid_argument("provider symbology does not resolve a native instrument ID");
            }
            const auto start = date::sys_days{interval.start_date};
            const auto finish = date::sys_days{interval.end_date};
            bindings.push_back({id, TimeRange{day_timestamp(start), day_timestamp(finish)}});
        }
    }
    return bindings;
}

inline void append_batch(ProviderBatch& destination, ProviderBatch source) {
    destination.mbo_events.insert(
        destination.mbo_events.end(), source.mbo_events.begin(), source.mbo_events.end());
    destination.trades.insert(destination.trades.end(), source.trades.begin(), source.trades.end());
    destination.order_executions.insert(
        destination.order_executions.end(), source.order_executions.begin(), source.order_executions.end());
    destination.provider_records.insert(
        destination.provider_records.end(), source.provider_records.begin(), source.provider_records.end());
    destination.quality_observations.insert(
        destination.quality_observations.end(),
        source.quality_observations.begin(),
        source.quality_observations.end());
}

} // namespace databento_detail

class DatabentoStrictLogReceiver final : public ::databento::ILogReceiver {
public:
    void Receive(const ::databento::LogLevel, const std::string& message) override {
        if (message.find("partial record") != std::string::npos ||
            message.find("Partial or incomplete record") != std::string::npos) {
            throw std::runtime_error("Databento returned a partial record stream: " + message);
        }
    }
};

class DatabentoProvider final : public ProviderPort {
public:
    explicit DatabentoProvider(::databento::Historical& client, std::string provider_name = "databento")
        : client_(client), provider_name_(std::move(provider_name)) {
        if (provider_name_.empty()) {
            throw std::invalid_argument("Databento provider name must not be empty");
        }
    }

    [[nodiscard]] std::string_view provider_name() const noexcept override { return provider_name_; }

    [[nodiscard]] static ::databento::DateTimeRange<::databento::UnixNanos> resolve_provider_range(
        const MarketDataQuery& query) {
        return databento_detail::provider_range(query);
    }

    integrity::Acquisition acquire(integrity::Acquisition request, integrity::AcquisitionSink& sink) override {
        integrity::validate_token(request.operation_id);
        integrity::validate_token(request.acquisition_id);
        integrity::validate_token(request.attempt_id);
        sink.begin(request);
        try {
            validate(request.query);
            validate(request.mapping);
            if (request.mode != AcquisitionMode::Historical || std::endian::native != std::endian::little) {
                throw std::invalid_argument("this acquisition adapter supports historical normalized little-endian records only");
            }
            if (request.mapping.provider != provider_name_ || !request.mapping.venue_id || !request.mapping.source_id) {
                throw std::invalid_argument("retained Databento acquisition requires a matching full scope");
            }
            const auto id = databento_detail::provider_instrument_id(request.mapping);
            if (!id && !request.mapping.symbol) { throw std::invalid_argument("missing provider symbol/ID"); }
            request.provider_request = resolve_provider_fetch_range(request.query);
            const auto conditions = client_.MetadataGetDatasetCondition(dataset_, databento_detail::date_range(request.provider_request.range));
            auto store = client_.TimeseriesGetRange(dataset_, resolve_provider_range(request.query),
                std::vector<std::string>{id ? *request.mapping.provider_id : *request.mapping.symbol},
                ::databento::Schema::Mbo, id ? ::databento::SType::InstrumentId : ::databento::SType::RawSymbol,
                ::databento::SType::InstrumentId, std::uint64_t{0});
            const auto& metadata = store.GetMetadata();
            request.provider_metadata = ::databento::ToString(metadata); // Complete SDK metadata fields and mappings.
            request.native_bindings = databento_detail::native_bindings(metadata, request.mapping);
            ProviderBatchMetadata batch{metadata.dataset, "mbo", ::databento::ToIso8601(metadata.start),
                ::databento::ToIso8601(metadata.end), {}, metadata.partial, metadata.not_found};
            for (const auto& condition : conditions) {
                batch.conditions.push_back({condition.date, databento_detail::condition_name(condition.condition), condition.last_modified_date});
            }
            request.provider_batches.push_back(std::move(batch));
            request.quality = databento_detail::day_conditions(conditions, request.mapping);
            if (!metadata.not_found.empty()) { request.quality.push_back({request.mapping.range, DataQualityKind::Missing, databento_detail::scope(request.mapping)}); }
            if (!metadata.partial.empty()) { request.quality.push_back({request.mapping.range, DataQualityKind::Degraded, databento_detail::scope(request.mapping)}); }
            sink.metadata(request);
            if (metadata.dataset != dataset_ || !metadata.schema || *metadata.schema != ::databento::Schema::Mbo || metadata.ts_out) {
                throw std::runtime_error("unsupported retained Databento metadata/encoding");
            }
            std::uint64_t ordinal = 0;
            while (const auto* record = store.NextRecord()) {
                integrity::RawRecord raw{ordinal, std::vector<std::byte>(record->Size())};
                std::memcpy(raw.bytes.data(), &record->Header(), raw.bytes.size());
                sink.record(raw); // Durably retain original delivered bytes BEFORE filtering/translation.
                request.records.push_back(std::move(raw));
                if (ordinal == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("acquisition ordinal exhausted"); }
                ++ordinal;
            }
            request.outcome = integrity::AcquisitionOutcome::Complete;
        } catch (const std::exception& error) {
            request.outcome = integrity::AcquisitionOutcome::Failed;
            request.operation_failure = error.what();
        }
        sink.finish(request);
        return request;
    }

    ProviderBatch fetch(const MarketDataQuery& query, const ProviderMappingSegment& segment) override {
        validate(query);
        validate(segment);
        if (segment.provider != provider_name_) {
            throw std::invalid_argument("Databento mapping does not match the provider port");
        }
        const auto mapped_provider_instrument_id = databento_detail::provider_instrument_id(segment);
        if (!segment.symbol && !mapped_provider_instrument_id) {
            throw std::invalid_argument("Databento mapping requires a provider symbol or instrument ID");
        }
        if (!segment.venue_id || !segment.source_id) {
            throw std::invalid_argument("Databento mapping requires canonical venue and source identities");
        }

        const auto requested_provider_range = resolve_provider_range(query);
        const auto conditions = client_.MetadataGetDatasetCondition(
            dataset_, databento_detail::date_range(databento_detail::fetch_time_range(query)));

        const auto provider_key = segment.provider_id ? *segment.provider_id : *segment.symbol;
        const auto symbol_type = mapped_provider_instrument_id ? ::databento::SType::InstrumentId
                                                               : ::databento::SType::RawSymbol;
        auto store = client_.TimeseriesGetRange(
            dataset_,
            requested_provider_range,
            std::vector<std::string>{provider_key},
            ::databento::Schema::Mbo,
            symbol_type,
            ::databento::SType::InstrumentId,
            std::uint64_t{0});

        const auto& metadata = store.GetMetadata();
        if (metadata.dataset != dataset_ || !metadata.schema || *metadata.schema != ::databento::Schema::Mbo) {
            throw std::runtime_error("Databento returned metadata for an unexpected dataset or schema");
        }

        ProviderBatch batch;
        batch.source_ids.push_back(*segment.source_id);
        ProviderBatchMetadata batch_metadata{
            metadata.dataset,
            "mbo",
            ::databento::ToIso8601(metadata.start),
            ::databento::ToIso8601(metadata.end),
            {},
            metadata.partial,
            metadata.not_found,
        };
        batch_metadata.conditions.reserve(conditions.size());
        for (const auto& condition : conditions) {
            batch_metadata.conditions.push_back(ProviderConditionEvidence{
                condition.date,
                databento_detail::condition_name(condition.condition),
                condition.last_modified_date,
            });
        }
        batch.batch_metadata.push_back(std::move(batch_metadata));

        bool bad_book = false;
        bool bad_source_receive_time = false;
        while (const auto* record = store.NextRecord()) {
            if (!record->Holds<::databento::MboMsg>()) {
                throw std::runtime_error("Databento MBO request returned a non-MBO record");
            }
            const auto& message = record->Get<::databento::MboMsg>();
            if (message.hd.publisher_id !=
                static_cast<std::uint16_t>(::databento::Publisher::XnasItchXnas)) {
                throw std::runtime_error("Databento MBO request returned an unexpected publisher");
            }
            if (mapped_provider_instrument_id && message.hd.instrument_id != *mapped_provider_instrument_id) {
                throw std::runtime_error("Databento MBO record returned an unexpected instrument ID");
            }
            const auto event_header = databento_detail::header(message, segment);
            if (!databento_detail::matches_segment(event_header, query, segment)) {
                continue;
            }
            bad_book = bad_book || message.flags.IsMaybeBadBook();
            bad_source_receive_time = bad_source_receive_time || message.flags.IsBadTsRecv();
            databento_detail::append_batch(batch, databento_detail::translate_mbo(message, segment));
        }

        if (!batch.batch_metadata.front().not_found_symbols.empty()) {
            batch.quality_observations.push_back(
                DataQualityObservation{segment.range, DataQualityKind::Missing, databento_detail::scope(segment)});
        } else if (!batch.batch_metadata.front().partial_symbols.empty()) {
            batch.quality_observations.push_back(
                DataQualityObservation{segment.range, DataQualityKind::Degraded, databento_detail::scope(segment)});
        } else {
            const auto scoped = databento_detail::day_conditions(conditions, segment);
            batch.quality_observations.insert(batch.quality_observations.end(), scoped.begin(), scoped.end());
        }
        if (bad_book) {
            batch.quality_observations.push_back(
                DataQualityObservation{segment.range, DataQualityKind::SequenceGap, databento_detail::scope(segment)});
        }
        if (bad_source_receive_time) {
            batch.quality_observations.push_back(
                DataQualityObservation{segment.range, DataQualityKind::Degraded, databento_detail::scope(segment)});
        }
        return batch;
    }

private:
    static constexpr const char* dataset_ = ::databento::dataset::kXnasItch;

    ::databento::Historical& client_;
    std::string provider_name_;
};

} // namespace quant::ingestion
