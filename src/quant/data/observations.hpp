#pragma once

#include "quant/data/value_types.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <vector>

namespace quant::data {

struct EventHeader {
    InstrumentId instrument_id;
    VenueId venue_id;
    Timestamp event_time;
    std::optional<Timestamp> source_receive_time; // Provider/source receive time, not local arrival time.
    std::optional<std::uint64_t> sequence;
    std::optional<std::uint32_t> channel_id;
    std::uint64_t source_flags;
    SourceId source_id;

    friend bool operator==(const EventHeader&, const EventHeader&) = default;
};

struct MboEvent {
    EventHeader header;
    MboAction action;
    std::optional<OrderId> order_id;
    std::optional<Side> side;
    std::optional<Price> price;
    std::optional<Quantity> quantity;

    [[nodiscard]] bool has_complete_order_state() const noexcept {
        return action != MboAction::Clear && order_id && side && price && quantity;
    }
};

// These types keep action-specific requirements at the writer boundary.
struct MboAdd {
    EventHeader header;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
};

struct MboModify {
    EventHeader header;
    OrderId order_id;
    std::optional<Side> side;
    std::optional<Price> price;
    std::optional<Quantity> quantity;
};

struct MboCancel {
    EventHeader header;
    OrderId order_id;
    Quantity quantity;
};

struct MboExecute {
    EventHeader header;
    OrderId order_id;
    Quantity quantity;
};

struct MboClear {
    EventHeader header;
};

struct OrderExecution {
    EventHeader header;
    OrderId resting_order_id;
    Quantity executed_quantity;
    std::optional<Price> price;
};

struct MboStreamContext {
    InstrumentId instrument_id;
    VenueId venue_id;
    SourceId source_id;
};

// A record contains event-local data. Stream scope is stored once by MboBuffer.
struct alignas(64) MboRecord {
    std::int64_t event_time;
    std::int64_t source_receive_time;
    std::uint64_t sequence;
    std::uint64_t order_id;
    std::int64_t price;
    std::int64_t quantity;
    std::uint64_t source_flags;
    std::uint32_t channel_id;
    std::uint32_t control;
};

static_assert(sizeof(MboRecord) == 64);
static_assert(alignof(MboRecord) == 64);
static_assert(offsetof(MboRecord, event_time) == 0);
static_assert(offsetof(MboRecord, source_receive_time) == 8);
static_assert(offsetof(MboRecord, sequence) == 16);
static_assert(offsetof(MboRecord, order_id) == 24);
static_assert(offsetof(MboRecord, price) == 32);
static_assert(offsetof(MboRecord, quantity) == 40);
static_assert(offsetof(MboRecord, source_flags) == 48);
static_assert(offsetof(MboRecord, channel_id) == 56);
static_assert(offsetof(MboRecord, control) == 60);

namespace mbo_detail {

inline constexpr std::uint32_t action_mask = 0x07U;
inline constexpr std::uint32_t source_receive_time_present = 1U << 3U;
inline constexpr std::uint32_t sequence_present = 1U << 4U;
inline constexpr std::uint32_t channel_id_present = 1U << 5U;
inline constexpr std::uint32_t order_id_present = 1U << 6U;
inline constexpr std::uint32_t side_present = 1U << 7U;
inline constexpr std::uint32_t price_present = 1U << 8U;
inline constexpr std::uint32_t quantity_present = 1U << 9U;
inline constexpr std::uint32_t sell_side = 1U << 10U;

[[nodiscard]] inline std::uint32_t encode_control(
    const MboAction action,
    const std::optional<Timestamp>& source_receive_time,
    const std::optional<std::uint64_t>& sequence,
    const std::optional<std::uint32_t>& channel_id,
    const std::optional<OrderId>& order_id,
    const std::optional<Side>& side,
    const std::optional<Price>& price,
    const std::optional<Quantity>& quantity) noexcept {
    auto control = static_cast<std::uint32_t>(action) & action_mask;
    if (source_receive_time) {
        control |= source_receive_time_present;
    }
    if (sequence) {
        control |= sequence_present;
    }
    if (channel_id) {
        control |= channel_id_present;
    }
    if (order_id) {
        control |= order_id_present;
    }
    if (side) {
        control |= side_present;
        if (*side == Side::Sell) {
            control |= sell_side;
        }
    }
    if (price) {
        control |= price_present;
    }
    if (quantity) {
        control |= quantity_present;
    }
    return control;
}

[[nodiscard]] inline MboAction decode_action(const std::uint32_t control) noexcept {
    return static_cast<MboAction>(control & action_mask);
}

[[nodiscard]] inline bool has(const std::uint32_t control, const std::uint32_t mask) noexcept {
    return (control & mask) != 0;
}

} // namespace mbo_detail

class MboRecordView {
public:
    [[nodiscard]] const MboRecord& record() const noexcept { return *record_; }
    [[nodiscard]] MboAction action() const noexcept { return mbo_detail::decode_action(record_->control); }

