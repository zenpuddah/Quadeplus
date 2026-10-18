#pragma once

#include "quant/storage/arrow_interchange.hpp"

#include <functional>
#include <span>

namespace quant::storage {

using BatchConsumer = std::function<arrow::Status(const ArrowBatch&)>;
using EventConsumer = std::function<arrow::Status(const CanonicalEvent&)>;
using SequencedEventConsumer = std::function<arrow::Status(const integrity::BoundObservation&)>;

class MarketDataReader {
public:
    virtual ~MarketDataReader() = default;

    [[nodiscard]] virtual arrow::Result<DatasetLocalMetadata> read_metadata(
        const DataFragment& fragment) const = 0;

    virtual arrow::Status read_batches(
        const DataFragment& fragment,
        BatchProjection projection,
        const BatchConsumer& consumer) const = 0;

    virtual arrow::Status stream_events(
        const DataFragment& fragment,
        const EventConsumer& consumer) const = 0;

    virtual arrow::Status read_batches_checked(
        const DataFragment&, BatchProjection, integrity::ConsumerRequirements, const BatchConsumer&) const {
        return arrow::Status::NotImplemented("checked consumer access is unsupported by this reader");
    }

    virtual arrow::Status stream_ordered_events(
        std::span<const DataFragment>, integrity::ConsumerRequirements, const SequencedEventConsumer&) const {
        return arrow::Status::NotImplemented("acquisition-scoped merged replay is unsupported by this reader");
    }
};

class StorageBackend : public MarketDataReader {
public:
    ~StorageBackend() override = default;

    [[nodiscard]] virtual arrow::Result<DataFragment> write(
        const DatasetLocalMetadata& metadata,
        const std::span<const ArrowBatch> batches) = 0;
};

} // namespace quant::storage
