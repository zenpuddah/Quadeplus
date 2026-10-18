#pragma once

#include "quant/data/observations.hpp"
#include "quant/data/validation.hpp"
#include "quant/storage/dataset.hpp"

#include <arrow/api.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace quant::storage {

using ArrowBatch = std::shared_ptr<arrow::RecordBatch>;

enum class BatchProjection {
    Full,
    Replay,
};

namespace column_names {

inline constexpr std::string_view event_time = "event_time";
inline constexpr std::string_view source_receive_time = "source_receive_time";
inline constexpr std::string_view sequence = "sequence";
inline constexpr std::string_view order_id = "order_id";
inline constexpr std::string_view price = "price";
inline constexpr std::string_view quantity = "quantity";
inline constexpr std::string_view source_flags = "source_flags";
inline constexpr std::string_view channel_id = "channel_id";
inline constexpr std::string_view control = "control";
inline constexpr std::string_view aggressor_side = "aggressor_side";
inline constexpr std::string_view resting_order_id = "resting_order_id";
inline constexpr std::string_view executed_quantity = "executed_quantity";
inline constexpr std::string_view stream_id = "stream_id";
inline constexpr std::string_view event_ordinal = "event_ordinal";

} // namespace column_names

[[nodiscard]] inline std::vector<std::string> canonical_column_names(const DataKind kind) {
    switch (kind) {
    case DataKind::Mbo:
        return {
            std::string{column_names::event_time},
            std::string{column_names::source_receive_time},
            std::string{column_names::sequence},
            std::string{column_names::order_id},
            std::string{column_names::price},
            std::string{column_names::quantity},
            std::string{column_names::source_flags},
            std::string{column_names::channel_id},
            std::string{column_names::control},
        };
    case DataKind::Trades:
        return {
            std::string{column_names::event_time},
            std::string{column_names::source_receive_time},
            std::string{column_names::sequence},
            std::string{column_names::channel_id},
            std::string{column_names::source_flags},
            std::string{column_names::price},
            std::string{column_names::quantity},
            std::string{column_names::aggressor_side},
        };
    case DataKind::OrderExecution:
        return {
            std::string{column_names::event_time},
            std::string{column_names::source_receive_time},
            std::string{column_names::sequence},
            std::string{column_names::channel_id},
            std::string{column_names::source_flags},
            std::string{column_names::resting_order_id},
            std::string{column_names::executed_quantity},
            std::string{column_names::price},
        };
    }
    return {};
}

[[nodiscard]] inline std::shared_ptr<arrow::Schema> schema_for(const DataKind kind) {
    using arrow::field;

    switch (kind) {
    case DataKind::Mbo:
        return arrow::schema({
            field(std::string{column_names::event_time}, arrow::int64(), false),
            field(std::string{column_names::source_receive_time}, arrow::int64(), false),
            field(std::string{column_names::sequence}, arrow::uint64(), false),
            field(std::string{column_names::order_id}, arrow::uint64(), false),
            field(std::string{column_names::price}, arrow::int64(), false),
            field(std::string{column_names::quantity}, arrow::int64(), false),
            field(std::string{column_names::source_flags}, arrow::uint64(), false),
            field(std::string{column_names::channel_id}, arrow::uint32(), false),
            field(std::string{column_names::control}, arrow::uint32(), false),
        });
    case DataKind::Trades:
        return arrow::schema({
            field(std::string{column_names::event_time}, arrow::int64(), false),
            field(std::string{column_names::source_receive_time}, arrow::int64()),
            field(std::string{column_names::sequence}, arrow::uint64()),
            field(std::string{column_names::channel_id}, arrow::uint32()),
            field(std::string{column_names::source_flags}, arrow::uint64(), false),
            field(std::string{column_names::price}, arrow::int64(), false),
            field(std::string{column_names::quantity}, arrow::int64(), false),
            field(std::string{column_names::aggressor_side}, arrow::int8()),
        });
    case DataKind::OrderExecution:
        return arrow::schema({
            field(std::string{column_names::event_time}, arrow::int64(), false),
            field(std::string{column_names::source_receive_time}, arrow::int64()),
            field(std::string{column_names::sequence}, arrow::uint64()),
            field(std::string{column_names::channel_id}, arrow::uint32()),
            field(std::string{column_names::source_flags}, arrow::uint64(), false),
            field(std::string{column_names::resting_order_id}, arrow::uint64(), false),
            field(std::string{column_names::executed_quantity}, arrow::int64(), false),
            field(std::string{column_names::price}, arrow::int64()),
        });
    }
    return arrow::schema({});
}

