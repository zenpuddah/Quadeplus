#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace quant::backtest {
enum class ErrorCode {
    InvalidConfig,
    Unsupported,
    Integrity,
    InvalidData,
    Coverage,
    NumericRange,
    ResourceExceeded,
    IO,
    FutureRead,
    SourceBinding
};
enum class Phase { Preparation, Source, Opening, Closing, Settlement, Finalization };
struct BacktestError {
    ErrorCode code;
    Phase phase;
    std::optional<std::uint32_t> trial;
    std::array<char, 256> message{};
    std::uint16_t length = 0;
    bool truncated = false;
    [[nodiscard]] std::string_view text() const noexcept {
        return {message.data(), length};
    }
};
inline BacktestError error(ErrorCode code, std::string_view text,
                           Phase phase = Phase::Preparation) noexcept {
    BacktestError result{code, phase, {}};
    const auto n = std::min(text.size(), result.message.size());
    std::copy_n(text.begin(), n, result.message.begin());
    result.length = static_cast<std::uint16_t>(n);
    result.truncated = n != text.size();
    return result;
}
template <class T> struct ResultType {
    using type = std::variant<T, BacktestError>;
};
template <> struct ResultType<void> {
    using type = std::variant<std::monostate, BacktestError>;
};
template <class T> using BacktestResult = typename ResultType<T>::type;
inline BacktestResult<void> ok() noexcept {
    return std::monostate{};
}
} // namespace quant::backtest
