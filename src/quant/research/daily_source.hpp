#pragma once

#include "quant/research/daily_dataset.hpp"
#include "quant/lexongine/components.hpp"
#include "lexongine/source.hpp"

namespace quant::research {

struct DailySourceLimits {
    std::uint64_t max_records, dataset_bytes, metadata_bytes, control_bytes, string_bytes;
};
// All choices are explicit. The first source accepts only the sealed synthetic
// clock, USD, complete-only/reject/fail and Strict coverage contracts.
struct DailySourceConfig {
    DailySourceLimits limits;
    std::string clock, provider_name, snapshot_name;
    ::lexongine::OperationKey operation;
    std::string currency, completeness, empty, missing, availability;
    ::lexongine::MissingIntervalPolicy missing_intervals;
};

namespace detail {
struct DailySourceBinding;
}
class RegisteredDailySource {
  public:
    [[nodiscard]] const DailyDataset &dataset() const noexcept;
    [[nodiscard]] const DailySourceConfig &config() const noexcept;
    // Half-open keys in the prepared dataset's numerical grid. Version resolves
    // exactly to manifest_sha256(); selection never rebases semantic keys.
    [[nodiscard]] BacktestResult<::lexongine::SourceRequest>
    request(const ::lexongine::SequenceScope &) const noexcept;

  private:
    explicit RegisteredDailySource(std::shared_ptr<const detail::DailySourceBinding> binding)
        : binding_(std::move(binding)) {}
    std::shared_ptr<const detail::DailySourceBinding> binding_;
    friend BacktestResult<RegisteredDailySource>
    register_daily_source(::lexongine::Registry &, ::lexongine::SourceResolver &,
                          const ::lexongine::RegisteredType<data::Bar> &, const DailyDataset &,
                          const DailySourceConfig &,
                          const quant::lexongine::ComponentRegistration &) noexcept;
};

// On registration failure discard the setup: the engine has no transactional
// rollback across its two registries. Only a successful return is a usable binding.
[[nodiscard]] BacktestResult<RegisteredDailySource>
register_daily_source(::lexongine::Registry &, ::lexongine::SourceResolver &,
                      const ::lexongine::RegisteredType<data::Bar> &, const DailyDataset &,
                      const DailySourceConfig &,
                      const quant::lexongine::ComponentRegistration &) noexcept;

} // namespace quant::research