    [[nodiscard]] EventHeader header() const {
        return EventHeader{
            context_->instrument_id,
            context_->venue_id,
            Timestamp::from_unix_nanos(record_->event_time),
            source_receive_time(),
            sequence(),
            channel_id(),
            record_->source_flags,
            context_->source_id,
        };
    }

    [[nodiscard]] std::optional<Timestamp> source_receive_time() const noexcept {
        if (!mbo_detail::has(record_->control, mbo_detail::source_receive_time_present)) {
            return std::nullopt;
        }
        return Timestamp::from_unix_nanos(record_->source_receive_time);
    }

    [[nodiscard]] std::optional<std::uint64_t> sequence() const noexcept {
        if (!mbo_detail::has(record_->control, mbo_detail::sequence_present)) {
            return std::nullopt;
        }
        return record_->sequence;
    }

    [[nodiscard]] std::optional<std::uint32_t> channel_id() const noexcept {
        if (!mbo_detail::has(record_->control, mbo_detail::channel_id_present)) {
            return std::nullopt;
        }
        return record_->channel_id;
    }

    [[nodiscard]] std::optional<OrderId> order_id() const {
        if (!mbo_detail::has(record_->control, mbo_detail::order_id_present)) {
            return std::nullopt;
        }
        return OrderId{record_->order_id};
    }

    [[nodiscard]] std::optional<Side> side() const noexcept {
        if (!mbo_detail::has(record_->control, mbo_detail::side_present)) {
            return std::nullopt;
        }
        return mbo_detail::has(record_->control, mbo_detail::sell_side) ? Side::Sell : Side::Buy;
    }

    [[nodiscard]] std::optional<Price> price() const noexcept {
        if (!mbo_detail::has(record_->control, mbo_detail::price_present)) {
            return std::nullopt;
        }
        return Price::from_raw(record_->price);
    }

    [[nodiscard]] std::optional<Quantity> quantity() const {
        if (!mbo_detail::has(record_->control, mbo_detail::quantity_present)) {
            return std::nullopt;
        }
        return Quantity::from_raw(record_->quantity);
    }

    [[nodiscard]] std::uint64_t source_flags() const noexcept { return record_->source_flags; }

    [[nodiscard]] MboEvent event() const {
        return MboEvent{header(), action(), order_id(), side(), price(), quantity()};
    }

    [[nodiscard]] std::optional<MboAdd> as_add() const {
        if (action() != MboAction::Add) {
            return std::nullopt;
        }
        const auto id = order_id();
        const auto event_side = side();
        const auto event_price = price();
        const auto event_quantity = quantity();
        if (!id || !event_side || !event_price || !event_quantity) {
            return std::nullopt;
        }
        return MboAdd{header(), *id, *event_side, *event_price, *event_quantity};
    }

