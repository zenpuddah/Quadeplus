#include "quant/ingestion/databento_provider.hpp"

#include <databento/record.hpp>

#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace quant::data;
using namespace quant::ingestion;
namespace integrity = quant::integrity;

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

ProviderMappingSegment mapping() {
    return ProviderMappingSegment{
        InstrumentId{42},
        "databento",
        std::string{"TEST"},
        VenueId{7},
        std::nullopt,
        range(100, 200),
        SourceId{9},
    };
}

::databento::MboMsg message(
    const ::databento::Action action,
    const std::uint64_t order_id = 100,
    const std::int64_t price = 101'000'000'000,
    const std::uint32_t size = 2,
    const ::databento::Side side = ::databento::Side::Bid,
    const std::uint8_t flags = 0) {
    return ::databento::MboMsg{
        ::databento::RecordHeader{
            sizeof(::databento::MboMsg) / ::databento::RecordHeader::kLengthMultiplier,
            ::databento::RType::Mbo,
            static_cast<std::uint16_t>(::databento::Publisher::XnasItchXnas),
            9001,
            ::databento::UnixNanos{std::chrono::nanoseconds{120}},
        },
        order_id,
        price,
        size,
        ::databento::FlagSet{flags},
        3,
        action,
        side,
        ::databento::UnixNanos{std::chrono::nanoseconds{130}},
        ::databento::TimeDeltaNanos{1},
        55,
    };
}

void test_mbo_action_interpretation() {
    const auto segment = mapping();

    const auto add = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Add), segment);
    require(add.mbo_events.size() == 1, "Databento Add must become one MBO event");
    require(add.mbo_events[0].action == MboAction::Add, "Databento Add must retain canonical action");
    require(add.mbo_events[0].header.instrument_id == InstrumentId{42},
            "Databento records must use the mapped canonical instrument");
    require(add.mbo_events[0].header.event_time == time(120), "event timestamp must be preserved");
    require(add.mbo_events[0].header.source_receive_time == std::optional<Timestamp>{time(130)},
            "provider receive timestamp must be preserved");
    require(add.mbo_events[0].header.source_flags == 0, "source flags must be preserved");
    require(add.provider_records.size() == 1 && add.provider_records[0].provider_instrument_id == 9001,
            "provider instrument identity must stay in the sidecar");

    const auto trade = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Trade, 0, 102'000'000'000, 4, ::databento::Side::Ask), segment);
    require(trade.mbo_events.empty(), "Databento Trade must not mutate the MBO buffer");
    require(trade.trades.size() == 1, "Databento Trade must become one canonical trade");
    require(trade.trades[0].price == Price::from_raw(102'000'000'000) &&
                trade.trades[0].quantity == Quantity::from_integer(4) &&
                trade.trades[0].aggressor_side == std::optional<Side>{Side::Sell},
            "trade fields must be normalized");

    const auto modify = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Modify, 100, 104'000'000'000, 5), segment);
    require(modify.mbo_events.size() == 1 && modify.mbo_events[0].action == MboAction::Modify &&
                modify.mbo_events[0].quantity == std::optional<Quantity>{Quantity::from_integer(5)},
            "Databento Modify must retain its changed order fields");

    const auto fill = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Fill, 101, 103'000'000'000, 3), segment);
    require(fill.mbo_events.empty() && fill.trades.empty(), "Databento Fill must not mutate the MBO buffer");
    require(fill.order_executions.size() == 1, "Databento Fill must become one order execution");
    require(fill.order_executions[0].resting_order_id == OrderId{101} &&
                fill.order_executions[0].executed_quantity == Quantity::from_integer(3) &&
                fill.order_executions[0].price == std::optional<Price>{Price::from_raw(103'000'000'000)},
            "execution fields must retain resting-order evidence");

    const auto cancel = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Cancel, 101, 0, 3), segment);
    require(cancel.mbo_events.size() == 1 && cancel.mbo_events[0].action == MboAction::Cancel,
            "Databento Cancel must be the displayed-book mutation");

    const auto clear = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Clear, 0, 0, 0, ::databento::Side::None), segment);
    require(clear.mbo_events.size() == 1 && clear.mbo_events[0].action == MboAction::Clear,
            "Databento Clear must become a canonical clear");

    const auto none = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::None, 0, 0, 0, ::databento::Side::None, ::databento::FlagSet::kLast), segment);
    require(none.mbo_events.empty() && none.trades.empty() && none.order_executions.empty(),
            "Databento None must not create a canonical market mutation");
    require(none.provider_records.size() == 1 && none.provider_records[0].header.source_flags == ::databento::FlagSet::kLast,
            "Databento None must remain recoverable as provider metadata");
}

