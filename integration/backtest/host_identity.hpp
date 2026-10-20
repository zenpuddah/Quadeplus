#pragma once
#include "lexongine/plugins.hpp"
namespace quant::backtest {
[[nodiscard]] ::lexongine::Result<::lexongine::HostBuild>
    inspect_financial_host(std::shared_ptr<::lexongine::ResourceAccount>,
                           ::lexongine::HostBuildLimits) noexcept;
}
