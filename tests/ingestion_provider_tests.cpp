#include "quant/ingestion/instrument_mapping.hpp"
#include "quant/ingestion/provider.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
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

template <typename Function>
void require_throws(Function&& function, const std::string_view message) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}

Timestamp time(const std::int64_t nanos) {
    return Timestamp::from_unix_nanos(nanos);
}

TimeRange range(const std::int64_t start, const std::int64_t end) {
    return TimeRange{time(start), time(end)};
}

ProviderMappingSegment mapping(
    const std::int64_t start,
    const std::int64_t end,
    const std::string_view symbol,
    const std::optional<std::string> provider_id) {
    return ProviderMappingSegment{
        InstrumentId{42},
        "test-provider",
        std::string{symbol},
        VenueId{7},
        provider_id,
        range(start, end),
    };
}

void test_mapping_segments() {
    InMemoryInstrumentMappingRegistry registry;
    registry.add(mapping(0, 10, "OLD", std::nullopt));
    registry.add(mapping(10, 20, "NEW", std::string{"provider-2"}));

    const auto first = registry.resolve(InstrumentId{42}, "test-provider", range(2, 8));
    require(first.size() == 1, "one canonical instrument must resolve to one provider segment");
    require(first[0].symbol == std::optional<std::string>{"OLD"}, "mapping must retain the historical symbol");
    require(!first[0].provider_id.has_value(), "provider id must be optional");
    require(first[0].range == range(2, 8), "resolved mapping must be clipped to the requested range");

    const auto spanning = registry.resolve(InstrumentId{42}, "test-provider", range(5, 15));
    require(spanning.size() == 2, "a historical mapping boundary must split the canonical range");
    require(spanning[0].range == range(5, 10), "first mapping segment must end at the symbol boundary");
    require(spanning[0].symbol == std::optional<std::string>{"OLD"}, "first segment must use the old symbol");
    require(spanning[1].range == range(10, 15), "second mapping segment must start at the symbol boundary");
    require(spanning[1].symbol == std::optional<std::string>{"NEW"}, "second segment must use the new symbol");
    require(spanning[1].provider_id == std::optional<std::string>{"provider-2"},
            "second segment must retain its optional provider id");
}

void test_mapping_scope_overlap() {
    InMemoryInstrumentMappingRegistry registry;
    registry.add(ProviderMappingSegment{
        InstrumentId{42},
        "test-provider",
        std::string{"VENUE-1"},
        VenueId{7},
        std::nullopt,
        range(0, 10),
        SourceId{9},
    });
    registry.add(ProviderMappingSegment{
        InstrumentId{42},
        "test-provider",
        std::string{"VENUE-1-SOURCE-2"},
        VenueId{7},
        std::nullopt,
        range(0, 10),
        SourceId{10},
    });
    registry.add(ProviderMappingSegment{
        InstrumentId{42},
        "test-provider",
        std::string{"VENUE-2"},
        VenueId{8},
        std::nullopt,
        range(0, 10),
        SourceId{9},
    });

    require_throws(
        [&] {
            registry.add(ProviderMappingSegment{
                InstrumentId{42},
                "test-provider",
                std::string{"CONFLICT"},
                VenueId{7},
                std::nullopt,
                range(5, 6),
                SourceId{9},
            });
        },
        "overlapping mappings in one stream scope must be rejected");
    require(registry.resolve(InstrumentId{42}, "test-provider", range(0, 10)).size() == 3,
            "different venue/source scopes must coexist in the mapping registry");
}

EventHeader header() {
    return EventHeader{
        InstrumentId{42},
        VenueId{7},
        time(3),
        time(4),
        std::uint64_t{8},
        std::uint32_t{1},
        std::uint64_t{5},
        SourceId{9},
    };
}

void test_provider_returns_canonical_batch() {
    FakeProvider provider{"test-provider"};
    const MboEvent event{
        header(),
        MboAction::Add,
        OrderId{100},
        Side::Buy,
        Price::from_integer(10),
        Quantity::from_integer(2),
    };
    provider.enqueue_response(ProviderBatch{
        {event},
        {DataQualityObservation{range(0, 10), DataQualityKind::Complete}},
        {SourceId{9}},
        {},
        {},
        {},
        {},
        {},
    });

    const auto requested_segment = mapping(0, 10, "OLD", std::nullopt);
    const auto requested_query = MarketDataQuery{
        InstrumentId{42},
        VenueId{7},
        MarketDataLevel::L3,
        range(0, 10),
    };
    const auto response = provider.fetch(requested_query, requested_segment);
    require(provider.provider_name() == "test-provider", "provider identity must remain behind the provider port");
    require(provider.calls().size() == 1, "provider fake must record one synchronous request");
    require(provider.calls()[0].range == range(0, 10), "provider fake must receive the resolved canonical range");
    require(response.mbo_events.size() == 1, "provider fake must return canonical MBO events");
    require(response.mbo_events[0].action == MboAction::Add, "provider output must use canonical MBO actions");
    require(response.mbo_events[0].header.instrument_id == InstrumentId{42},
            "provider output must retain canonical instrument identity");
    require(response.quality_observations[0].kind == DataQualityKind::Complete,
            "provider quality evidence must use the canonical quality vocabulary");
}

} // namespace

int main() {
    try {
        test_mapping_segments();
        test_mapping_scope_overlap();
        test_provider_returns_canonical_batch();
        std::cout << "ingestion_provider_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "ingestion_provider_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