void test_query_time_resolution() {
    const MarketDataQuery event_query{
        InstrumentId{42},
        VenueId{7},
        MarketDataLevel::L3,
        std::optional<TimeRange>{range(100, 200)},
        std::nullopt,
        TimeBasis::EventTime,
        FetchPolicy::DeriveProviderRange,
        TimeMargin{5, 7},
    };
    const auto derived = DatabentoProvider::resolve_provider_range(event_query);
    require(
        derived.start.time_since_epoch().count() == 95 && derived.end.time_since_epoch().count() == 207,
        "event-only queries must derive a source receive-time fetch range with margin");

    const MarketDataQuery receive_query{
        InstrumentId{42},
        VenueId{7},
        MarketDataLevel::L3,
        std::optional<TimeRange>{range(100, 200)},
        std::optional<TimeRange>{range(300, 400)},
        TimeBasis::SourceReceiveTime,
        FetchPolicy::RequireExplicitRange,
    };
    const auto explicit_range = DatabentoProvider::resolve_provider_range(receive_query);
    require(
        explicit_range.start.time_since_epoch().count() == 300 && explicit_range.end.time_since_epoch().count() == 400,
        "source receive-time queries must use their explicit provider range");

    require_throws(
        [] {
            const MarketDataQuery receive_only_query{
                InstrumentId{42},
                VenueId{7},
                MarketDataLevel::L3,
                std::nullopt,
                std::optional<TimeRange>{range(300, 400)},
                TimeBasis::SourceReceiveTime,
                FetchPolicy::RequireExplicitRange,
            };
            (void)DatabentoProvider::resolve_provider_range(receive_only_query);
        },
        "receive-only queries must be rejected until receive-time mapping exists");

    require_throws(
        [] {
            const MarketDataQuery strict_event_query{
                InstrumentId{42},
                VenueId{7},
                MarketDataLevel::L3,
                std::optional<TimeRange>{range(100, 200)},
                std::nullopt,
                TimeBasis::EventTime,
                FetchPolicy::RequireExplicitRange,
            };
            (void)DatabentoProvider::resolve_provider_range(strict_event_query);
        },
        "strict fetch policy must reject event-only Databento requests before the API call");
}

void test_corrupt_provider_record() {
    const auto corrupt = quant::ingestion::databento_detail::translate_mbo(
        message(::databento::Action::Trade, 0, ::databento::kUndefPrice, 4), mapping());
    require(corrupt.trades.empty(), "an unusable Databento trade must not enter canonical output");
    require(corrupt.quality_observations.size() == 1 &&
                corrupt.quality_observations[0].kind == DataQualityKind::Corrupt,
            "an unusable Databento trade must produce corrupt quality evidence");
}

void test_undefined_source_receive_time_is_optional() {
    auto record = message(::databento::Action::Add);
    record.ts_recv = ::databento::UnixNanos{
        ::databento::UnixNanos::duration{::databento::kUndefTimestamp}};
    const auto translated = quant::ingestion::databento_detail::translate_mbo(record, mapping());
    require(!translated.mbo_events[0].header.source_receive_time,
            "undefined provider receive timestamps must remain absent");
}

void test_provider_instrument_id_mapping() {
    auto id_mapping = mapping();
    id_mapping.symbol = std::nullopt;
    id_mapping.provider_id = std::string{"9001"};
    const auto provider_id = quant::ingestion::databento_detail::provider_instrument_id(id_mapping);
    require(provider_id && *provider_id == 9001, "numeric Databento provider IDs must be preserved from mappings");

    id_mapping.provider_id = std::string{"not-a-number"};
    require_throws(
        [&] { static_cast<void>(quant::ingestion::databento_detail::provider_instrument_id(id_mapping)); },
        "non-numeric Databento provider IDs must be rejected");
}

