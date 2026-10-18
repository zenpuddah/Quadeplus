#pragma once

// Full normalized DBN MBO bytes are interpreted only at this provider boundary.
#include "quant/integrity/prepare.hpp"

#include <bit>
#include <charconv>

namespace quant::ingestion {

namespace dbn_integrity_detail {
template <typename Integer>
[[nodiscard]] inline Integer little_endian(const std::span<const std::byte> bytes, const std::size_t offset) {
    using Unsigned = std::make_unsigned_t<Integer>;
    if (offset + sizeof(Integer) > bytes.size()) { throw std::invalid_argument("truncated normalized DBN MBO record"); }
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Unsigned>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8U);
    }
    return std::bit_cast<Integer>(value);
}

[[nodiscard]] inline std::optional<data::Timestamp> timestamp(const std::uint64_t nanos) {
    if (nanos == std::numeric_limits<std::uint64_t>::max()) { return std::nullopt; }
    if (nanos > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("DBN timestamp outside canonical signed nanosecond range");
    }
    return data::Timestamp::from_unix_nanos(static_cast<std::int64_t>(nanos));
}
} // namespace dbn_integrity_detail

[[nodiscard]] inline integrity::DecodedRecord decode_xnas_record(
    const integrity::RawRecord& raw, const integrity::Acquisition& acquisition) {
    using namespace dbn_integrity_detail;
    integrity::DecodedRecord result;
    try {
        if (acquisition.provider_encoding != "dbn-mbo-56-le-v1" || raw.bytes.size() != 56 ||
            little_endian<std::uint8_t>(raw.bytes, 0) != 14 || little_endian<std::uint8_t>(raw.bytes, 1) != 0xA0) {
            throw std::invalid_argument("unsupported normalized provider record encoding/length/type");
        }
        if (!acquisition.mapping.venue_id || !acquisition.mapping.source_id) {
            throw std::invalid_argument("provider mapping has no canonical venue/source");
        }
        result.native_instrument = little_endian<std::uint32_t>(raw.bytes, 4);
        const auto event_time = timestamp(little_endian<std::uint64_t>(raw.bytes, 8));
        if (!event_time) { throw std::invalid_argument("undefined DBN event time"); }
        const auto flags = little_endian<std::uint8_t>(raw.bytes, 36);
        result.snapshot = (flags & 32U) != 0;
        result.last = (flags & 128U) != 0;
        result.bad_receive_time = (flags & 8U) != 0;
        result.bad_book = (flags & 4U) != 0;
        result.header = data::EventHeader{
            acquisition.mapping.instrument_id, *acquisition.mapping.venue_id, *event_time,
            timestamp(little_endian<std::uint64_t>(raw.bytes, 40)),
            little_endian<std::uint32_t>(raw.bytes, 52), little_endian<std::uint8_t>(raw.bytes, 37),
            flags, *acquisition.mapping.source_id,
        };
        // The publisher definition is part of the concrete XNAS adapter, not a canonical ID.
        if (little_endian<std::uint16_t>(raw.bytes, 2) != 2) {
            throw std::invalid_argument("unexpected XNAS.ITCH publisher");
        }
        bool known = false;
        for (const auto& binding : acquisition.native_bindings) {
            if (binding.instrument_id == result.native_instrument && contains(binding.range, *event_time)) { known = true; }
        }
        if (!known) { throw std::invalid_argument("native instrument has no retained point-in-time mapping"); }
        const auto action = static_cast<char>(little_endian<std::uint8_t>(raw.bytes, 38));
        const auto native_side = static_cast<char>(little_endian<std::uint8_t>(raw.bytes, 39));
        std::optional<data::Side> side;
        if (native_side == 'B') { side = data::Side::Buy; }
        else if (native_side == 'A') { side = data::Side::Sell; }
        else if (native_side != 'N') { throw std::invalid_argument("unknown DBN side"); }
        const auto order = little_endian<std::uint64_t>(raw.bytes, 16);
        const auto price = little_endian<std::int64_t>(raw.bytes, 24);
        const auto size = little_endian<std::uint32_t>(raw.bytes, 32);
        const auto order_id = order == 0 ? std::nullopt : std::optional<data::OrderId>{data::OrderId{order}};
        const auto event_price = price == std::numeric_limits<std::int64_t>::max() ? std::nullopt :
            std::optional<data::Price>{data::Price::from_raw(price)};
        const auto quantity = size == std::numeric_limits<std::uint32_t>::max() ? std::nullopt :
            std::optional<data::Quantity>{data::Quantity::from_integer(size)};
        if (action == 'N') { return result; }
        if (action == 'T') {
            if (!event_price || !quantity) { throw std::invalid_argument("undefined Trade price/size"); }
            result.event = data::Trade{*result.header, *event_price, *quantity, side};
        } else if (action == 'F') {
            if (!order_id || !quantity) { throw std::invalid_argument("undefined execution order/size"); }
            result.event = data::OrderExecution{*result.header, *order_id, *quantity, event_price};
        } else {
            data::MboAction canonical_action;
            switch (action) {
            case 'A': canonical_action = data::MboAction::Add; break;
            case 'M': canonical_action = data::MboAction::Modify; break;
            case 'C': canonical_action = data::MboAction::Cancel; break;
            case 'R': canonical_action = data::MboAction::Clear; break;
            default: throw std::invalid_argument("unknown DBN action");
            }
            result.event = data::MboEvent{*result.header, canonical_action,
                action == 'R' ? std::nullopt : order_id,
                action == 'C' || action == 'R' ? std::nullopt : side,
                action == 'C' || action == 'R' ? std::nullopt : event_price,
                action == 'R' ? std::nullopt : quantity};
        }
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

} // namespace quant::ingestion
