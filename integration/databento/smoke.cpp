#include "quant/ingestion/databento_provider.hpp"
#include "quant/ingestion/synchronous_ingestor.hpp"

#include <databento/historical.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace quant::data;
using namespace quant::ingestion;

Timestamp time(const std::int64_t nanos) {
    return Timestamp::from_unix_nanos(nanos);
}

DataState state(const IngestionResult& result) {
    return result.metadata.data_state;
}

const char* state_name(const DataState value) {
    switch (value) {
    case DataState::Unknown:
        return "unknown";
    case DataState::Complete:
        return "complete";
    case DataState::Degraded:
        return "degraded";
    case DataState::Missing:
        return "missing";
    case DataState::Corrupt:
        return "corrupt";
    }
    return "unknown";
}

const char* quality_name(const DataQualityKind value) {
    switch (value) {
    case DataQualityKind::Complete:
        return "complete";
    case DataQualityKind::Degraded:
        return "degraded";
    case DataQualityKind::Pending:
        return "pending";
    case DataQualityKind::Missing:
        return "missing";
    case DataQualityKind::Corrupt:
        return "corrupt";
    case DataQualityKind::SequenceGap:
        return "sequence_gap";
    }
    return "unknown";
}

void print_scope(const std::optional<MboStreamContext>& scope) {
    if (!scope) {
        std::cout << "none";
        return;
    }
    std::cout << "instrument=" << scope->instrument_id.value() << " venue=" << scope->venue_id.value()
              << " source=" << scope->source_id.value();
}

double estimate_cost(::databento::Historical& client, const std::string& symbol, const TimeRange range) {
    const MarketDataQuery query{InstrumentId{1}, VenueId{1}, MarketDataLevel::L3, range};
    const auto provider_range = DatabentoProvider::resolve_provider_range(query);
    return client.MetadataGetCost(
        ::databento::dataset::kXnasItch,
        provider_range,
        std::vector<std::string>{symbol},
        ::databento::Schema::Mbo,
        ::databento::SType::RawSymbol,
        std::uint64_t{0});
}

