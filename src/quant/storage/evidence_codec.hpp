#pragma once

#include "quant/integrity/model.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>

namespace quant::storage::evidence {

class Writer {
public:
    template <typename T> void number(const T value) { output_ << +value << ' '; }
    void text(const std::string& value) { output_ << std::quoted(value) << '\n'; }
    [[nodiscard]] std::string str() const { return output_.str(); }
private:
    std::ostringstream output_;
};
class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}
    template <typename T> [[nodiscard]] T number() {
        std::string token;
        if (!(input_ >> token)) { throw std::invalid_argument("truncated evidence integer"); }
        T value{};
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size()) { throw std::invalid_argument("invalid evidence integer"); }
        return value;
    }
    [[nodiscard]] bool boolean() {
        const auto value = number<unsigned>();
        if (value > 1) { throw std::invalid_argument("invalid evidence boolean"); }
        return value != 0;
    }
    template <typename E> [[nodiscard]] E enumeration(const unsigned maximum) {
        const auto value = number<unsigned>();
        if (value > maximum) { throw std::invalid_argument("invalid evidence enum"); }
        return static_cast<E>(value);
    }
    [[nodiscard]] std::string text() {
        std::string value;
        if (!(input_ >> std::quoted(value)) || value.size() > 16 * 1024 * 1024) { throw std::invalid_argument("invalid evidence string"); }
        return value;
    }
    [[nodiscard]] std::size_t count() {
        const auto value = number<std::size_t>();
        if (value > 10'000'000) { throw std::invalid_argument("unreasonable evidence count"); }
        return value;
    }
    void expect(const std::string& value) { if (text() != value) { throw std::invalid_argument("unsupported evidence contract"); } }
    void finish() { input_ >> std::ws; if (!input_.eof()) { throw std::invalid_argument("trailing evidence fields"); } }
private:
    std::istringstream input_;
};

