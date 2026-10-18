#pragma once

#include "quant/ingestion/query.hpp"

#include <stdexcept>
#include <vector>

namespace quant::ingestion {

enum class CacheStatus {
    Miss,
    FullHit,
    PartialHit,
};

struct CacheLookup {
    CacheStatus status;
    std::vector<TimeRange> covered;
    std::vector<TimeRange> missing;
};

class CachePort {
public:
    virtual ~CachePort() = default;
    virtual CacheLookup lookup(const MarketDataQuery& query) = 0;
};

class NoopCache final : public CachePort {
public:
    [[nodiscard]] CacheLookup lookup(const MarketDataQuery& query) override {
        validate(query);
        if (!query.event_time_range) {
            throw std::invalid_argument("cache lookup requires an event-time range for mapping resolution");
        }
        return CacheLookup{CacheStatus::Miss, {}, {*query.event_time_range}};
    }
};

} // namespace quant::ingestion