[[nodiscard]] inline arrow::Status validate_batch_schema(
    const arrow::Schema& schema,
    const DataKind kind) {
    const auto names = canonical_column_names(kind);
    if (names.empty()) {
        return arrow::Status::Invalid("Arrow batch uses an unknown data kind");
    }
    const auto expected = schema_for(kind);
    for (const auto& name : names) {
        const auto index = schema.GetFieldIndex(name);
        if (index < 0) {
            return arrow::Status::Invalid("Arrow batch is missing canonical column: ", name);
        }
        const auto expected_index = expected->GetFieldIndex(name);
        const auto& actual_field = schema.field(index);
        const auto& expected_field = expected->field(expected_index);
        if (!actual_field->type()->Equals(expected_field->type())) {
            return arrow::Status::Invalid("Arrow column has an unexpected type: ", name);
        }
        if (!expected_field->nullable() && actual_field->nullable()) {
            return arrow::Status::Invalid("Arrow required column is nullable: ", name);
        }
    }
    return arrow::Status::OK();
}

namespace detail {

template <typename ArrayType>
[[nodiscard]] inline arrow::Result<std::shared_ptr<ArrayType>> column(
    const arrow::RecordBatch& batch,
    const std::string_view name,
    const arrow::Type::type expected_type) {
    const auto index = batch.schema()->GetFieldIndex(std::string{name});
    if (index < 0) {
        return arrow::Status::Invalid("Arrow batch is missing canonical column: ", name);
    }
    const auto array = batch.column(index);
    if (array->type_id() != expected_type) {
        return arrow::Status::Invalid("Arrow column has an unexpected type: ", name);
    }
    const auto typed = std::dynamic_pointer_cast<ArrayType>(array);
    if (!typed) {
        return arrow::Status::Invalid("Arrow column cannot be read as its declared type: ", name);
    }
    return typed;
}

[[nodiscard]] inline std::optional<data::Timestamp> optional_timestamp(
    const std::shared_ptr<arrow::Int64Array>& values,
    const int64_t index) {
    return values->IsNull(index) ? std::nullopt : std::optional<data::Timestamp>{
                                                   data::Timestamp::from_unix_nanos(values->Value(index))};
}

[[nodiscard]] inline std::optional<std::uint64_t> optional_uint64(
    const std::shared_ptr<arrow::UInt64Array>& values,
    const int64_t index) {
    return values->IsNull(index) ? std::nullopt : std::optional<std::uint64_t>{values->Value(index)};
}

[[nodiscard]] inline std::optional<std::uint32_t> optional_uint32(
    const std::shared_ptr<arrow::UInt32Array>& values,
    const int64_t index) {
    return values->IsNull(index) ? std::nullopt : std::optional<std::uint32_t>{values->Value(index)};
}

[[nodiscard]] inline std::optional<data::Side> optional_side(
    const std::shared_ptr<arrow::Int8Array>& values,
    const int64_t index) {
    if (values->IsNull(index)) {
        return std::nullopt;
    }
    return values->Value(index) == 0 ? std::optional<data::Side>{data::Side::Buy}
                                     : std::optional<data::Side>{data::Side::Sell};
}

[[nodiscard]] inline data::EventHeader header(
    const StreamIdentity& stream,
    const std::shared_ptr<arrow::Int64Array>& event_time,
    const std::shared_ptr<arrow::Int64Array>& source_receive_time,
    const std::shared_ptr<arrow::UInt64Array>& sequence,
    const std::shared_ptr<arrow::UInt32Array>& channel_id,
    const std::shared_ptr<arrow::UInt64Array>& source_flags,
    const int64_t index) {
    return data::EventHeader{
        stream.instrument_id,
        stream.venue_id,
        data::Timestamp::from_unix_nanos(event_time->Value(index)),
        optional_timestamp(source_receive_time, index),
        optional_uint64(sequence, index),
        optional_uint32(channel_id, index),
        source_flags->Value(index),
        stream.source_id,
    };
}

[[nodiscard]] inline data::MboEvent event_from_record(
    const data::MboRecord& record,
    const StreamIdentity& stream) {
    const auto has = [control = record.control](const std::uint32_t mask) {
        return data::mbo_detail::has(control, mask);
    };

    return data::MboEvent{
        data::EventHeader{
            stream.instrument_id,
            stream.venue_id,
            data::Timestamp::from_unix_nanos(record.event_time),
            has(data::mbo_detail::source_receive_time_present)
                ? std::optional<data::Timestamp>{data::Timestamp::from_unix_nanos(record.source_receive_time)}
                : std::nullopt,
            has(data::mbo_detail::sequence_present) ? std::optional<std::uint64_t>{record.sequence} : std::nullopt,
            has(data::mbo_detail::channel_id_present) ? std::optional<std::uint32_t>{record.channel_id}
                                                       : std::nullopt,
            record.source_flags,
            stream.source_id,
        },
        data::mbo_detail::decode_action(record.control),
        has(data::mbo_detail::order_id_present) ? std::optional<data::OrderId>{data::OrderId{record.order_id}}
                                                 : std::nullopt,
        has(data::mbo_detail::side_present)
            ? std::optional<data::Side>{has(data::mbo_detail::sell_side) ? data::Side::Sell : data::Side::Buy}
            : std::nullopt,
        has(data::mbo_detail::price_present) ? std::optional<data::Price>{data::Price::from_raw(record.price)}
                                              : std::nullopt,
        has(data::mbo_detail::quantity_present)
            ? std::optional<data::Quantity>{data::Quantity::from_raw(record.quantity)}
            : std::nullopt,
    };
}

} // namespace detail