inline void put(Writer& writer, const ingestion::TimeRange& range) {
    writer.number(range.start.unix_nanos()); writer.number(range.end.unix_nanos());
}
inline ingestion::TimeRange get_range(Reader& reader) {
    const auto start = reader.number<std::int64_t>(); const auto end = reader.number<std::int64_t>();
    return {data::Timestamp{start}, data::Timestamp{end}};
}
inline void put(Writer& writer, const integrity::StreamIdentity& stream) {
    writer.number(stream.instrument_id.value()); writer.number(stream.venue_id.value()); writer.number(stream.source_id.value());
}
inline integrity::StreamIdentity get_stream(Reader& reader) {
    const auto instrument = reader.number<std::uint64_t>(); const auto venue = reader.number<std::uint32_t>();
    const auto source = reader.number<std::uint32_t>();
    return {data::InstrumentId{instrument}, data::VenueId{venue}, data::SourceId{source}};
}
template <typename T, typename Function> inline void optional(Writer& writer, const std::optional<T>& value, Function emit) {
    writer.number(value.has_value()); if (value) { emit(*value); }
}
template <typename T, typename Function> inline std::optional<T> optional(Reader& reader, Function consume) {
    if (!reader.boolean()) { return std::nullopt; } return consume();
}
inline void put(Writer& writer, const ingestion::ProviderMappingSegment& mapping) {
    writer.number(mapping.instrument_id.value()); writer.text(mapping.provider);
    optional(writer, mapping.symbol, [&](const auto& value) { writer.text(value); });
    optional(writer, mapping.venue_id, [&](const auto value) { writer.number(value.value()); });
    optional(writer, mapping.provider_id, [&](const auto& value) { writer.text(value); });
    put(writer, mapping.range);
    optional(writer, mapping.source_id, [&](const auto value) { writer.number(value.value()); });
}
inline ingestion::ProviderMappingSegment get_mapping(Reader& reader) {
    const auto instrument = data::InstrumentId{reader.number<std::uint64_t>()}; const auto provider = reader.text();
    const auto symbol = optional<std::string>(reader, [&] { return reader.text(); });
    const auto venue = optional<data::VenueId>(reader, [&] { return data::VenueId{reader.number<std::uint32_t>()}; });
    const auto native = optional<std::string>(reader, [&] { return reader.text(); });
    const auto range = get_range(reader);
    const auto source = optional<data::SourceId>(reader, [&] { return data::SourceId{reader.number<std::uint32_t>()}; });
    return {instrument, provider, symbol, venue, native, range, source};
}
inline void put(Writer& writer, const ingestion::MarketDataQuery& query) {
    writer.number(query.instrument_id.value());
    optional(writer, query.venue_id, [&](const auto value) { writer.number(value.value()); });
    writer.number(static_cast<unsigned>(query.level));
    optional(writer, query.event_time_range, [&](const auto range) { put(writer, range); });
    optional(writer, query.source_receive_time_range, [&](const auto range) { put(writer, range); });
    writer.number(static_cast<unsigned>(query.primary_time_basis)); writer.number(static_cast<unsigned>(query.fetch_policy));
    optional(writer, query.derivation_margin, [&](const auto margin) { writer.number(margin.before_nanos); writer.number(margin.after_nanos); });
}
inline ingestion::MarketDataQuery get_query(Reader& reader) {
    const auto instrument = data::InstrumentId{reader.number<std::uint64_t>()};
    const auto venue = optional<data::VenueId>(reader, [&] { return data::VenueId{reader.number<std::uint32_t>()}; });
    const auto level = reader.enumeration<ingestion::MarketDataLevel>(2);
    const auto event = optional<ingestion::TimeRange>(reader, [&] { return get_range(reader); });
    const auto receive = optional<ingestion::TimeRange>(reader, [&] { return get_range(reader); });
    const auto basis = reader.enumeration<ingestion::TimeBasis>(1);
    const auto policy = reader.enumeration<ingestion::FetchPolicy>(1);
    const auto margin = optional<ingestion::TimeMargin>(reader, [&] {
        const auto before = reader.number<std::int64_t>(); const auto after = reader.number<std::int64_t>();
        return ingestion::TimeMargin{before, after};
    });
    return {instrument, venue, level, event, receive, basis, policy, margin};
}
inline void put_scope(Writer& writer, const std::optional<data::MboStreamContext>& scope) {
    optional(writer, scope, [&](const auto& value) { put(writer, integrity::StreamIdentity{value.instrument_id, value.venue_id, value.source_id}); });
}
inline std::optional<data::MboStreamContext> get_scope(Reader& reader) {
    return optional<data::MboStreamContext>(reader, [&] { const auto value = get_stream(reader);
        return data::MboStreamContext{value.instrument_id, value.venue_id, value.source_id}; });
}

