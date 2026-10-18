#pragma once

#include "quant/data/observations.hpp"
#include "quant/ingestion/instrument_mapping.hpp"
#include "quant/ingestion/result.hpp"
#include "quant/integrity/model.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quant::ingestion {

struct ProviderBatch {
    std::vector<data::MboEvent> mbo_events;
    std::vector<DataQualityObservation> quality_observations;
    std::vector<data::SourceId> source_ids;
    std::vector<SourceArtifactProvenance> source_artifacts;
    std::vector<data::Trade> trades;
    std::vector<data::OrderExecution> order_executions;
    std::vector<data::ProviderRecordMetadata> provider_records;
    std::vector<ProviderBatchMetadata> batch_metadata;
};

class ProviderPort {
public:
    virtual ~ProviderPort() = default;
    [[nodiscard]] virtual std::string_view provider_name() const noexcept = 0;
    virtual ProviderBatch fetch(const MarketDataQuery& query, const ProviderMappingSegment& segment) = 0;
    virtual integrity::Acquisition acquire(integrity::Acquisition, integrity::AcquisitionSink&) {
        throw std::logic_error("provider does not implement retained acquisition");
    }
};

class FakeProvider final : public ProviderPort {
public:
    explicit FakeProvider(std::string provider) : provider_(std::move(provider)) {
        if (provider_.empty()) {
            throw std::invalid_argument("fake provider name must not be empty");
        }
    }

    [[nodiscard]] std::string_view provider_name() const noexcept override { return provider_; }

    void enqueue_response(ProviderBatch response) { responses_.push_back(std::move(response)); }

    void enqueue_acquisition(integrity::Acquisition acquisition) { acquisitions_.push_back(std::move(acquisition)); }
    [[nodiscard]] std::size_t retained_requests() const noexcept { return retained_requests_; }

    integrity::Acquisition acquire(integrity::Acquisition request, integrity::AcquisitionSink& sink) override {
        ++retained_requests_;
        sink.begin(request);
        if (next_acquisition_ >= acquisitions_.size()) {
            request.outcome = integrity::AcquisitionOutcome::Failed;
            request.operation_failure = "fake provider has no retained acquisition";
            sink.finish(request);
            return request;
        }
        auto result = std::move(acquisitions_[next_acquisition_++]);
        result.operation_id = request.operation_id;
        result.acquisition_id = request.acquisition_id;
        result.attempt_id = request.attempt_id;
        result.acquired_at = request.acquired_at;
        result.query = request.query;
        result.mapping = request.mapping;
        result.provider_request = request.provider_request;
        sink.metadata(result);
        for (const auto& record : result.records) { sink.record(record); }
        sink.finish(result);
        return result;
    }

    ProviderBatch fetch(const MarketDataQuery& query, const ProviderMappingSegment& segment) override {
        validate(query);
        validate(segment);
        if (segment.provider != provider_) {
            throw std::invalid_argument("provider mapping does not match provider port");
        }
        if (next_response_ >= responses_.size()) {
            throw std::logic_error("fake provider has no response for requested segment");
        }
        calls_.push_back(segment);
        queries_.push_back(query);
        return std::move(responses_[next_response_++]);
    }

    [[nodiscard]] const std::vector<ProviderMappingSegment>& calls() const noexcept { return calls_; }
    [[nodiscard]] const std::vector<MarketDataQuery>& queries() const noexcept { return queries_; }

private:
    std::string provider_;
    std::vector<ProviderBatch> responses_;
    std::vector<ProviderMappingSegment> calls_;
    std::vector<MarketDataQuery> queries_;
    std::size_t next_response_ = 0;
    std::vector<integrity::Acquisition> acquisitions_;
    std::size_t next_acquisition_ = 0;
    std::size_t retained_requests_ = 0;
};

} // namespace quant::ingestion
