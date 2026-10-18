#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quant::data {

class StreamId {
public:
    explicit StreamId(const std::uint64_t value) : value_(value) {
        if (value == 0) {
            throw std::invalid_argument("StreamId must be non-zero");
        }
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    friend constexpr bool operator==(const StreamId&, const StreamId&) = default;
    friend constexpr auto operator<=>(const StreamId&, const StreamId&) = default;
private:
    std::uint64_t value_;
};

class InstrumentId {
public:
    explicit InstrumentId(const std::uint64_t value) : value_(value) {
        if (value == 0) {
            throw std::invalid_argument("InstrumentId must be non-zero");
        }
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(const InstrumentId&, const InstrumentId&) = default;
    friend constexpr auto operator<=>(const InstrumentId&, const InstrumentId&) = default;

private:
    std::uint64_t value_;
};

class OrderId {
public:
    explicit OrderId(const std::uint64_t value) : value_(value) {
        if (value == 0) {
            throw std::invalid_argument("OrderId must be non-zero");
        }
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(const OrderId&, const OrderId&) = default;
    friend constexpr auto operator<=>(const OrderId&, const OrderId&) = default;

private:
    std::uint64_t value_;
};

class Timestamp {
public:
    explicit constexpr Timestamp(const std::int64_t unix_nanos) noexcept : unix_nanos_(unix_nanos) {}

    [[nodiscard]] static constexpr Timestamp from_unix_nanos(const std::int64_t unix_nanos) noexcept {
        return Timestamp(unix_nanos);
    }

    [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return unix_nanos_; }

    friend constexpr bool operator==(const Timestamp&, const Timestamp&) = default;
    friend constexpr auto operator<=>(const Timestamp&, const Timestamp&) = default;

private:
    std::int64_t unix_nanos_;
};

class VenueId {
public:
    // Zero is reserved so an uninitialized/reference-free venue cannot look valid.
    explicit VenueId(const std::uint32_t value) : value_(value) {
        if (value == 0) {
            throw std::invalid_argument("VenueId must be non-zero");
        }
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    friend constexpr bool operator==(const VenueId&, const VenueId&) = default;
    friend constexpr auto operator<=>(const VenueId&, const VenueId&) = default;

private:
    std::uint32_t value_;
};

class SourceId {
public:
    // Zero is reserved so an uninitialized/reference-free source cannot look valid.
    explicit SourceId(const std::uint32_t value) : value_(value) {
        if (value == 0) {
            throw std::invalid_argument("SourceId must be non-zero");
        }
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    friend constexpr bool operator==(const SourceId&, const SourceId&) = default;
    friend constexpr auto operator<=>(const SourceId&, const SourceId&) = default;

private:
    std::uint32_t value_;
};

class CurrencyCode {
public:
    explicit CurrencyCode(std::string value) : value_(std::move(value)) {
        if (value_.size() != 3 || !is_uppercase_ascii(value_[0]) || !is_uppercase_ascii(value_[1]) ||
            !is_uppercase_ascii(value_[2])) {
            throw std::invalid_argument("CurrencyCode must be three uppercase ASCII letters");
        }
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    friend bool operator==(const CurrencyCode&, const CurrencyCode&) = default;
    friend auto operator<=>(const CurrencyCode&, const CurrencyCode&) = default;

private:
    [[nodiscard]] static constexpr bool is_uppercase_ascii(const char value) noexcept {
        return value >= 'A' && value <= 'Z';
    }

    std::string value_;
};

struct DecimalInput {
    std::int64_t coefficient;
    std::uint8_t scale;
};

namespace detail {

inline constexpr std::int64_t decimal_powers_of_ten[] = {
    1LL,
    10LL,
    100LL,
    1'000LL,
    10'000LL,
    100'000LL,
    1'000'000LL,
    10'000'000LL,
    100'000'000LL,
    1'000'000'000LL,
    10'000'000'000LL,
    100'000'000'000LL,
    1'000'000'000'000LL,
    10'000'000'000'000LL,
    100'000'000'000'000LL,
    1'000'000'000'000'000LL,
    10'000'000'000'000'000LL,
    100'000'000'000'000'000LL,
    1'000'000'000'000'000'000LL,
};

[[nodiscard]] inline std::int64_t checked_add(const std::int64_t lhs, const std::int64_t rhs) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    if ((rhs > 0 && lhs > max - rhs) || (rhs < 0 && lhs < min - rhs)) {
        throw std::overflow_error("canonical decimal addition overflow");
    }
    return lhs + rhs;
}

[[nodiscard]] inline std::int64_t checked_subtract(const std::int64_t lhs, const std::int64_t rhs) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    if ((rhs > 0 && lhs < min + rhs) || (rhs < 0 && lhs > max + rhs)) {
        throw std::overflow_error("canonical decimal subtraction overflow");
    }
    return lhs - rhs;
}

} // namespace detail

// Provider decimal fields are normalized at the adapter/boundary, never in hot comparisons.
[[nodiscard]] inline std::int64_t normalize_decimal(const DecimalInput input, const std::uint8_t target_scale) {
    constexpr auto max_scale = std::uint8_t{18};
    if (input.scale > max_scale || target_scale > max_scale) {
        throw std::invalid_argument("decimal scale must be at most 18");
    }

    if (input.scale == target_scale) {
        return input.coefficient;
    }

    if (input.scale < target_scale) {
        const auto factor = detail::decimal_powers_of_ten[target_scale - input.scale];
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        if (input.coefficient > max / factor || input.coefficient < min / factor) {
            throw std::overflow_error("decimal normalization overflow");
        }
        return input.coefficient * factor;
    }

    const auto divisor = detail::decimal_powers_of_ten[input.scale - target_scale];
    if (input.coefficient % divisor != 0) {
        throw std::invalid_argument("decimal cannot be represented at the canonical scale without rounding");
    }
    return input.coefficient / divisor;
}

struct VenueReference {
    VenueId id;
    std::string code;
};

class VenueReferenceTable {
public:
    void add(VenueReference reference) {
        if (reference.code.empty()) {
            throw std::invalid_argument("venue reference code must not be empty");
        }
        if (find(reference.id) != nullptr) {
            throw std::invalid_argument("venue reference id must be unique");
        }
        references_.push_back(std::move(reference));
    }

