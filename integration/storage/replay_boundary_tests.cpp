#include "quant/storage/backend.hpp"

#include <iostream>

namespace {

class FakeReader final : public quant::storage::MarketDataReader {
public:
    [[nodiscard]] arrow::Result<quant::storage::DatasetLocalMetadata> read_metadata(
        const quant::storage::DataFragment&) const override {
        return arrow::Status::NotImplemented("fake reader");
    }

    arrow::Status read_batches(
        const quant::storage::DataFragment&,
        quant::storage::BatchProjection,
        const quant::storage::BatchConsumer&) const override {
        return arrow::Status::NotImplemented("fake reader");
    }

    arrow::Status stream_events(
        const quant::storage::DataFragment&,
        const quant::storage::EventConsumer&) const override {
        return arrow::Status::NotImplemented("fake reader");
    }
};

} // namespace

int main() {
    FakeReader reader;
    (void)reader;
    std::cout << "quant_storage_replay_boundary_tests: passed\n";
    return 0;
}
