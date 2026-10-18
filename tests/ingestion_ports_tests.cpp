#include "quant/ingestion/cache.hpp"
#include "quant/ingestion/recovery.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace quant::data;
using namespace quant::ingestion;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Timestamp time(const std::int64_t nanos) {
    return Timestamp::from_unix_nanos(nanos);
}

void test_noop_cache() {
    const MarketDataQuery query{InstrumentId{42}, std::nullopt, MarketDataLevel::L3, TimeRange{time(10), time(20)}};
    NoopCache cache;

    const auto lookup = cache.lookup(query);
    require(lookup.status == CacheStatus::Miss, "NoopCache must always report a miss");
    require(lookup.covered.empty(), "NoopCache must not report covered ranges");
    require(
        lookup.missing == std::vector<TimeRange>{query.primary_range()},
        "NoopCache must miss the full primary canonical range");
}

void test_noop_recovery() {
    const NoopRecoveryPolicy policy;
    const MarketDataQuery query{
        InstrumentId{42},
        std::nullopt,
        MarketDataLevel::L3,
        TimeRange{time(10), time(20)},
    };
    const std::vector<DataStateSegment> state_segments{
        DataStateSegment{query.primary_range(), DataState::Unknown},
    };
    const RecoveryContext context{
        query,
        state_segments,
        state_segments,
        DataState::Unknown,
    };
    const auto plan = policy.decide(context);
    require(plan.action == RecoveryAction::NoAction, "NoopRecoveryPolicy must always do nothing");
    require(plan.ranges.empty(), "NoopRecoveryPolicy must not schedule recovery ranges");
}

} // namespace

int main() {
    try {
        test_noop_cache();
        test_noop_recovery();
        std::cout << "ingestion_ports_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "ingestion_ports_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