void test_dataset_condition_mapping() {
    using ConditionDetail = ::databento::DatasetConditionDetail;
    const auto available = quant::ingestion::databento_detail::condition_kind(
        std::vector<ConditionDetail>{{"2024-01-02", ::databento::DatasetCondition::Available, std::nullopt}});
    require(available && *available == DataQualityKind::Complete,
            "available dataset conditions must produce complete evidence");

    const auto degraded = quant::ingestion::databento_detail::condition_kind(
        std::vector<ConditionDetail>{{"2024-01-02", ::databento::DatasetCondition::Degraded, std::nullopt}});
    require(degraded && *degraded == DataQualityKind::Degraded,
            "degraded dataset conditions must produce degraded evidence");

    const auto pending = quant::ingestion::databento_detail::condition_kind(
        std::vector<ConditionDetail>{{"2024-01-02", ::databento::DatasetCondition::Pending, std::nullopt}});
    require(pending && *pending == DataQualityKind::Pending,
            "pending dataset conditions must remain unknown evidence");

    const auto missing = quant::ingestion::databento_detail::condition_kind(
        std::vector<ConditionDetail>{{"2024-01-02", ::databento::DatasetCondition::Missing, std::nullopt}});
    require(missing && *missing == DataQualityKind::Missing,
            "missing dataset conditions must produce missing evidence");
}

void test_retained_normalized_bytes_and_dates() {
    auto record = message(::databento::Action::Add);
    integrity::RawRecord raw{0, std::vector<std::byte>(sizeof(record))};
    std::memcpy(raw.bytes.data(), &record, sizeof(record));
    const auto segment = mapping();
    const MarketDataQuery query{InstrumentId{42}, VenueId{7}, MarketDataLevel::L3, range(100, 200)};
    integrity::Acquisition acquisition{"operation-test", "acquisition-test", "attempt-test", time(300),
        query, segment, resolve_provider_fetch_range(query)};
    acquisition.native_bindings.push_back({9001, segment.range});
    const auto decoded = decode_xnas_record(raw, acquisition);
    const auto translated = databento_detail::translate_mbo(record, segment);
    require(decoded.event && decoded.error.empty() &&
                integrity::same_observation(*decoded.event, translated.mbo_events.front()),
            "retained-byte decoder must agree with official SDK interpretation");
    record.price = ::databento::kUndefPrice;
    std::memcpy(raw.bytes.data(), &record, sizeof(record));
    const auto invalid = decode_xnas_record(raw, acquisition);
    require(invalid.event && !is_valid(validate(std::get<MboEvent>(*invalid.event))),
            "invalid acquired record must stay representable as retained bytes/failure evidence");

    const auto midnight = std::chrono::duration_cast<std::chrono::nanoseconds>(
        date::sys_days{date::year{2024}/1/3}.time_since_epoch()).count();
    const auto intraday = databento_detail::date_range(range(midnight - 3600'000'000'000LL, midnight - 1800'000'000'000LL));
    require(intraday.start == "2024-01-02" && intraday.end == "2024-01-02", "intraday condition request includes an extra date");
    const auto exact_end = databento_detail::date_range(range(midnight - 60'000'000'000LL, midnight));
    require(exact_end.end == "2024-01-02", "midnight-exclusive end includes next day's condition");
    const auto spanning = databento_detail::date_range(range(midnight - 60'000'000'000LL, midnight + 60'000'000'000LL));
    require(spanning.end == "2024-01-03", "multi-day condition endpoint is wrong");
    auto day_mapping = segment;
    day_mapping.range = range(midnight - 60'000'000'000LL, midnight + 60'000'000'000LL);
    const auto scoped = databento_detail::day_conditions({
        {"2024-01-02", ::databento::DatasetCondition::Available, std::nullopt},
        {"2024-01-03", ::databento::DatasetCondition::Missing, std::nullopt},
        {"2024-01-04", ::databento::DatasetCondition::Degraded, std::nullopt}}, day_mapping);
    require(scoped.size() == 2 && scoped[0].range.end == time(midnight) && scoped[1].range.start == time(midnight) &&
                scoped[0].kind == DataQualityKind::Complete && scoped[1].kind == DataQualityKind::Missing,
            "daily conditions must affect only their intersecting UTC day");
    require_throws([&] {
        static_cast<void>(databento_detail::day_conditions({
            {"9999-12-31", ::databento::DatasetCondition::Available, std::nullopt}}, day_mapping));
    }, "unsupported day must be rejected without signed timestamp overflow");
}

} // namespace

int main() {
    try {
        test_mbo_action_interpretation();
        test_query_time_resolution();
        test_corrupt_provider_record();
        test_undefined_source_receive_time_is_optional();
        test_provider_instrument_id_mapping();
        test_dataset_condition_mapping();
        test_retained_normalized_bytes_and_dates();
        std::cout << "databento_translation_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "databento_translation_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
