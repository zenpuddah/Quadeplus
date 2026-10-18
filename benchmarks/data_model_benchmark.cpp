#include "quant/data/reference.hpp"
#include "quant/data/observations.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace quant::data;

struct Candidate32 {
    std::int64_t event_time;
    std::uint64_t order_id;
    std::int64_t price;
    std::int64_t quantity;
};

struct Candidate40 {
    std::int64_t event_time;
    std::int64_t source_receive_time;
    std::uint64_t order_id;
    std::int64_t price;
    std::int64_t quantity;
};

struct Candidate48 {
    std::int64_t event_time;
    std::int64_t source_receive_time;
    std::uint64_t sequence;
    std::uint64_t order_id;
    std::int64_t price;
    std::int64_t quantity;
};

static_assert(sizeof(Candidate32) == 32);
static_assert(sizeof(Candidate40) == 40);
static_assert(sizeof(Candidate48) == 48);

template <typename Record>
struct LayoutTraits;

template <>
struct LayoutTraits<Candidate32> {
    static constexpr std::size_t field_bytes = 32;
};

template <>
struct LayoutTraits<Candidate40> {
    static constexpr std::size_t field_bytes = 40;
};

template <>
struct LayoutTraits<Candidate48> {
    static constexpr std::size_t field_bytes = 48;
};

template <>
struct LayoutTraits<MboRecord> {
    static constexpr std::size_t field_bytes = 64;
};

constexpr std::size_t cache_line_size = 64;
constexpr std::size_t layout_record_count = 1'000'000;
constexpr std::size_t traversal_passes = 3;

template <typename Record>
void benchmark_layout(const std::string_view name) {
    std::vector<Record> records(layout_record_count);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto value = static_cast<std::uint64_t>(index);
        std::memcpy(&records[index], &value, sizeof(value));
    }

    const auto base = reinterpret_cast<std::uintptr_t>(records.data());
    const auto bytes = records.size() * sizeof(Record);
    const auto cache_lines = (base % cache_line_size + bytes + cache_line_size - 1) / cache_line_size;
    std::size_t records_crossing_cache_lines = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto record_begin = base + index * sizeof(Record);
        const auto record_end = record_begin + sizeof(Record) - 1;
        if (record_begin / cache_line_size != record_end / cache_line_size) {
            ++records_crossing_cache_lines;
        }
    }

    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t pass = 0; pass < traversal_passes; ++pass) {
        for (const auto& record : records) {
            std::uint64_t value = 0;
            std::memcpy(&value, &record, sizeof(value));
            checksum += value;
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::cout << "layout=" << name << " sizeof=" << sizeof(Record) << " alignof=" << alignof(Record)
              << " field_bytes=" << LayoutTraits<Record>::field_bytes
              << " padding_bytes=" << sizeof(Record) - LayoutTraits<Record>::field_bytes << " bytes=" << bytes
              << " cache_lines=" << cache_lines
              << " records_crossing_cache_lines=" << records_crossing_cache_lines
              << " crossing_percent="
              << (100.0 * static_cast<double>(records_crossing_cache_lines) /
                  static_cast<double>(records.size()))
              << " traversal_records_per_second="
              << static_cast<double>(records.size() * traversal_passes) / elapsed
              << " checksum=" << checksum << '\n';
}

void benchmark_reference_lookup() {
    constexpr std::int64_t version_count = 100'000;
    constexpr std::size_t lookup_count = 1'000'000;
    const InstrumentId instrument{1};
    ReferenceHistory history{instrument};
    for (std::int64_t index = 0; index < version_count; ++index) {
        const auto start = index * 3;
        history.append(ReferenceVersion{
            instrument,
            Timestamp::from_unix_nanos(start),
            Timestamp::from_unix_nanos(start + 2),
            InstrumentType::Equity,
            "SYM",
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    }

    std::size_t hits = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < lookup_count; ++index) {
        const auto offset = static_cast<std::int64_t>(index % static_cast<std::size_t>(version_count));
        const auto query = Timestamp::from_unix_nanos(offset * 3 + static_cast<std::int64_t>(index % 3));
        if (history.at(query) != nullptr) {
            ++hits;
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::cout << "reference_lookup versions=" << version_count << " lookups=" << lookup_count
              << " hits=" << hits << " lookups_per_second=" << static_cast<double>(lookup_count) / elapsed
              << '\n';
}

} // namespace

int main() {
    std::cout << "cache_line_size=" << cache_line_size << " layout_records=" << layout_record_count
              << " traversal_passes=" << traversal_passes << '\n';
    benchmark_layout<Candidate32>("32");
    benchmark_layout<Candidate40>("40");
    benchmark_layout<Candidate48>("48");
    benchmark_layout<MboRecord>("64");

    constexpr auto fixed_bytes = layout_record_count * sizeof(MboRecord);
    constexpr auto hypothetical_variable_bytes = layout_record_count * 70 / 100 * 64 +
        layout_record_count * 30 / 100 * 32;
    std::cout << "fixed_64_bytes=" << fixed_bytes
              << " hypothetical_70_percent_64_30_percent_32_bytes=" << hypothetical_variable_bytes << '\n';

    benchmark_reference_lookup();
}