    [[nodiscard]] std::optional<MboModify> as_modify() const {
        if (action() != MboAction::Modify) {
            return std::nullopt;
        }
        const auto id = order_id();
        if (!id || (!side() && !price() && !quantity())) {
            return std::nullopt;
        }
        return MboModify{header(), *id, side(), price(), quantity()};
    }

    [[nodiscard]] std::optional<MboCancel> as_cancel() const {
        if (action() != MboAction::Cancel) {
            return std::nullopt;
        }
        const auto id = order_id();
        const auto event_quantity = quantity();
        if (!id || !event_quantity) {
            return std::nullopt;
        }
        return MboCancel{header(), *id, *event_quantity};
    }

    [[nodiscard]] std::optional<MboExecute> as_execute() const {
        if (action() != MboAction::Execute) {
            return std::nullopt;
        }
        const auto id = order_id();
        const auto event_quantity = quantity();
        if (!id || !event_quantity) {
            return std::nullopt;
        }
        return MboExecute{header(), *id, *event_quantity};
    }

    [[nodiscard]] std::optional<MboClear> as_clear() const {
        return action() == MboAction::Clear ? std::optional<MboClear>{MboClear{header()}} : std::nullopt;
    }

private:
    MboRecordView(const MboRecord* record, const MboStreamContext* context) noexcept
        : record_(record), context_(context) {}

    const MboRecord* record_;
    const MboStreamContext* context_;

    friend class MboBuffer;
};

class MboBuffer {
public:
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = MboRecordView;
        using difference_type = std::ptrdiff_t;

        [[nodiscard]] MboRecordView operator*() const noexcept { return owner_->view_at(index_); }

        const_iterator& operator++() noexcept {
            ++index_;
            return *this;
        }

        friend bool operator==(const const_iterator&, const const_iterator&) = default;

    private:
        const_iterator(const MboBuffer* owner, const std::size_t index) noexcept : owner_(owner), index_(index) {}

        const MboBuffer* owner_;
        std::size_t index_;

        friend class MboBuffer;
    };

    explicit MboBuffer(const MboStreamContext context, const std::size_t initial_capacity = 0) : context_(context) {
        records_.reserve(initial_capacity);
    }

    void append(const MboAdd& event) {
        append_record(
            event.header,
            MboAction::Add,
            std::optional<OrderId>{event.order_id},
            std::optional<Side>{event.side},
            std::optional<Price>{event.price},
            std::optional<Quantity>{event.quantity});
    }

    void append(const MboModify& event) {
        if (!event.side && !event.price && !event.quantity) {
            throw std::invalid_argument("MboModify requires at least one changed order field");
        }
        append_record(
            event.header,
            MboAction::Modify,
            std::optional<OrderId>{event.order_id},
            event.side,
            event.price,
            event.quantity);
    }

    void append(const MboCancel& event) {
        append_record(
            event.header,
            MboAction::Cancel,
            std::optional<OrderId>{event.order_id},
            std::nullopt,
            std::nullopt,
            std::optional<Quantity>{event.quantity});
    }

    void append(const MboExecute& event) {
        append_record(
            event.header,
            MboAction::Execute,
            std::optional<OrderId>{event.order_id},
            std::nullopt,
            std::nullopt,
            std::optional<Quantity>{event.quantity});
    }