TimeRange reduce_to_one_minute(const TimeRange range) {
    constexpr auto one_minute_nanos = std::int64_t{60'000'000'000LL};
    if (range.start.unix_nanos() > std::numeric_limits<std::int64_t>::max() - one_minute_nanos) {
        throw std::overflow_error("cannot reduce smoke interval without timestamp overflow");
    }
    const auto one_minute_end = Timestamp::from_unix_nanos(range.start.unix_nanos() + one_minute_nanos);
    if (one_minute_end >= range.end) {
        throw std::invalid_argument("cost estimate exceeds target for an interval already at most one minute");
    }
    return TimeRange{range.start, one_minute_end};
}

std::int64_t parse_timestamp(const char* value) {
    std::string text{value};
    std::size_t parsed = 0;
    const auto result = std::stoll(text, &parsed);
    if (parsed != text.size()) {
        throw std::invalid_argument("timestamp arguments must be Unix nanoseconds");
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 1 && argc != 4) {
            throw std::invalid_argument("usage: quant_databento_smoke [symbol start_unix_nanos end_unix_nanos]");
        }

        const std::string symbol = argc == 4 ? argv[1] : "AAPL";
        const std::int64_t start_nanos = argc == 4 ? parse_timestamp(argv[2]) : 1'704'205'800'000'000'000LL;
        const std::int64_t end_nanos = argc == 4 ? parse_timestamp(argv[3]) : 1'704'206'100'000'000'000LL;
        TimeRange requested_range{time(start_nanos), time(end_nanos)};

        InMemoryInstrumentMappingRegistry mappings;

        DatabentoStrictLogReceiver log_receiver;
        auto client = ::databento::Historical::Builder().SetKeyFromEnv().SetLogReceiver(&log_receiver).Build();

        constexpr double target_cost_usd = 0.25;
        constexpr double hard_cost_ceiling_usd = 1.0;
        double estimated_cost_usd = estimate_cost(client, symbol, requested_range);
        std::cout << std::fixed << std::setprecision(6)
                  << "cost_estimate_usd=" << estimated_cost_usd
                  << " interval_start_unix_nanos=" << requested_range.start.unix_nanos()
                  << " interval_end_unix_nanos=" << requested_range.end.unix_nanos() << '\n';
        if (estimated_cost_usd > target_cost_usd) {
            requested_range = reduce_to_one_minute(requested_range);
            estimated_cost_usd = estimate_cost(client, symbol, requested_range);
            std::cout << "cost_estimate_usd=" << estimated_cost_usd
                      << " interval_start_unix_nanos=" << requested_range.start.unix_nanos()
                      << " interval_end_unix_nanos=" << requested_range.end.unix_nanos() << '\n';
        }
        if (!std::isfinite(estimated_cost_usd) || estimated_cost_usd > hard_cost_ceiling_usd) {
            throw std::runtime_error("cost gate refused the request: estimate is above the $1 hard ceiling");
        }
        if (estimated_cost_usd > target_cost_usd) {
            throw std::runtime_error("cost gate refused the request: estimate remains above the $0.25 target");
        }
        std::cout << "cost_gate=pass estimated_cost_usd=" << estimated_cost_usd << '\n';

        const MarketDataQuery query{
            InstrumentId{1},
            VenueId{1},
            MarketDataLevel::L3,
            requested_range,
        };
        mappings.add(ProviderMappingSegment{
            InstrumentId{1},
            "databento",
            symbol,
            VenueId{1},
            std::nullopt,
            requested_range,
            SourceId{1},
        });
        DatabentoProvider provider{client};
        NoopCache cache;
        NoopRecoveryPolicy recovery;
        DefaultDataUsabilityPolicy usability;
        SynchronousIngestor ingestor{cache, mappings, provider, recovery, usability};
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto result = ingestor.ingest(
            query,
            Timestamp::from_unix_nanos(static_cast<std::int64_t>(now)),
            "databento-cpp-v0.67.0",
            "canonical-mbo-v1",
            "manual-v1");

        std::size_t mbo_count = 0;
        for (const auto& buffer : result.mbo_buffers) {
            mbo_count += buffer.size();
        }
        std::size_t source_receive_present = 0;
        std::size_t source_receive_missing = 0;
        std::optional<std::int64_t> minimum_source_receive;
        std::optional<std::int64_t> maximum_source_receive;
        std::size_t sequence_present = 0;
        std::optional<std::uint64_t> minimum_sequence;
        std::optional<std::uint64_t> maximum_sequence;
        std::size_t channel_present = 0;
        std::set<std::uint32_t> channels;
        std::set<std::uint64_t> flags;
        for (const auto& record : result.metadata.provider_records) {
            const auto& header = record.header;
            if (header.source_receive_time) {
                ++source_receive_present;
                const auto nanos = header.source_receive_time->unix_nanos();
                minimum_source_receive = minimum_source_receive
                    ? std::optional<std::int64_t>{std::min(*minimum_source_receive, nanos)}
                    : std::optional<std::int64_t>{nanos};
                maximum_source_receive = maximum_source_receive
                    ? std::optional<std::int64_t>{std::max(*maximum_source_receive, nanos)}
                    : std::optional<std::int64_t>{nanos};
            } else {
                ++source_receive_missing;
            }
            if (header.sequence) {
                ++sequence_present;
                minimum_sequence = minimum_sequence
                    ? std::optional<std::uint64_t>{std::min(*minimum_sequence, *header.sequence)}
                    : std::optional<std::uint64_t>{*header.sequence};
                maximum_sequence = maximum_sequence
                    ? std::optional<std::uint64_t>{std::max(*maximum_sequence, *header.sequence)}
                    : std::optional<std::uint64_t>{*header.sequence};
            }
            if (header.channel_id) {
                ++channel_present;
                channels.insert(*header.channel_id);
            }
            flags.insert(header.source_flags);
        }

        std::size_t corrupt_quality_observations = 0;
        for (const auto& observation : result.metadata.quality_observations) {
            if (observation.kind == DataQualityKind::Corrupt) {
                ++corrupt_quality_observations;
            }
        }

        std::cout << "provider=databento dataset=XNAS.ITCH schema=MBO symbol=" << symbol << '\n'
                  << "interval_start_unix_nanos=" << requested_range.start.unix_nanos()
                  << " interval_end_unix_nanos=" << requested_range.end.unix_nanos() << '\n'
                  << "provider_records_received=" << result.metadata.provider_records.size()
                  << " mbo_records=" << mbo_count << " trades=" << result.trades.size()
                  << " order_executions=" << result.order_executions.size() << '\n'
                  << "data_state=" << state_name(state(result))
                  << " state_segments=" << result.metadata.data_state_segments.size() << '\n'
                  << "source_receive_time_present=" << source_receive_present
                  << " source_receive_time_missing=" << source_receive_missing;
        if (minimum_source_receive && maximum_source_receive) {
            std::cout << " source_receive_time_min=" << *minimum_source_receive
                      << " source_receive_time_max=" << *maximum_source_receive;
        }
        std::cout << '\n'
                  << "sequence_present=" << sequence_present;
        if (minimum_sequence && maximum_sequence) {
            std::cout << " sequence_min=" << *minimum_sequence << " sequence_max=" << *maximum_sequence;
        }
        std::cout << " channel_present=" << channel_present << " channels_seen=";
        for (const auto channel : channels) {
            std::cout << channel << ',';
        }
        std::cout << " flags_seen=";
        for (const auto flag : flags) {
            std::cout << flag << ',';
        }
        std::cout << " unique_channels=" << channels.size() << " unique_flags=" << flags.size() << '\n'
                  << "corrupt_quality_observations=" << corrupt_quality_observations << '\n';

        for (const auto& segment : result.metadata.data_state_segments) {
            std::cout << "state_segment start=" << segment.range.start.unix_nanos()
                      << " end=" << segment.range.end.unix_nanos()
                      << " state=" << state_name(segment.state) << " scope=";
            print_scope(segment.scope);
            std::cout << '\n';
        }
        for (const auto& batch : result.metadata.provider_batches) {
            std::cout << "provider_batch dataset=" << batch.dataset << " schema=" << batch.schema
                      << " provider_start=" << batch.provider_start << " provider_end=" << batch.provider_end
                      << " partial_symbols=" << batch.partial_symbols.size()
                      << " not_found_symbols=" << batch.not_found_symbols.size() << '\n';
            for (const auto& condition : batch.conditions) {
                std::cout << "dataset_condition date=" << condition.date
                          << " condition=" << condition.condition;
                if (condition.last_modified_date) {
                    std::cout << " last_modified_date=" << *condition.last_modified_date;
                }
                std::cout << '\n';
            }
        }
        for (const auto& observation : result.metadata.quality_observations) {
            std::cout << "quality_observation kind=" << quality_name(observation.kind)
                      << " start=" << observation.range.start.unix_nanos()
                      << " end=" << observation.range.end.unix_nanos() << " scope=";
            print_scope(observation.scope);
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "quant_databento_smoke: failed: " << error.what() << '\n';
        return 1;
    }
}