    [[nodiscard]] const VenueReference* find(const VenueId id) const noexcept {
        for (const auto& reference : references_) {
            if (reference.id == id) {
                return &reference;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return references_.size(); }

private:
    std::vector<VenueReference> references_;
};

struct SourceMetadata {
    SourceId id;
    std::string provider;
    std::string dataset;
    std::string schema;
};

class SourceMetadataTable {
public:
    void add(SourceMetadata metadata) {
        if (metadata.provider.empty()) {
            throw std::invalid_argument("source metadata provider must not be empty");
        }
        if (find(metadata.id) != nullptr) {
            throw std::invalid_argument("source metadata id must be unique");
        }
        metadata_.push_back(std::move(metadata));
    }

    [[nodiscard]] const SourceMetadata* find(const SourceId id) const noexcept {
        for (const auto& metadata : metadata_) {
            if (metadata.id == id) {
                return &metadata;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return metadata_.size(); }

private:
    std::vector<SourceMetadata> metadata_;
};

class Price {
public:
    static constexpr std::uint8_t canonical_scale = 9;

    [[nodiscard]] static Price from_raw(const std::int64_t raw_value) noexcept { return Price(raw_value); }

    [[nodiscard]] static Price from_integer(const std::int64_t value) {
        return from_decimal(DecimalInput{value, 0});
    }

    [[nodiscard]] static Price from_decimal(const DecimalInput input) {
        return from_raw(normalize_decimal(input, canonical_scale));
    }

    [[nodiscard]] static Price from_decimal(const std::int64_t coefficient, const std::uint8_t scale) {
        return from_decimal(DecimalInput{coefficient, scale});
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_value_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_value_ == 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_value_ < 0; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_value_ > 0; }

    [[nodiscard]] Price operator+(const Price& other) const {
        return from_raw(detail::checked_add(raw_value_, other.raw_value_));
    }

    [[nodiscard]] Price operator-(const Price& other) const {
        return from_raw(detail::checked_subtract(raw_value_, other.raw_value_));
    }

    friend constexpr bool operator==(const Price&, const Price&) = default;
    friend constexpr auto operator<=>(const Price&, const Price&) = default;

private:
    explicit constexpr Price(const std::int64_t raw_value) noexcept : raw_value_(raw_value) {}

    std::int64_t raw_value_;
};

class Quantity {
public:
    static constexpr std::uint8_t canonical_scale = 6;

    [[nodiscard]] static Quantity from_raw(const std::int64_t raw_value) {
        if (raw_value < 0) {
            throw std::invalid_argument("Quantity must not be negative");
        }
        return Quantity(raw_value);
    }

    [[nodiscard]] static Quantity from_integer(const std::int64_t value) {
        return from_decimal(DecimalInput{value, 0});
    }

    [[nodiscard]] static Quantity from_decimal(const DecimalInput input) {
        return from_raw(normalize_decimal(input, canonical_scale));
    }

    [[nodiscard]] static Quantity from_decimal(const std::int64_t coefficient, const std::uint8_t scale) {
        return from_decimal(DecimalInput{coefficient, scale});
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_value_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_value_ == 0; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_value_ > 0; }

    [[nodiscard]] Quantity operator+(const Quantity& other) const {
        return from_raw(detail::checked_add(raw_value_, other.raw_value_));
    }

    [[nodiscard]] Quantity operator-(const Quantity& other) const {
        const auto result = detail::checked_subtract(raw_value_, other.raw_value_);
        if (result < 0) {
            throw std::overflow_error("quantity subtraction would become negative");
        }
        return from_raw(result);
    }

    friend constexpr bool operator==(const Quantity&, const Quantity&) = default;
    friend constexpr auto operator<=>(const Quantity&, const Quantity&) = default;

private:
    explicit constexpr Quantity(const std::int64_t raw_value) noexcept : raw_value_(raw_value) {}

    std::int64_t raw_value_;
};

class Money {
public:
    static constexpr std::uint8_t canonical_scale = 2;

    [[nodiscard]] static Money from_raw(CurrencyCode currency, const std::int64_t raw_amount) {
        return Money(std::move(currency), raw_amount);
    }

    [[nodiscard]] static Money from_decimal(CurrencyCode currency, const DecimalInput input) {
        return from_raw(std::move(currency), normalize_decimal(input, canonical_scale));
    }

    [[nodiscard]] const CurrencyCode& currency() const noexcept { return currency_; }
    [[nodiscard]] constexpr std::int64_t amount() const noexcept { return raw_amount_; }

    [[nodiscard]] Money operator+(const Money& other) const {
        require_same_currency(other);
        return from_raw(currency_, detail::checked_add(raw_amount_, other.raw_amount_));
    }

    [[nodiscard]] Money operator-(const Money& other) const {
        require_same_currency(other);
        return from_raw(currency_, detail::checked_subtract(raw_amount_, other.raw_amount_));
    }

    friend bool operator==(const Money&, const Money&) = default;

private:
    Money(CurrencyCode currency, const std::int64_t raw_amount)
        : currency_(std::move(currency)), raw_amount_(raw_amount) {}

    void require_same_currency(const Money& other) const {
        if (currency_ != other.currency_) {
            throw std::invalid_argument("money arithmetic requires matching currencies");
        }
    }

    CurrencyCode currency_;
    std::int64_t raw_amount_;
};

enum class InstrumentType {
    Equity,
    Option,
    Future,
    Other,
};

enum class Side {
    Buy,
    Sell,
};

enum class MboAction {
    Add,
    Modify,
    Cancel,
    Execute,
    Clear,
};

} // namespace quant::data
