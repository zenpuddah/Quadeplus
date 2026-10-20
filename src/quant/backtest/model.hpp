#pragma once
#include "quant/backtest/result.hpp"
#include "quant/data/value_types.hpp"
#include <span>
#include <string>
#include <vector>

namespace quant::research {
class OpeningProjection;
class CloseProjection;
} // namespace quant::research
namespace quant::backtest {
inline constexpr std::string_view execution_profile = "price9_money2_exact_v1";
inline constexpr std::string_view dividend_profile = "ex_date_cash_compatibility_v1";
class DividendPerUnit9 {
  public:
    DividendPerUnit9(data::CurrencyCode currency, std::int64_t raw)
        : currency_(std::move(currency)), raw_(raw) {
        if (raw < 0) {
            throw std::invalid_argument("negative dividend per unit");
        }
    }
    [[nodiscard]] const data::CurrencyCode &currency() const noexcept {
        return currency_;
    }
    [[nodiscard]] std::int64_t raw() const noexcept {
        return raw_;
    }
    friend bool operator==(const DividendPerUnit9 &, const DividendPerUnit9 &) = default;

  private:
    data::CurrencyCode currency_;
    std::int64_t raw_;
};
struct SourceIdentity {
    std::string manifest_sha256;
    std::uint64_t instrument;
    std::uint32_t venue, source;
    data::CurrencyCode currency;
    friend bool operator==(const SourceIdentity &, const SourceIdentity &) = default;
};
enum class ValuationPhase { PriorClose, Open, Close };
class ValuationProjection {
  public:
    [[nodiscard]] const SourceIdentity &source() const noexcept {
        return source_;
    }
    [[nodiscard]] data::Price price() const noexcept {
        return price_;
    }
    [[nodiscard]] std::uint64_t key() const noexcept {
        return key_;
    }
    [[nodiscard]] std::uint64_t price_key() const noexcept {
        return price_key_;
    }
    [[nodiscard]] std::int64_t available_at_ns() const noexcept {
        return available_;
    }
    [[nodiscard]] ValuationPhase phase() const noexcept {
        return phase_;
    }
    friend bool operator==(const ValuationProjection &, const ValuationProjection &) = default;

  private:
    SourceIdentity source_;
    data::Price price_;
    std::uint64_t key_, price_key_;
    std::int64_t available_;
    ValuationPhase phase_;
    ValuationProjection(SourceIdentity source, data::Price price, std::uint64_t key,
                        std::uint64_t price_key, std::int64_t available, ValuationPhase phase)
        : source_(std::move(source)), price_(price), key_(key), price_key_(price_key),
          available_(available), phase_(phase) {}
    friend BacktestResult<ValuationProjection>
    opening_valuation(const research::OpeningProjection &) noexcept;
    friend BacktestResult<ValuationProjection>
    closing_valuation(const research::CloseProjection &) noexcept;
    friend BacktestResult<ValuationProjection>
    prior_close_valuation(const research::CloseProjection &,
                          const research::OpeningProjection &) noexcept;
};
[[nodiscard]] BacktestResult<ValuationProjection>
opening_valuation(const research::OpeningProjection &) noexcept;
[[nodiscard]] BacktestResult<ValuationProjection>
closing_valuation(const research::CloseProjection &) noexcept;
[[nodiscard]] BacktestResult<ValuationProjection>
prior_close_valuation(const research::CloseProjection &,
                      const research::OpeningProjection &) noexcept;

struct Account {
    std::uint32_t trial;
    SourceIdentity source;
    data::Money initial_cash, cash, basis, realized, dividends, fees;
    data::Quantity units, lot_step;
    std::uint64_t revision = 0, cash_revision = 0;
    struct EventLocation {
        std::uint64_t key;
        ValuationPhase phase;
        friend bool operator==(const EventLocation &, const EventLocation &) = default;
    };
    std::optional<EventLocation> last_event{};
    friend bool operator==(const Account &, const Account &) = default;
};
struct ExecutionPolicy {
    std::int32_t commission_bps, slippage_bps;
    friend bool operator==(const ExecutionPolicy &, const ExecutionPolicy &) = default;
};
struct BuyCashBudget {
    data::Money budget, decision_cash;
    std::uint64_t cash_revision;
    friend bool operator==(const BuyCashBudget &, const BuyCashBudget &) = default;
};
struct BuyFixedQuantity {
    data::Quantity quantity;
    std::optional<data::Price> initial_stop_distance{};
    friend bool operator==(const BuyFixedQuantity &, const BuyFixedQuantity &) = default;
};
struct SellAll {
    friend bool operator==(const SellAll &, const SellAll &) = default;
};
struct Intent {
    std::uint32_t trial;
    std::uint64_t id;
    SourceIdentity source;
    std::uint64_t decision_key, earliest_open_key;
    std::string reason;
    std::variant<BuyCashBudget, BuyFixedQuantity, SellAll> order;
    friend bool operator==(const Intent &, const Intent &) = default;
};
enum class RejectionReason {
    BelowMinimumLot,
    InsufficientCash,
    AllocationCap,
    PositionAlreadyOpen,
    NoPosition,
    CashChanged,
    NotEligible,
    NonpositiveProtectiveThreshold
};
struct OrderRejection {
    std::uint32_t trial;
    std::uint64_t id;
    RejectionReason reason;
};
enum class TerminalMode { Mark, SyntheticClose };
struct TerminalPolicy {
    TerminalMode mode;
    std::uint64_t fill_id;
};
struct Fill {
    std::uint32_t trial;
    std::uint64_t id, expected_revision;
    std::optional<Intent> intent;
    ValuationProjection mark;
    data::Side side;
    data::Quantity quantity;
    data::Price executed_price;
    data::Money gross, fee;
    ExecutionPolicy policy;
    bool synthetic_terminal;
    std::string reason;
    friend bool operator==(const Fill &, const Fill &) = default;
};
using FillQuote = std::variant<Fill, OrderRejection>;
struct DividendEvent {
    std::uint32_t trial;
    ValuationProjection prior_close;
    DividendPerUnit9 per_unit;
    data::Quantity entitled_units;
    friend bool operator==(const DividendEvent &, const DividendEvent &) = default;
};
struct EquityPoint {
    std::uint32_t trial;
    std::uint64_t account_revision;
    ValuationProjection mark;
    data::Money position_value, equity, unrealized;
    friend bool operator==(const EquityPoint &, const EquityPoint &) = default;
};
struct LedgerRow {
    std::uint64_t sequence;
    std::variant<Fill, DividendEvent> event;
    Account after;
};
class Ledger {
  public:
    [[nodiscard]] static BacktestResult<Ledger> create(const Account &,
                                                       std::size_t max_events) noexcept;
    [[nodiscard]] const Account &initial() const noexcept {
        return initial_;
    }
    [[nodiscard]] std::span<const LedgerRow> rows() const noexcept {
        return rows_;
    }
    [[nodiscard]] std::size_t capacity_limit() const noexcept {
        return max_events_;
    }

  private:
    Ledger(Account initial, std::size_t maximum)
        : initial_(std::move(initial)), max_events_(maximum) {}
    Account initial_;
    std::size_t max_events_;
    std::vector<LedgerRow> rows_;
    friend struct LedgerAccess;
};
} // namespace quant::backtest