[[nodiscard]] inline arrow::Result<ArrowBatch> to_arrow(const data::MboBuffer& buffer) {
    arrow::Int64Builder event_time;
    arrow::Int64Builder source_receive_time;
    arrow::UInt64Builder sequence;
    arrow::UInt64Builder order_id;
    arrow::Int64Builder price;
    arrow::Int64Builder quantity;
    arrow::UInt64Builder source_flags;
    arrow::UInt32Builder channel_id;
    arrow::UInt32Builder control;

    for (std::size_t index = 0; index < buffer.size(); ++index) {
        const auto& record = buffer.data()[index];
        ARROW_RETURN_NOT_OK(event_time.Append(record.event_time));
        ARROW_RETURN_NOT_OK(source_receive_time.Append(record.source_receive_time));
        ARROW_RETURN_NOT_OK(sequence.Append(record.sequence));
        ARROW_RETURN_NOT_OK(order_id.Append(record.order_id));
        ARROW_RETURN_NOT_OK(price.Append(record.price));
        ARROW_RETURN_NOT_OK(quantity.Append(record.quantity));
        ARROW_RETURN_NOT_OK(source_flags.Append(record.source_flags));
        ARROW_RETURN_NOT_OK(channel_id.Append(record.channel_id));
        ARROW_RETURN_NOT_OK(control.Append(record.control));
    }

    std::shared_ptr<arrow::Array> event_time_array;
    std::shared_ptr<arrow::Array> source_receive_time_array;
    std::shared_ptr<arrow::Array> sequence_array;
    std::shared_ptr<arrow::Array> order_id_array;
    std::shared_ptr<arrow::Array> price_array;
    std::shared_ptr<arrow::Array> quantity_array;
    std::shared_ptr<arrow::Array> source_flags_array;
    std::shared_ptr<arrow::Array> channel_id_array;
    std::shared_ptr<arrow::Array> control_array;
    ARROW_RETURN_NOT_OK(event_time.Finish(&event_time_array));
    ARROW_RETURN_NOT_OK(source_receive_time.Finish(&source_receive_time_array));
    ARROW_RETURN_NOT_OK(sequence.Finish(&sequence_array));
    ARROW_RETURN_NOT_OK(order_id.Finish(&order_id_array));
    ARROW_RETURN_NOT_OK(price.Finish(&price_array));
    ARROW_RETURN_NOT_OK(quantity.Finish(&quantity_array));
    ARROW_RETURN_NOT_OK(source_flags.Finish(&source_flags_array));
    ARROW_RETURN_NOT_OK(channel_id.Finish(&channel_id_array));
    ARROW_RETURN_NOT_OK(control.Finish(&control_array));

    return arrow::RecordBatch::Make(
        schema_for(DataKind::Mbo),
        static_cast<int64_t>(buffer.size()),
        {
            event_time_array,
            source_receive_time_array,
            sequence_array,
            order_id_array,
            price_array,
            quantity_array,
            source_flags_array,
            channel_id_array,
            control_array,
        });
}

