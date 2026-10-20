#pragma once

#include <cstdint>
#include <type_traits>

namespace quant::strategies {
// Dimensionless exact K/100, not a Price. Reduction to lowest terms is optional.
// Valid values have 0 <= numerator <= denominator and denominator > 0.
struct StochasticValue {
    std::int64_t numerator, denominator;
    friend bool operator==(const StochasticValue &, const StochasticValue &) = default;
};
static_assert(std::is_trivially_copyable_v<StochasticValue>);
static_assert(sizeof(StochasticValue) == 2 * sizeof(std::int64_t));
[[nodiscard]] constexpr bool valid_stochastic(StochasticValue value) noexcept {
    return value.denominator > 0 && value.numerator >= 0 && value.numerator <= value.denominator;
}
} // namespace quant::strategies
