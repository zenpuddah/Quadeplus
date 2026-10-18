#pragma once

#include "quant/ingestion/databento_integrity.hpp"

namespace quant::fixture {

inline data::Timestamp time(const std::int64_t nanos) { return data::Timestamp::from_unix_nanos(nanos); }
inline ingestion::TimeRange range(const std::int64_t start, const std::int64_t end) { return {time(start), time(end)}; }

template <typename Integer>
inline void put(std::vector<std::byte>& bytes, const std::size_t offset, const Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto bits = std::bit_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.at(offset + index) = static_cast<std::byte>((bits >> (index * 8U)) & 255U);
    }
}

inline integrity::RawRecord record(
    const std::uint64_t ordinal, const char action, const std::int64_t event_time,
    const std::uint32_t sequence, const std::uint8_t flags = 128,
    const std::uint64_t order = 100, const std::uint32_t size = 2,
    const std::int64_t price = 100'000'000'000, const char side = 'B') {
    std::vector<std::byte> bytes(56);
    put<std::uint8_t>(bytes, 0, 14);
    put<std::uint8_t>(bytes, 1, 0xA0);
    put<std::uint16_t>(bytes, 2, 2);
    put<std::uint32_t>(bytes, 4, 9001);
    put<std::uint64_t>(bytes, 8, static_cast<std::uint64_t>(event_time));
    put(bytes, 16, order);
    put(bytes, 24, price);
    put(bytes, 32, size);
    put(bytes, 36, flags);
    put<std::uint8_t>(bytes, 37, 2);
    put<std::uint8_t>(bytes, 38, static_cast<std::uint8_t>(action));
    put<std::uint8_t>(bytes, 39, static_cast<std::uint8_t>(side));
    put<std::uint64_t>(bytes, 40, static_cast<std::uint64_t>(event_time + 1));
    put<std::int32_t>(bytes, 48, 1);
    put(bytes, 52, sequence);
    return {ordinal, std::move(bytes)};
}

inline ingestion::ProviderMappingSegment mapping() {
    return {data::InstrumentId{42}, "databento", std::string{"TEST"}, data::VenueId{7},
            std::string{"9001"}, range(0, 10'000), data::SourceId{9}};
}

inline integrity::ReferenceSnapshot references() {
    integrity::StreamCatalog catalog;
    catalog.add({data::StreamId{1}, {data::InstrumentId{42}, data::VenueId{7}, data::SourceId{9}}});
    return {"references-1", catalog,
        {{data::InstrumentId{42}, time(0), time(10'000), data::InstrumentType::Equity, "TEST",
          data::CurrencyCode{"USD"}, data::Price::from_decimal(1, 2), data::Quantity::from_integer(1)}},
        {{data::VenueId{7}, "XNAS"}}, {{data::SourceId{9}, "databento", "XNAS.ITCH", "mbo"}}, {mapping()}};
}

inline integrity::Acquisition acquisition(const std::string& identity = "acquisition-1") {
    const ingestion::MarketDataQuery query{
        data::InstrumentId{42}, data::VenueId{7}, ingestion::MarketDataLevel::L3,
        range(100, 200), std::nullopt, ingestion::TimeBasis::EventTime,
        ingestion::FetchPolicy::DeriveProviderRange, ingestion::TimeMargin{10, 10}};
    integrity::Acquisition result{"operation-1", identity, "attempt-1", time(500), query, mapping(),
        ingestion::resolve_provider_fetch_range(query)};
    result.provider_metadata = "synthetic normalized XNAS MBO fixture; 56-byte little-endian DBN records; not a network download";
    result.native_bindings.push_back({9001, range(0, 10'000)});
    result.quality.push_back({range(100, 200), ingestion::DataQualityKind::Complete,
        data::MboStreamContext{data::InstrumentId{42}, data::VenueId{7}, data::SourceId{9}}});
    result.provider_batches.push_back({"XNAS.ITCH", "mbo", "90", "210", {}, {}, {}});
    result.records = {
        record(0, 'R', 100, 0, 160, 0, 0, 0, 'N'),
        record(1, 'A', 101, 7),
        record(2, 'T', 103, 8, 0, 0, 1),
        record(3, 'F', 103, 8, 0, 100, 1),
        record(4, 'C', 103, 8, 128, 100, 1),
    };
    return result;
}

inline integrity::PreparedSegment prepared(const std::string& identity = "acquisition-1") {
    return integrity::prepare(acquisition(identity), references(), data::StreamId{1}, range(100, 200),
                              ingestion::decode_xnas_record);
}

} // namespace quant::fixture