[[nodiscard]] inline arrow::Result<ArrowBatch> to_arrow(const std::span<const data::Trade> trades) {
    arrow::Int64Builder event_time;
    arrow::Int64Builder source_receive_time;
    arrow::UInt64Builder sequence;
    arrow::UInt32Builder channel_id;
    arrow::UInt64Builder source_flags;
    arrow::Int64Builder price;
    arrow::Int64Builder quantity;
    arrow::Int8Builder aggressor_side;

    for (const auto& trade : trades) {
        if (!data::is_valid(data::validate(trade))) { return arrow::Status::Invalid("invalid canonical Trade"); }
        if (trade.header.instrument_id != trades.front().header.instrument_id ||
            trade.header.venue_id != trades.front().header.venue_id || trade.header.source_id != trades.front().header.source_id) {
            return arrow::Status::Invalid("mixed-stream Trade input");
        }
        ARROW_RETURN_NOT_OK(event_time.Append(trade.header.event_time.unix_nanos()));
        if (trade.header.source_receive_time) {
            ARROW_RETURN_NOT_OK(source_receive_time.Append(trade.header.source_receive_time->unix_nanos()));
        } else {
            ARROW_RETURN_NOT_OK(source_receive_time.AppendNull());
        }
        if (trade.header.sequence) {
            ARROW_RETURN_NOT_OK(sequence.Append(*trade.header.sequence));
        } else {
            ARROW_RETURN_NOT_OK(sequence.AppendNull());
        }
        if (trade.header.channel_id) {
            ARROW_RETURN_NOT_OK(channel_id.Append(*trade.header.channel_id));
        } else {
            ARROW_RETURN_NOT_OK(channel_id.AppendNull());
        }
        ARROW_RETURN_NOT_OK(source_flags.Append(trade.header.source_flags));
        ARROW_RETURN_NOT_OK(price.Append(trade.price.raw()));
        ARROW_RETURN_NOT_OK(quantity.Append(trade.quantity.raw()));
        if (trade.aggressor_side) {
            ARROW_RETURN_NOT_OK(aggressor_side.Append(*trade.aggressor_side == data::Side::Buy ? 0 : 1));
        } else {
            ARROW_RETURN_NOT_OK(aggressor_side.AppendNull());
        }
    }

    std::shared_ptr<arrow::Array> event_time_array;
    std::shared_ptr<arrow::Array> source_receive_time_array;
    std::shared_ptr<arrow::Array> sequence_array;
    std::shared_ptr<arrow::Array> channel_id_array;
    std::shared_ptr<arrow::Array> source_flags_array;
    std::shared_ptr<arrow::Array> price_array;
    std::shared_ptr<arrow::Array> quantity_array;
    std::shared_ptr<arrow::Array> aggressor_side_array;
    ARROW_RETURN_NOT_OK(event_time.Finish(&event_time_array));
    ARROW_RETURN_NOT_OK(source_receive_time.Finish(&source_receive_time_array));
    ARROW_RETURN_NOT_OK(sequence.Finish(&sequence_array));
    ARROW_RETURN_NOT_OK(channel_id.Finish(&channel_id_array));
    ARROW_RETURN_NOT_OK(source_flags.Finish(&source_flags_array));
    ARROW_RETURN_NOT_OK(price.Finish(&price_array));
    ARROW_RETURN_NOT_OK(quantity.Finish(&quantity_array));
    ARROW_RETURN_NOT_OK(aggressor_side.Finish(&aggressor_side_array));

    return arrow::RecordBatch::Make(
        schema_for(DataKind::Trades),
        static_cast<int64_t>(trades.size()),
        {
            event_time_array,
            source_receive_time_array,
            sequence_array,
            channel_id_array,
            source_flags_array,
            price_array,
            quantity_array,
            aggressor_side_array,
        });
}

