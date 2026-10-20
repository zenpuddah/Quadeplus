#pragma once
#include "quant/lexongine/components.hpp"
namespace quant::strategies {
// Registration uses existing Bar/Price witnesses and the validated Price codec.
[[nodiscard]] ::lexongine::Result<void>
register_keltner_features(::lexongine::Registry &, const quant::lexongine::ComponentTypes &,
                          const quant::lexongine::ComponentRegistration &) noexcept;
} // namespace quant::strategies
