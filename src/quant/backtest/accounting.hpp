#pragma once
#include "quant/backtest/model.hpp"

namespace quant::backtest {
[[nodiscard]] BacktestResult<void> apply_fill(Account &, Ledger &, const Fill &) noexcept;
[[nodiscard]] BacktestResult<void> apply_dividend(Account &, Ledger &,
                                                  const DividendEvent &) noexcept;
[[nodiscard]] BacktestResult<EquityPoint> mark_account(const Account &, const ValuationProjection &,
                                                       ValuationPhase expected_phase) noexcept;
[[nodiscard]] BacktestResult<void> verify_financial_invariants(const Account &, const Ledger &,
                                                               const EquityPoint &) noexcept;
} // namespace quant::backtest