inline std::string encode_references(const integrity::ReferenceSnapshot& references) {
    references.validate(); Writer writer; writer.text("quant-references-v1"); writer.text(references.identity);
    writer.number(references.catalog.entries().size());
    for (const auto& binding : references.catalog.entries()) { writer.number(binding.id.value()); put(writer, binding.stream); }
    writer.number(references.instruments.size());
    for (const auto& instrument : references.instruments) {
        writer.number(instrument.instrument_id.value()); writer.number(instrument.valid_from.unix_nanos());
        optional(writer, instrument.valid_until, [&](const auto value) { writer.number(value.unix_nanos()); });
        writer.number(static_cast<unsigned>(instrument.type)); writer.text(instrument.symbol);
        optional(writer, instrument.currency, [&](const auto& value) { writer.text(value.value()); });
        optional(writer, instrument.tick_size, [&](const auto value) { writer.number(value.raw()); });
        optional(writer, instrument.lot_size, [&](const auto value) { writer.number(value.raw()); });
    }
    writer.number(references.venues.size());
    for (const auto& venue : references.venues) { writer.number(venue.id.value()); writer.text(venue.code); }
    writer.number(references.sources.size());
    for (const auto& source : references.sources) {
        writer.number(source.id.value()); writer.text(source.provider); writer.text(source.dataset); writer.text(source.schema);
    }
    writer.number(references.mappings.size()); for (const auto& mapping : references.mappings) { put(writer, mapping); }
    return writer.str();
}
inline integrity::ReferenceSnapshot decode_references(const std::string& text) {
    Reader reader{text}; reader.expect("quant-references-v1");
    integrity::ReferenceSnapshot references{reader.text(), {}, {}, {}, {}, {}};
    auto count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = data::StreamId{reader.number<std::uint64_t>()}; references.catalog.add({id, get_stream(reader)});
    }
    count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = data::InstrumentId{reader.number<std::uint64_t>()}; const auto start = data::Timestamp{reader.number<std::int64_t>()};
        const auto end = optional<data::Timestamp>(reader, [&] { return data::Timestamp{reader.number<std::int64_t>()}; });
        const auto type = reader.enumeration<data::InstrumentType>(3); const auto symbol = reader.text();
        const auto currency = optional<data::CurrencyCode>(reader, [&] { return data::CurrencyCode{reader.text()}; });
        const auto tick = optional<data::Price>(reader, [&] { return data::Price::from_raw(reader.number<std::int64_t>()); });
        const auto lot = optional<data::Quantity>(reader, [&] { return data::Quantity::from_raw(reader.number<std::int64_t>()); });
        references.instruments.push_back({id, start, end, type, symbol, currency, tick, lot});
    }
    count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = data::VenueId{reader.number<std::uint32_t>()}; references.venues.push_back({id, reader.text()});
    }
    count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = data::SourceId{reader.number<std::uint32_t>()}; const auto provider = reader.text();
        const auto dataset = reader.text(); const auto schema = reader.text(); references.sources.push_back({id, provider, dataset, schema});
    }
    count = reader.count(); for (std::size_t index = 0; index < count; ++index) { references.mappings.push_back(get_mapping(reader)); }
    reader.finish(); references.validate(); return references;
}