[[nodiscard]] inline arrow::Result<ArrowBatch> to_arrow(
    const std::span<const data::OrderExecution> executions) {
    arrow::Int64Builder event_time;
    arrow::Int64Builder source_receive_time;
    arrow::UInt64Builder sequence;
    arrow::UInt32Builder channel_id;
    arrow::UInt64Builder source_flags;
    arrow::UInt64Builder resting_order_id;
    arrow::Int64Builder executed_quantity;
    arrow::Int64Builder price;

    for (const auto& execution : executions) {
        if (!data::is_valid(data::validate(execution))) { return arrow::Status::Invalid("invalid canonical OrderExecution"); }
        if (execution.header.instrument_id != executions.front().header.instrument_id ||
            execution.header.venue_id != executions.front().header.venue_id || execution.header.source_id != executions.front().header.source_id) {
            return arrow::Status::Invalid("mixed-stream execution input");
        }
        ARROW_RETURN_NOT_OK(event_time.Append(execution.header.event_time.unix_nanos()));
        if (execution.header.source_receive_time) {
            ARROW_RETURN_NOT_OK(source_receive_time.Append(execution.header.source_receive_time->unix_nanos()));
        } else {
            ARROW_RETURN_NOT_OK(source_receive_time.AppendNull());
        }
        if (execution.header.sequence) {
            ARROW_RETURN_NOT_OK(sequence.Append(*execution.header.sequence));
        } else {
            ARROW_RETURN_NOT_OK(sequence.AppendNull());
        }
        if (execution.header.channel_id) {
            ARROW_RETURN_NOT_OK(channel_id.Append(*execution.header.channel_id));
        } else {
            ARROW_RETURN_NOT_OK(channel_id.AppendNull());
        }
        ARROW_RETURN_NOT_OK(source_flags.Append(execution.header.source_flags));
        ARROW_RETURN_NOT_OK(resting_order_id.Append(execution.resting_order_id.value()));
        ARROW_RETURN_NOT_OK(executed_quantity.Append(execution.executed_quantity.raw()));
        if (execution.price) {
            ARROW_RETURN_NOT_OK(price.Append(execution.price->raw()));
        } else {
            ARROW_RETURN_NOT_OK(price.AppendNull());
        }
    }

    std::shared_ptr<arrow::Array> event_time_array;
    std::shared_ptr<arrow::Array> source_receive_time_array;
    std::shared_ptr<arrow::Array> sequence_array;
    std::shared_ptr<arrow::Array> channel_id_array;
    std::shared_ptr<arrow::Array> source_flags_array;
    std::shared_ptr<arrow::Array> resting_order_id_array;
    std::shared_ptr<arrow::Array> executed_quantity_array;
    std::shared_ptr<arrow::Array> price_array;
    ARROW_RETURN_NOT_OK(event_time.Finish(&event_time_array));
    ARROW_RETURN_NOT_OK(source_receive_time.Finish(&source_receive_time_array));
    ARROW_RETURN_NOT_OK(sequence.Finish(&sequence_array));
    ARROW_RETURN_NOT_OK(channel_id.Finish(&channel_id_array));
    ARROW_RETURN_NOT_OK(source_flags.Finish(&source_flags_array));
    ARROW_RETURN_NOT_OK(resting_order_id.Finish(&resting_order_id_array));
    ARROW_RETURN_NOT_OK(executed_quantity.Finish(&executed_quantity_array));
    ARROW_RETURN_NOT_OK(price.Finish(&price_array));

    return arrow::RecordBatch::Make(
        schema_for(DataKind::OrderExecution),
        static_cast<int64_t>(executions.size()),
        {
            event_time_array,
            source_receive_time_array,
            sequence_array,
            channel_id_array,
            source_flags_array,
            resting_order_id_array,
            executed_quantity_array,
            price_array,
        });
}

