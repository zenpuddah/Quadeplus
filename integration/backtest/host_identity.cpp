#include "host_identity.hpp"
#include "lexongine/build_contract.hpp"
namespace quant::backtest {
::lexongine::Result<::lexongine::HostBuild>
inspect_financial_host(std::shared_ptr<::lexongine::ResourceAccount> account,
                       ::lexongine::HostBuildLimits limits) noexcept {
    return ::lexongine::inspect_host_build(::lexongine::build_contract_id,
                                           LEXONGINE_HOST_SOURCE_BUILD, std::move(account), limits);
}
} // namespace quant::backtest
