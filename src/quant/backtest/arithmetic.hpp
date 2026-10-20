#pragma once
#include "quant/backtest/model.hpp"

namespace quant::backtest {
[[nodiscard]] BacktestResult<Account> fund_account(std::uint32_t trial, SourceIdentity,
                                                   data::Money initial_cash,
                                                   data::Quantity lot_step) noexcept;
[[nodiscard]] BacktestResult<BuyCashBudget> freeze_cash_budget(const Account &) noexcept;
[[nodiscard]] BacktestResult<data::Price> execution_price(data::Price, data::Side,
                                                          const ExecutionPolicy &) noexcept;
[[nodiscard]] BacktestResult<data::Money> settle_gross(data::Price, data::Quantity,
                                                       const data::CurrencyCode &) noexcept;
[[nodiscard]] BacktestResult<data::Money> settle_fee(data::Price, data::Quantity,
                                                     const data::CurrencyCode &,
                                                     const ExecutionPolicy &) noexcept;
[[nodiscard]] BacktestResult<data::Money> settle_dividend(const DividendPerUnit9 &,
                                                          data::Quantity) noexcept;
[[nodiscard]] BacktestResult<FillQuote> quote_fill(const Intent &,
                                                   const research::OpeningProjection &,
                                                   const Account &,
                                                   const ExecutionPolicy &) noexcept;
[[nodiscard]] BacktestResult<FillQuote>
quote_terminal_liquidation(const research::CloseProjection &, const Account &,
                           const ExecutionPolicy &, const TerminalPolicy &) noexcept;
// Posting boundary: reconstruct and compare the entire quoted candidate.
[[nodiscard]] BacktestResult<void> validate_fill(const Fill &, const Account &) noexcept;
[[nodiscard]] BacktestResult<bool> loss_at_least(data::Money baseline, data::Money current,
                                                 std::uint32_t bps) noexcept;
[[nodiscard]] BacktestResult<bool> ratio_greater(std::int64_t numerator, std::int64_t denominator,
                                                 std::int64_t other_numerator,
                                                 std::int64_t other_denominator) noexcept;
[[nodiscard]] BacktestResult<data::Price> weighted_price(data::Price a, std::uint64_t weight_a,
                                                         data::Price b, std::uint64_t weight_b,
                                                         std::uint64_t denominator) noexcept;
[[nodiscard]] BacktestResult<data::Quantity> keltner_quantity(const Account &,
                                                              data::Price signal_close,
                                                              data::Price atr,
                                                              const ExecutionPolicy &) noexcept;
} // namespace quant::backtest