[[nodiscard]] inline arrow::Result<data::MboBuffer> from_arrow(
    const arrow::RecordBatch& batch,
    const StreamIdentity& stream) {
    ARROW_RETURN_NOT_OK(batch.ValidateFull());
    ARROW_RETURN_NOT_OK(validate_batch_schema(*batch.schema(), DataKind::Mbo));
    ARROW_ASSIGN_OR_RAISE(
        const auto event_time,
        detail::column<arrow::Int64Array>(batch, column_names::event_time, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto source_receive_time,
        detail::column<arrow::Int64Array>(batch, column_names::source_receive_time, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto sequence,
        detail::column<arrow::UInt64Array>(batch, column_names::sequence, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto order_id,
        detail::column<arrow::UInt64Array>(batch, column_names::order_id, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto price,
        detail::column<arrow::Int64Array>(batch, column_names::price, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto quantity,
        detail::column<arrow::Int64Array>(batch, column_names::quantity, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto source_flags,
        detail::column<arrow::UInt64Array>(batch, column_names::source_flags, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto channel_id,
        detail::column<arrow::UInt32Array>(batch, column_names::channel_id, arrow::Type::UINT32));
    ARROW_ASSIGN_OR_RAISE(
        const auto control,
        detail::column<arrow::UInt32Array>(batch, column_names::control, arrow::Type::UINT32));

    data::MboBuffer result{
        data::MboStreamContext{stream.instrument_id, stream.venue_id, stream.source_id},
        static_cast<std::size_t>(batch.num_rows()),
    };
    for (int64_t index = 0; index < batch.num_rows(); ++index) {
        if (event_time->IsNull(index) || source_receive_time->IsNull(index) || sequence->IsNull(index) ||
            order_id->IsNull(index) || price->IsNull(index) || quantity->IsNull(index) ||
            source_flags->IsNull(index) || channel_id->IsNull(index) || control->IsNull(index)) {
            return arrow::Status::Invalid("MBO physical columns must not contain null values");
        }
        const data::MboRecord record{
            event_time->Value(index),
            source_receive_time->Value(index),
            sequence->Value(index),
            order_id->Value(index),
            price->Value(index),
            quantity->Value(index),
            source_flags->Value(index),
            channel_id->Value(index),
            control->Value(index),
        };
        try {
            result.append(detail::event_from_record(record, stream));
            const auto& normalized = result.data()[result.size() - 1];
            if (normalized.control != record.control || normalized.source_receive_time != record.source_receive_time ||
                normalized.sequence != record.sequence || normalized.channel_id != record.channel_id ||
                normalized.order_id != record.order_id || normalized.price != record.price || normalized.quantity != record.quantity) {
                return arrow::Status::Invalid("noncanonical MBO control/presence representation");
            }
        } catch (const std::exception&) {
            return arrow::Status::Invalid("Arrow batch contains an invalid canonical MBO record");
        }
    }
    return result;
}

[[nodiscard]] inline arrow::Result<std::vector<data::Trade>> trades_from_arrow(
    const arrow::RecordBatch& batch,
    const StreamIdentity& stream) {
    ARROW_RETURN_NOT_OK(batch.ValidateFull());
    ARROW_RETURN_NOT_OK(validate_batch_schema(*batch.schema(), DataKind::Trades));
    ARROW_ASSIGN_OR_RAISE(
        const auto event_time,
        detail::column<arrow::Int64Array>(batch, column_names::event_time, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto source_receive_time,
        detail::column<arrow::Int64Array>(batch, column_names::source_receive_time, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto sequence,
        detail::column<arrow::UInt64Array>(batch, column_names::sequence, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto channel_id,
        detail::column<arrow::UInt32Array>(batch, column_names::channel_id, arrow::Type::UINT32));
    ARROW_ASSIGN_OR_RAISE(
        const auto source_flags,
        detail::column<arrow::UInt64Array>(batch, column_names::source_flags, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto price,
        detail::column<arrow::Int64Array>(batch, column_names::price, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto quantity,
        detail::column<arrow::Int64Array>(batch, column_names::quantity, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto aggressor_side,
        detail::column<arrow::Int8Array>(batch, column_names::aggressor_side, arrow::Type::INT8));

    std::vector<data::Trade> result;
    result.reserve(static_cast<std::size_t>(batch.num_rows()));
    for (int64_t index = 0; index < batch.num_rows(); ++index) {
        if (event_time->IsNull(index) || source_flags->IsNull(index) || price->IsNull(index) ||
            quantity->IsNull(index)) {
            return arrow::Status::Invalid("Trade required columns must not contain null values");
        }
        if (!aggressor_side->IsNull(index) && aggressor_side->Value(index) != 0 &&
            aggressor_side->Value(index) != 1) {
            return arrow::Status::Invalid("Trade aggressor side must be encoded as 0, 1, or null");
        }
        try {
            result.push_back(data::Trade{
                detail::header(
                    stream,
                    event_time,
                    source_receive_time,
                    sequence,
                    channel_id,
                    source_flags,
                    index),
                data::Price::from_raw(price->Value(index)),
                data::Quantity::from_raw(quantity->Value(index)),
                detail::optional_side(aggressor_side, index),
            });
            if (!data::is_valid(data::validate(result.back()))) { return arrow::Status::Invalid("invalid canonical Trade"); }
        } catch (const std::exception&) {
            return arrow::Status::Invalid("Arrow batch contains an invalid canonical Trade");
        }
    }
    return result;
}

[[nodiscard]] inline arrow::Result<std::vector<data::OrderExecution>> executions_from_arrow(
    const arrow::RecordBatch& batch,
    const StreamIdentity& stream) {
    ARROW_RETURN_NOT_OK(batch.ValidateFull());
    ARROW_RETURN_NOT_OK(validate_batch_schema(*batch.schema(), DataKind::OrderExecution));
    ARROW_ASSIGN_OR_RAISE(
        const auto event_time,
        detail::column<arrow::Int64Array>(batch, column_names::event_time, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto source_receive_time,
        detail::column<arrow::Int64Array>(batch, column_names::source_receive_time, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto sequence,
        detail::column<arrow::UInt64Array>(batch, column_names::sequence, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto channel_id,
        detail::column<arrow::UInt32Array>(batch, column_names::channel_id, arrow::Type::UINT32));
    ARROW_ASSIGN_OR_RAISE(
        const auto source_flags,
        detail::column<arrow::UInt64Array>(batch, column_names::source_flags, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto resting_order_id,
        detail::column<arrow::UInt64Array>(batch, column_names::resting_order_id, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto executed_quantity,
        detail::column<arrow::Int64Array>(batch, column_names::executed_quantity, arrow::Type::INT64));
    ARROW_ASSIGN_OR_RAISE(
        const auto price,
        detail::column<arrow::Int64Array>(batch, column_names::price, arrow::Type::INT64));

    std::vector<data::OrderExecution> result;
    result.reserve(static_cast<std::size_t>(batch.num_rows()));
    for (int64_t index = 0; index < batch.num_rows(); ++index) {
        if (event_time->IsNull(index) || source_flags->IsNull(index) || resting_order_id->IsNull(index) ||
            executed_quantity->IsNull(index)) {
            return arrow::Status::Invalid("OrderExecution required columns must not contain null values");
        }
        try {
            result.push_back(data::OrderExecution{
                detail::header(
                    stream,
                    event_time,
                    source_receive_time,
                    sequence,
                    channel_id,
                    source_flags,
                    index),
                data::OrderId{resting_order_id->Value(index)},
                data::Quantity::from_raw(executed_quantity->Value(index)),
                price->IsNull(index) ? std::nullopt
                                     : std::optional<data::Price>{data::Price::from_raw(price->Value(index))},
            });
            if (!data::is_valid(data::validate(result.back()))) { return arrow::Status::Invalid("invalid canonical execution"); }
        } catch (const std::exception&) {
            return arrow::Status::Invalid("Arrow batch contains an invalid canonical OrderExecution");
        }
    }
    return result;
}

using CanonicalEvent = integrity::Observation;

[[nodiscard]] inline arrow::Result<std::vector<CanonicalEvent>> events_from_arrow(
    const arrow::RecordBatch& batch,
    const DataKind kind,
    const StreamIdentity& stream) {
    std::vector<CanonicalEvent> result;
    switch (kind) {
    case DataKind::Mbo: {
        ARROW_ASSIGN_OR_RAISE(const auto buffer, from_arrow(batch, stream));
        result.reserve(buffer.size());
        for (const auto view : buffer) {
            result.emplace_back(view.event());
        }
        break;
    }
    case DataKind::Trades: {
        ARROW_ASSIGN_OR_RAISE(const auto trades, trades_from_arrow(batch, stream));
        result.reserve(trades.size());
        for (const auto& trade : trades) {
            result.emplace_back(trade);
        }
        break;
    }
    case DataKind::OrderExecution: {
        ARROW_ASSIGN_OR_RAISE(const auto executions, executions_from_arrow(batch, stream));
        result.reserve(executions.size());
        for (const auto& execution : executions) {
            result.emplace_back(execution);
        }
        break;
    }
    default: return arrow::Status::Invalid("unknown canonical data kind");
    }
    return result;
}

inline constexpr std::string_view supported_encoding = "quant-observations-v2";

[[nodiscard]] inline DataKind kind_of(const CanonicalEvent& event) {
    return event.index() == 0 ? DataKind::Mbo : event.index() == 1 ? DataKind::Trades : DataKind::OrderExecution;
}

[[nodiscard]] inline arrow::Status validate_bound_schema(const arrow::Schema& schema, const DataKind kind) {
    ARROW_RETURN_NOT_OK(validate_batch_schema(schema, kind));
    for (const auto name : {column_names::stream_id, column_names::event_ordinal}) {
        const auto index = schema.GetFieldIndex(std::string{name});
        if (index < 0 || schema.field(index)->type()->id() != arrow::Type::UINT64 || schema.field(index)->nullable()) {
            return arrow::Status::Invalid("missing/invalid nonnullable uint64 binding column: ", name);
        }
    }
    return arrow::Status::OK();
}

[[nodiscard]] inline arrow::Result<ArrowBatch> to_bound_arrow(
    const DataKind kind, const std::span<const integrity::BoundObservation> rows,
    const integrity::StreamBinding& binding, const std::string& acquisition_id) {
    try { integrity::validate_token(acquisition_id); }
    catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
    data::MboBuffer mbo{{binding.stream.instrument_id, binding.stream.venue_id, binding.stream.source_id}};
    std::vector<data::Trade> trades;
    std::vector<data::OrderExecution> executions;
    arrow::UInt64Builder stream_ids;
    arrow::UInt64Builder ordinals;
    std::optional<std::uint64_t> previous;
    for (const auto& row : rows) {
        const auto& original = integrity::header(row.event);
        if (row.stream_id != binding.id ||
            StreamIdentity{original.instrument_id, original.venue_id, original.source_id} != binding.stream ||
            kind_of(row.event) != kind || (previous && row.ordinal <= *previous)) {
            return arrow::Status::Invalid("wrong stream/kind/ordinal association before conversion");
        }
        if (!std::visit([](const auto& event) { return data::is_valid(data::validate(event)); }, row.event)) {
            return arrow::Status::Invalid("invalid canonical observation before conversion");
        }
        try {
            if (kind == DataKind::Mbo) { mbo.append(std::get<data::MboEvent>(row.event)); }
            else if (kind == DataKind::Trades) { trades.push_back(std::get<data::Trade>(row.event)); }
            else if (kind == DataKind::OrderExecution) { executions.push_back(std::get<data::OrderExecution>(row.event)); }
            else { return arrow::Status::Invalid("unsupported data kind"); }
        } catch (const std::exception& error) { return arrow::Status::Invalid(error.what()); }
        ARROW_RETURN_NOT_OK(stream_ids.Append(binding.id.value()));
        ARROW_RETURN_NOT_OK(ordinals.Append(row.ordinal));
        previous = row.ordinal;
    }
    ArrowBatch batch;
    if (kind == DataKind::Mbo) { ARROW_ASSIGN_OR_RAISE(batch, to_arrow(mbo)); }
    else if (kind == DataKind::Trades) { ARROW_ASSIGN_OR_RAISE(batch, to_arrow(std::span<const data::Trade>{trades})); }
    else if (kind == DataKind::OrderExecution) { ARROW_ASSIGN_OR_RAISE(batch, to_arrow(std::span<const data::OrderExecution>{executions})); }
    else { return arrow::Status::Invalid("unsupported data kind"); }
    std::shared_ptr<arrow::Array> ids;
    std::shared_ptr<arrow::Array> positions;
    ARROW_RETURN_NOT_OK(stream_ids.Finish(&ids));
    ARROW_RETURN_NOT_OK(ordinals.Finish(&positions));
    ARROW_ASSIGN_OR_RAISE(batch, batch->AddColumn(batch->num_columns(), arrow::field("stream_id", arrow::uint64(), false), ids));
    ARROW_ASSIGN_OR_RAISE(batch, batch->AddColumn(batch->num_columns(), arrow::field("event_ordinal", arrow::uint64(), false), positions));
    return batch->ReplaceSchemaMetadata(arrow::KeyValueMetadata::Make(
        {"quant.encoding", "quant.acquisition_id", "quant.stream_id", "quant.instrument_id", "quant.venue_id", "quant.source_id"},
        {std::string{supported_encoding}, acquisition_id, std::to_string(binding.id.value()),
         std::to_string(binding.stream.instrument_id.value()), std::to_string(binding.stream.venue_id.value()),
         std::to_string(binding.stream.source_id.value())}));
}

[[nodiscard]] inline arrow::Result<std::vector<integrity::BoundObservation>> bound_from_arrow(
    const arrow::RecordBatch& batch, const DataKind kind, const integrity::StreamBinding& binding) {
    ARROW_RETURN_NOT_OK(batch.ValidateFull());
    ARROW_RETURN_NOT_OK(validate_bound_schema(*batch.schema(), kind));
    const auto metadata = batch.schema()->metadata();
    if (!metadata) { return arrow::Status::Invalid("binding metadata is absent"); }
    for (const auto& [key, expected] : std::vector<std::pair<std::string, std::string>>{
        {"quant.encoding", std::string{supported_encoding}}, {"quant.stream_id", std::to_string(binding.id.value())},
        {"quant.instrument_id", std::to_string(binding.stream.instrument_id.value())},
        {"quant.venue_id", std::to_string(binding.stream.venue_id.value())}, {"quant.source_id", std::to_string(binding.stream.source_id.value())}}) {
        if (!metadata->Contains(key)) { return arrow::Status::Invalid("missing binding metadata: ", key); }
        ARROW_ASSIGN_OR_RAISE(const auto actual, metadata->Get(key));
        if (actual != expected) { return arrow::Status::Invalid("binding metadata disagrees with original stream: ", key); }
    }
    ARROW_ASSIGN_OR_RAISE(const auto ids, detail::column<arrow::UInt64Array>(batch, column_names::stream_id, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(const auto ordinals, detail::column<arrow::UInt64Array>(batch, column_names::event_ordinal, arrow::Type::UINT64));
    ARROW_ASSIGN_OR_RAISE(const auto events, events_from_arrow(batch, kind, binding.stream));
    std::vector<integrity::BoundObservation> result;
    std::optional<std::uint64_t> previous;
    for (int64_t index = 0; index < batch.num_rows(); ++index) {
        if (ids->IsNull(index) || ordinals->IsNull(index) || ids->Value(index) != binding.id.value() ||
            (previous && ordinals->Value(index) <= *previous)) {
            return arrow::Status::Invalid("wrong/reordered/duplicate persisted StreamId/ordinal");
        }
        result.push_back({binding.id, ordinals->Value(index), events[static_cast<std::size_t>(index)]});
        previous = ordinals->Value(index);
    }
    return result;
}

} // namespace quant::storage
