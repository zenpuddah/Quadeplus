#pragma once
#include "quant/backtest/result.hpp"
#include "quant/backtest/model.hpp"
#include "quant/data/observations.hpp"
#include "lexongine/results.hpp"
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace quant::research {
using backtest::BacktestResult;
inline constexpr std::string_view daily_format = "quadeplus-daily-research-v1";
inline constexpr std::string_view decimal_profile = "research_decimal_to_price9_even_v1";
inline constexpr std::string_view clock_profile = "date_utc_midnight_close_plus_second_v1";
inline constexpr std::string_view adjustment_profile =
    "split_adjusted_ohlc_dividends_same_share_basis_v1";
inline constexpr std::string_view volume_profile = "units_exact_quantity6_v1";

using backtest::DividendPerUnit9;
struct DatasetLimits {
    std::uint64_t max_file_bytes, max_rows, max_decimal_bytes;
};
struct DatasetSpec {
    // version is the bundle's explicit artifact label; daily_format is its schema.
    std::string version, symbol, currency, provenance;
    std::uint64_t instrument;
    std::uint32_t venue, source;
    std::string schedule, adjustment, normalization, volume, clock;
    std::string numerical_origin, evaluation_begin, evaluation_end;
};
struct DatasetRequest {
    std::filesystem::path directory;
    std::optional<std::string> expected_manifest_sha256;
    DatasetLimits limits;
    // Host/source admission capability, not a financial value or numerical input.
    std::shared_ptr<::lexongine::ResourceAccount> account;
};
struct DatasetPreparation {
    std::filesystem::path prices_csv, sessions_csv, output_directory;
    std::string expected_prices_sha256, expected_sessions_sha256;
    DatasetSpec specification;
    DatasetLimits limits;
    std::shared_ptr<::lexongine::ResourceAccount> account;
};
struct DailyRow {
    std::string session;
    data::Bar bar;
    DividendPerUnit9 dividend;
    std::int64_t close_available_ns;
};
struct NormalizationChange {
    std::string session, field, original, normalized, absolute_error;
};
class OpeningProjection {
  public:
    [[nodiscard]] const backtest::SourceIdentity &source() const noexcept {
        return source_;
    }
    [[nodiscard]] std::uint64_t key() const noexcept {
        return key_;
    }
    [[nodiscard]] data::Price price() const noexcept {
        return price_;
    }
    [[nodiscard]] std::int64_t available_at_ns() const noexcept {
        return available_;
    }

  private:
    friend class DailyDataset;
    OpeningProjection(backtest::SourceIdentity source, std::uint64_t key, data::Price price,
                      std::int64_t available)
        : source_(std::move(source)), key_(key), price_(price), available_(available) {}
    backtest::SourceIdentity source_;
    std::uint64_t key_;
    data::Price price_;
    std::int64_t available_;
};
class CloseProjection {
  public:
    [[nodiscard]] const backtest::SourceIdentity &source() const noexcept {
        return source_;
    }
    [[nodiscard]] std::uint64_t key() const noexcept {
        return key_;
    }
    [[nodiscard]] const data::Bar &bar() const noexcept {
        return bar_;
    }
    [[nodiscard]] std::int64_t available_at_ns() const noexcept {
        return available_;
    }

  private:
    friend class DailyDataset;
    CloseProjection(backtest::SourceIdentity source, std::uint64_t key, data::Bar bar,
                    std::int64_t available)
        : source_(std::move(source)), key_(key), bar_(std::move(bar)), available_(available) {}
    backtest::SourceIdentity source_;
    std::uint64_t key_;
    data::Bar bar_;
    std::int64_t available_;
};
class DailyDataset {
  public:
    [[nodiscard]] const DatasetSpec &specification() const noexcept;
    [[nodiscard]] std::span<const DailyRow> rows() const noexcept;
    [[nodiscard]] std::span<const NormalizationChange> normalizations() const noexcept;
    [[nodiscard]] std::string_view maximum_normalization_error() const noexcept;
    [[nodiscard]] std::string_view manifest_sha256() const noexcept;
    [[nodiscard]] std::string_view prices_sha256() const noexcept;
    [[nodiscard]] std::string_view sessions_sha256() const noexcept;
    [[nodiscard]] std::uint64_t owned_bytes() const noexcept;
    [[nodiscard]] std::shared_ptr<::lexongine::ResourceAccount> account() const noexcept;
    [[nodiscard]] BacktestResult<OpeningProjection>
    opening(std::uint64_t key, std::int64_t decision_time_ns) const noexcept;
    [[nodiscard]] BacktestResult<CloseProjection>
    closing(std::uint64_t key, std::int64_t decision_time_ns) const noexcept;

  private:
    struct Data;
    explicit DailyDataset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
    friend struct DatasetAccess;
};
[[nodiscard]] BacktestResult<DailyDataset> load_daily_dataset(const DatasetRequest &) noexcept;
[[nodiscard]] BacktestResult<DailyDataset>
prepare_daily_dataset(const DatasetPreparation &) noexcept;
// Seals dates from a separately hash-pinned original before a candidate is admitted.
[[nodiscard]] BacktestResult<std::string>
seal_reference_grid(const std::filesystem::path &original_csv, std::string_view expected_sha256,
                    const std::filesystem::path &new_grid, DatasetLimits,
                    std::shared_ptr<::lexongine::ResourceAccount>) noexcept;
} // namespace quant::research
