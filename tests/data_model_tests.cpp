#include "quant/data/reference.hpp"
#include "quant/data/validation.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <stdexcept>
#include <string_view>

namespace {

using namespace quant::data;

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
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, message);
}

template <typename Function>
void require_throws_exception(Function&& function, const std::string_view message) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}

bool contains_code(const ValidationIssues& issues, const std::string_view code) {
    for (const auto& issue : issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

Timestamp time(const std::int64_t nanos) {
    return Timestamp::from_unix_nanos(nanos);
}

EventHeader header() {
    return EventHeader{
        InstrumentId{1},
        VenueId{1},
        time(100),
        time(110),
        std::uint64_t{7},
        std::uint32_t{2},
        std::uint64_t{0},
        SourceId{1},
    };
}

ReferenceVersion reference(
    const InstrumentId id,
    const Timestamp from,
    const std::optional<Timestamp> until,
    const std::string_view symbol) {
    return ReferenceVersion{
        id,
        from,
        until,
        InstrumentType::Equity,
        std::string(symbol),
        CurrencyCode{"USD"},
        Price::from_decimal(1, 2),
        Quantity::from_integer(1),
    };
}

void test_value_types() {
    require(Price::canonical_scale == 9, "price canonical scale must be nine decimal places");
    require(Quantity::canonical_scale == 6, "quantity canonical scale must be six decimal places");
    require(Money::canonical_scale == 2, "money canonical scale must be two decimal places");

    require(Price::from_decimal(10, 2) == Price::from_decimal(1, 1),
            "price normalization must be scale-independent");
    require(Price::from_decimal(120, 2) > Price::from_decimal(119, 2),
            "price ordering must be exact");
    require(Price::from_decimal(-1, 0) < Price::from_raw(0), "negative price must compare below zero");
    require(Price::from_decimal(1, 3) < Price::from_decimal(1, 2),
            "decimal scale must affect value ordering");
    require(Price::from_decimal(0, 18) == Price::from_raw(0),
            "all zero price representations must compare equal");
    require(Price::from_decimal(12345, 2).raw() == 123'450'000'000LL,
            "price input must normalize to canonical integer units");
    require(Quantity::from_decimal(10, 0).raw() == 10'000'000,
            "quantity input must normalize to canonical integer units");
    require(Price::from_integer(1'000'000'000).raw() == 1'000'000'000'000'000'000LL,
            "price scale must retain billion-dollar Phase 1 values");
    require(Quantity::from_integer(1'000'000'000).raw() == 1'000'000'000'000'000LL,
            "quantity scale must retain billion-unit Phase 1 values");
    require(sizeof(VenueId) == sizeof(std::uint32_t), "venue ids must use compact 32-bit storage");
    require(sizeof(SourceId) == sizeof(std::uint32_t), "source ids must use compact 32-bit storage");

    require((Price::from_decimal(1, 2) + Price::from_decimal(2, 2)) == Price::from_decimal(3, 2),
            "price addition must use checked canonical units");
    require((Quantity::from_integer(3) - Quantity::from_integer(1)) == Quantity::from_integer(2),
            "quantity subtraction must use checked canonical units");
    require_throws_exception(
        [] { (void)(Price::from_raw(std::numeric_limits<std::int64_t>::max()) + Price::from_raw(1)); },
        "price arithmetic overflow must be rejected");
    require_throws_exception(
        [] { (void)(Quantity::from_raw(0) - Quantity::from_raw(1)); },
        "quantity arithmetic underflow must be rejected");

    const auto usd = Money::from_decimal(CurrencyCode{"USD"}, DecimalInput{12345, 2});
    const auto usd_change = Money::from_decimal(CurrencyCode{"USD"}, DecimalInput{55, 2});
    require(usd.amount() == 12345, "money must retain canonical minor units");
    require((usd + usd_change).amount() == 12400, "money addition must use canonical minor units");
    require_throws(
        [&] { (void)(usd + Money::from_decimal(CurrencyCode{"EUR"}, DecimalInput{1, 2})); },
        "money arithmetic must reject mixed currencies");

    require_throws([] { InstrumentId{0}; }, "zero instrument id must be rejected");
    require_throws([] { OrderId{0}; }, "zero order id must be rejected");
    require_throws([] { VenueId{0}; }, "zero venue id must be rejected");
    require_throws([] { SourceId{0}; }, "zero source id must be rejected");
    require_throws([] { CurrencyCode{"usd"}; }, "lowercase currency code must be rejected");
    require_throws(
        [] { (void)Price::from_decimal(1, 10); },
        "non-exact decimal downscaling must be rejected");
    require_throws([] { (void)Quantity::from_decimal(-1, 0); }, "negative quantity must be rejected");
    require_throws_exception(
        [] { (void)Price::from_decimal(std::numeric_limits<std::int64_t>::max(), 0); },
        "canonical decimal overflow must be rejected");

    VenueReferenceTable venues;
    venues.add(VenueReference{VenueId{1}, "XNAS"});
    require(venues.find(VenueId{1}) != nullptr, "venue reference must be keyed by compact id");
    require(venues.find(VenueId{1})->code == "XNAS", "venue code must live in the reference table");
    require_throws(
        [&] { venues.add(VenueReference{VenueId{1}, "OTHER"}); },
        "venue reference ids must be unique");
    require_throws(
        [&] { venues.add(VenueReference{VenueId{2}, ""}); },
        "venue reference codes must not be empty");

    SourceMetadataTable sources;
    sources.add(SourceMetadata{SourceId{1}, "databento", "test", "mbo"});
    require(sources.find(SourceId{1}) != nullptr, "source metadata must be keyed by compact id");
    require(sources.find(SourceId{1})->provider == "databento", "source provider must live in the metadata table");
    require_throws(
        [&] { sources.add(SourceMetadata{SourceId{1}, "other", "test", "mbo"}); },
        "source metadata ids must be unique");
    require_throws(
        [&] { sources.add(SourceMetadata{SourceId{2}, "", "test", "mbo"}); },
        "source metadata providers must not be empty");
}

void test_reference_history() {
    const InstrumentId id{1};
    ReferenceHistory history{id};
    history.append(reference(id, time(0), time(10), "ABC"));
    history.append(reference(id, time(10), std::nullopt, "XYZ"));

    require(history.size() == 2, "reference history must retain both versions");
    require(history.at(time(9)) != nullptr, "reference history must resolve an active version");
    require(history.at(time(9))->symbol == "ABC", "reference lookup must be point-in-time");
    require(history.at(time(10))->symbol == "XYZ", "validity end must be exclusive");
    require(history.at(time(-1)) == nullptr, "reference lookup before history must be empty");

    ReferenceHistory with_gaps{id};
    for (std::int64_t index = 0; index < 64; ++index) {
        const auto start = index * 10;
        with_gaps.append(reference(id, time(start), time(start + 5), "SYM"));
    }
    require(with_gaps.at(time(0)) != nullptr, "binary lookup must include the first interval");
    require(with_gaps.at(time(634)) != nullptr, "binary lookup must include the last interval");
    require(with_gaps.at(time(5)) == nullptr, "binary lookup must preserve gap semantics");
    require(with_gaps.at(time(640)) == nullptr, "binary lookup must reject time after history");

    require_throws(
        [&] { history.append(reference(InstrumentId{2}, time(20), std::nullopt, "OTHER")); },
        "reference history must not mix instruments");
    require_throws(
        [&] { history.append(reference(id, time(20), std::nullopt, "LATE")); },
        "reference history must not append after an open interval");

    ReferenceHistory overlapping{id};
    overlapping.append(reference(id, time(0), time(10), "ABC"));
    require_throws(
        [&] { overlapping.append(reference(id, time(9), std::nullopt, "OVERLAP")); },
        "reference validity intervals must not overlap");
}

void test_mbo_events() {
    const auto event_header = header();
    const auto price = Price::from_decimal(12345, 2);
    const auto quantity = Quantity::from_integer(10);

    const MboEvent add{
        event_header,
        MboAction::Add,
        OrderId{100},
        Side::Buy,
        price,
        quantity,
    };
    require(add.has_complete_order_state(), "complete add must expose complete order state");
    require(is_valid(validate(add)), "complete add must validate");

    const MboEvent clear{
        event_header,
        MboAction::Clear,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    require(is_valid(validate(clear)), "empty clear must validate");

    const MboEvent incomplete_add{
        event_header,
        MboAction::Add,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    const auto add_issues = validate(incomplete_add);
    require(contains_code(add_issues, "missing_order_id"), "incomplete add must report missing order id");
    require(contains_code(add_issues, "incomplete_add"), "incomplete add must report missing order fields");

    const MboEvent invalid_clear{
        event_header,
        MboAction::Clear,
        OrderId{101},
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    require(contains_code(validate(invalid_clear), "clear_has_order_fields"),
            "clear must not carry order fields");

    MboBuffer buffer{MboStreamContext{InstrumentId{1}, VenueId{1}, SourceId{1}}};
    buffer.append(MboAdd{event_header, OrderId{100}, Side::Buy, price, quantity});
    buffer.append(MboModify{event_header, OrderId{100}, Side::Sell, std::nullopt, std::nullopt});
    buffer.append(MboCancel{event_header, OrderId{100}, Quantity::from_integer(2)});
    buffer.append(MboExecute{event_header, OrderId{100}, Quantity::from_integer(3)});
    buffer.append(MboClear{event_header});

    require(sizeof(MboRecord) == 64, "MBO records must use the measured fixed stride");
    require(buffer.size() == 5, "MBO buffer must retain each typed writer event");
    require(buffer.data() != nullptr, "non-empty MBO buffer must expose contiguous records");
    require(reinterpret_cast<std::uintptr_t>(buffer.data()) % alignof(MboRecord) == 0,
            "MBO buffer data must honor the record alignment");
    require(
        reinterpret_cast<std::uintptr_t>(buffer.data() + 1) - reinterpret_cast<std::uintptr_t>(buffer.data()) ==
            sizeof(MboRecord),
        "MBO buffer records must have a constant physical stride");
    require(buffer.at(0).action() == MboAction::Add, "MBO view must decode the action tag");
    require(buffer.at(0).header().venue_id == VenueId{1}, "MBO view must restore compact venue context");
    require(buffer.at(0).header().source_id == SourceId{1}, "MBO view must restore compact source context");
    require(buffer.at(0).order_id()->value() == 100, "MBO view must decode the order id");
    require(buffer.at(0).price()->raw() == price.raw(), "MBO view must decode canonical price units");
    require(buffer.at(0).as_add().has_value(), "MBO view must expose a typed add view");
    require(buffer.at(1).as_modify()->side == Side::Sell, "MBO view must expose a typed modify view");
    require(buffer.at(2).as_cancel()->quantity.raw() == Quantity::from_integer(2).raw(),
            "MBO view must expose a typed cancel view");
    require(buffer.at(3).as_execute()->quantity.raw() == Quantity::from_integer(3).raw(),
            "MBO view must expose a typed execute view");
    require(buffer.at(4).as_clear().has_value(), "MBO view must expose a typed clear view");
    require(!buffer.at(4).order_id() && !buffer.at(4).price() && !buffer.at(4).quantity(),
            "clear records must leave order payload slots unused");

    std::size_t iterated = 0;
    for (const auto view : buffer) {
        require(view.event().header.source_id == SourceId{1}, "buffer iteration must preserve source identity");
        ++iterated;
    }
    require(iterated == buffer.size(), "MBO buffer iteration must use the fixed contiguous sequence");

    EventHeader edge_header{
        InstrumentId{1},
        VenueId{1},
        time(-7),
        time(std::numeric_limits<std::int64_t>::min()),
        std::uint64_t{0},
        std::uint32_t{0},
        std::uint64_t{9},
        SourceId{1},
    };
    MboBuffer edge_buffer{MboStreamContext{InstrumentId{1}, VenueId{1}, SourceId{1}}};
    edge_buffer.append(MboClear{edge_header});
    require(edge_buffer.at(0).source_receive_time()->unix_nanos() == std::numeric_limits<std::int64_t>::min(),
            "MBO presence bits must preserve the full receive timestamp range");
    require(edge_buffer.at(0).sequence() == std::optional<std::uint64_t>{0},
            "MBO presence bits must preserve zero sequence values");
    require(edge_buffer.at(0).channel_id() == std::optional<std::uint32_t>{0},
            "MBO presence bits must preserve zero channel values");

    require_throws(
        [&] {
            buffer.append(MboModify{event_header, OrderId{100}, std::nullopt, std::nullopt, std::nullopt});
        },
        "empty typed modify must be rejected");
    require_throws(
        [&] {
            buffer.append(MboAdd{
                EventHeader{
                    InstrumentId{1},
                    VenueId{2},
                    time(100),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    0,
                    SourceId{1},
                },
                OrderId{102},
                Side::Buy,
                price,
                quantity,
            });
        },
        "MBO buffer must reject events from a different stream scope");

    MboBuffer logical_buffer{MboStreamContext{InstrumentId{1}, VenueId{1}, SourceId{1}}};
    logical_buffer.append(add);
    require(logical_buffer.at(0).as_add().has_value(), "logical MBO events must normalize into the fixed buffer");
}

void test_trade_quote_and_bar() {
    const auto event_header = header();
    const auto bid = Price::from_integer(100);
    const auto ask = Price::from_integer(101);
    const auto quantity = Quantity::from_integer(5);

    const Trade trade{event_header, bid, quantity, Side::Buy};
    require(is_valid(validate(trade)), "positive trade must validate");

    const Trade zero_trade{event_header, bid, Quantity::from_raw(0), std::nullopt};
    require(contains_code(validate(zero_trade), "zero_trade_quantity"),
            "zero trade quantity must be rejected");

    const OrderExecution execution{event_header, OrderId{100}, quantity, bid};
    require(is_valid(validate(execution)), "positive order execution must validate");
    const OrderExecution zero_execution{event_header, OrderId{100}, Quantity::from_raw(0), bid};
    require(contains_code(validate(zero_execution), "zero_execution_quantity"),
            "zero order execution quantity must be rejected");

    const Quote quote{event_header, bid, quantity, ask, quantity};
    require(is_valid(validate(quote)), "two-sided quote must validate");

    const Quote crossed{event_header, ask, quantity, bid, quantity};
    require(contains_code(validate(crossed), "crossed_quote"), "crossed quote must be rejected");

    const Quote incomplete_bid{event_header, bid, std::nullopt, ask, quantity};
    require(contains_code(validate(incomplete_bid), "incomplete_bid"),
            "quote sides must contain price and quantity together");

    const Bar bar{
        event_header,
        time(0),
        time(60),
        Price::from_integer(100),
        Price::from_integer(105),
        Price::from_integer(95),
        Price::from_integer(101),
        Quantity::from_integer(1000),
    };
    require(is_valid(validate(bar)), "well-formed bar must validate");

    const Bar invalid_bar{
        event_header,
        time(60),
        time(0),
        Price::from_integer(110),
        Price::from_integer(105),
        Price::from_integer(95),
        Price::from_integer(101),
        Quantity::from_integer(1000),
    };
    const auto bar_issues = validate(invalid_bar);
    require(contains_code(bar_issues, "invalid_bar_interval"), "bar interval must be ordered");
    require(contains_code(bar_issues, "open_outside_range"), "bar open must stay within its range");
}

} // namespace

int main() {
    try {
        test_value_types();
        test_reference_history();
        test_mbo_events();
        test_trade_quote_and_bar();
        std::cout << "data_model_tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "data_model_tests: failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