    void append(const MboClear& event) {
        append_record(event.header, MboAction::Clear, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
    }

    // This overload keeps the logical record useful at the adapter boundary; storage is still fixed-stride.
    void append(const MboEvent& event) {
        switch (event.action) {
        case MboAction::Add:
            if (!event.order_id || !event.side || !event.price || !event.quantity) {
                throw std::invalid_argument("MboEvent Add is incomplete");
            }
            append(MboAdd{event.header, *event.order_id, *event.side, *event.price, *event.quantity});
            break;
        case MboAction::Modify:
            if (!event.order_id || (!event.side && !event.price && !event.quantity)) {
                throw std::invalid_argument("MboEvent Modify is incomplete");
            }
            append(MboModify{event.header, *event.order_id, event.side, event.price, event.quantity});
            break;
        case MboAction::Cancel:
            if (!event.order_id || !event.quantity) {
                throw std::invalid_argument("MboEvent Cancel is incomplete");
            }
            append(MboCancel{event.header, *event.order_id, *event.quantity});
            break;
        case MboAction::Execute:
            if (!event.order_id || !event.quantity) {
                throw std::invalid_argument("MboEvent Execute is incomplete");
            }
            append(MboExecute{event.header, *event.order_id, *event.quantity});
            break;
        case MboAction::Clear:
            if (event.order_id || event.side || event.price || event.quantity) {
                throw std::invalid_argument("MboEvent Clear must not carry order fields");
            }
            append(MboClear{event.header});
            break;
        default:
            throw std::invalid_argument("MboEvent has an unknown action");
        }
    }

    [[nodiscard]] const MboStreamContext& context() const noexcept { return context_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] const MboRecord* data() const noexcept { return records_.data(); }

    [[nodiscard]] MboRecordView operator[](const std::size_t index) const noexcept { return view_at(index); }

    [[nodiscard]] MboRecordView at(const std::size_t index) const {
        if (index >= records_.size()) {
            throw std::out_of_range("MboBuffer index out of range");
        }
        return view_at(index);
    }

    [[nodiscard]] const_iterator begin() const noexcept { return const_iterator(this, 0); }
    [[nodiscard]] const_iterator end() const noexcept { return const_iterator(this, records_.size()); }

private:
    void append_record(
        const EventHeader& header,
        const MboAction action,
        const std::optional<OrderId>& order_id,
        const std::optional<Side>& side,
        const std::optional<Price>& price,
        const std::optional<Quantity>& quantity) {
        if (header.instrument_id != context_.instrument_id || header.venue_id != context_.venue_id ||
            header.source_id != context_.source_id) {
            throw std::invalid_argument("MBO event scope does not match buffer context");
        }
        if (side && *side != Side::Buy && *side != Side::Sell) {
            throw std::invalid_argument("MBO side is outside the canonical vocabulary");
        }

        records_.push_back(MboRecord{
            header.event_time.unix_nanos(),
            header.source_receive_time ? header.source_receive_time->unix_nanos() : 0,
            header.sequence.value_or(0),
            order_id ? order_id->value() : 0,
            price ? price->raw() : 0,
            quantity ? quantity->raw() : 0,
            header.source_flags,
            header.channel_id.value_or(0),
            mbo_detail::encode_control(
                action,
                header.source_receive_time,
                header.sequence,
                header.channel_id,
                order_id,
                side,
                price,
                quantity),
        });
    }

    [[nodiscard]] MboRecordView view_at(const std::size_t index) const noexcept {
        return MboRecordView(&records_[index], &context_);
    }

    MboStreamContext context_;
    std::vector<MboRecord> records_;
};

struct Trade {
    EventHeader header;
    Price price;
    Quantity quantity;
    std::optional<Side> aggressor_side;
};

// Provider-native identity and action evidence lives outside the fixed MBO record.
struct ProviderRecordMetadata {
    EventHeader header;
    std::uint32_t provider_instrument_id;
    std::uint16_t provider_publisher_id;
    std::uint8_t provider_action;

    friend bool operator==(const ProviderRecordMetadata&, const ProviderRecordMetadata&) = default;
};

struct Quote {
    EventHeader header;
    std::optional<Price> bid_price;
    std::optional<Quantity> bid_quantity;
    std::optional<Price> ask_price;
    std::optional<Quantity> ask_quantity;
};

struct Bar {
    EventHeader header;
    Timestamp interval_start;
    Timestamp interval_end;
    Price open;
    Price high;
    Price low;
    Price close;
    Quantity volume;
};

} // namespace quant::data