// Record bytes live separately in raw Parquet/journal; all supplied acquisition metadata is preserved here.
inline std::string encode_acquisition(const integrity::Acquisition& acquisition) {
    Writer writer; writer.text("quant-acquisition-v1");
    writer.text(acquisition.operation_id); writer.text(acquisition.acquisition_id); writer.text(acquisition.attempt_id);
    writer.number(acquisition.acquired_at.unix_nanos()); put(writer, acquisition.query); put(writer, acquisition.mapping);
    writer.number(static_cast<unsigned>(acquisition.provider_request.basis)); put(writer, acquisition.provider_request.range);
    writer.text(acquisition.provider_encoding); writer.text(acquisition.provider_metadata);
    writer.number(acquisition.native_bindings.size());
    for (const auto& binding : acquisition.native_bindings) { writer.number(binding.instrument_id); put(writer, binding.range); }
    writer.number(acquisition.provider_batches.size());
    for (const auto& batch : acquisition.provider_batches) {
        writer.text(batch.dataset); writer.text(batch.schema); writer.text(batch.provider_start); writer.text(batch.provider_end);
        writer.number(batch.conditions.size());
        for (const auto& condition : batch.conditions) {
            writer.text(condition.date); writer.text(condition.condition);
            optional(writer, condition.last_modified_date, [&](const auto& value) { writer.text(value); });
        }
        writer.number(batch.partial_symbols.size()); for (const auto& symbol : batch.partial_symbols) { writer.text(symbol); }
        writer.number(batch.not_found_symbols.size()); for (const auto& symbol : batch.not_found_symbols) { writer.text(symbol); }
    }
    writer.number(acquisition.quality.size());
    for (const auto& observation : acquisition.quality) {
        put(writer, observation.range); writer.number(static_cast<unsigned>(observation.kind)); put_scope(writer, observation.scope);
    }
    writer.number(static_cast<unsigned>(acquisition.outcome)); writer.text(acquisition.operation_failure); writer.text(acquisition.retry_of);
    optional(writer, acquisition.empty_book, [&](const auto& value) { put(writer, value.stream); writer.number(value.time.unix_nanos()); writer.text(value.reason); });
    writer.number(static_cast<unsigned>(acquisition.mode));
    return writer.str();
}
inline integrity::Acquisition decode_acquisition(const std::string& text) {
    Reader reader{text}; reader.expect("quant-acquisition-v1");
    const auto operation = reader.text(); const auto acquisition_id = reader.text(); const auto attempt = reader.text();
    const auto acquired = data::Timestamp{reader.number<std::int64_t>()}; const auto query = get_query(reader);
    const auto mapping = get_mapping(reader); const auto basis = reader.enumeration<ingestion::TimeBasis>(1); const auto range = get_range(reader);
    integrity::Acquisition acquisition{operation, acquisition_id, attempt, acquired, query, mapping, {basis, range}};
    integrity::validate_token(operation); integrity::validate_token(acquisition_id); integrity::validate_token(attempt);
    acquisition.provider_encoding = reader.text(); acquisition.provider_metadata = reader.text();
    auto count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto native = reader.number<std::uint32_t>(); acquisition.native_bindings.push_back({native, get_range(reader)});
    }
    count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto dataset = reader.text(); const auto schema = reader.text(); const auto start = reader.text(); const auto end = reader.text();
        ingestion::ProviderBatchMetadata batch{dataset, schema, start, end, {}, {}, {}};
        auto entries = reader.count();
        for (std::size_t entry = 0; entry < entries; ++entry) {
            const auto date = reader.text(); const auto condition = reader.text();
            batch.conditions.push_back({date, condition, optional<std::string>(reader, [&] { return reader.text(); })});
        }
        entries = reader.count(); for (std::size_t entry = 0; entry < entries; ++entry) { batch.partial_symbols.push_back(reader.text()); }
        entries = reader.count(); for (std::size_t entry = 0; entry < entries; ++entry) { batch.not_found_symbols.push_back(reader.text()); }
        acquisition.provider_batches.push_back(std::move(batch));
    }
    count = reader.count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto interval = get_range(reader); const auto kind = reader.enumeration<ingestion::DataQualityKind>(5);
        acquisition.quality.push_back({interval, kind, get_scope(reader)});
    }
    acquisition.outcome = reader.enumeration<integrity::AcquisitionOutcome>(1);
    acquisition.operation_failure = reader.text(); acquisition.retry_of = reader.text();
    acquisition.empty_book = optional<integrity::EmptyBookEvidence>(reader, [&] {
        const auto stream = get_stream(reader); const auto time = data::Timestamp{reader.number<std::int64_t>()};
        return integrity::EmptyBookEvidence{stream, time, reader.text()};
    });
    acquisition.mode = reader.enumeration<ingestion::AcquisitionMode>(1);
    reader.finish(); return acquisition;
}

inline std::string encode_report(const integrity::ValidationReport& report) {
    Writer writer; writer.text("quant-validation-v1");
    for (const auto value : {report.structural, report.ordering, report.groups, report.initial_state}) { writer.number(static_cast<unsigned>(value)); }
    writer.number(report.failures.size());
    for (const auto& failure : report.failures) { writer.number(failure.ordinal); writer.text(failure.rule); writer.text(failure.reason); }
    writer.number(report.filtered_ordinals.size()); for (const auto ordinal : report.filtered_ordinals) { writer.number(ordinal); }
    writer.number(report.data_states.size());
    for (const auto& segment : report.data_states) { put(writer, segment.range); writer.number(static_cast<unsigned>(segment.state)); put_scope(writer, segment.scope); }
    writer.number(report.assumptions.size()); for (const auto& assumption : report.assumptions) { writer.text(assumption); }
    return writer.str();
}

} // namespace quant::storage::evidence
