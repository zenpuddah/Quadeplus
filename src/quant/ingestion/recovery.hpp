#pragma once

#include "quant/ingestion/quality.hpp"

#include <span>
#include <vector>

namespace quant::ingestion {

enum class RecoveryAction {
    NoAction,
    Retry,
    RepairMissingRanges,
    Abort,
};

struct RecoveryContext {
    MarketDataQuery query;
    std::span<const DataStateSegment> state_segments;
    std::span<const DataStateSegment> unresolved_ranges;
    DataState data_state;
};

struct RecoveryPlan {
    RecoveryAction action;
    std::vector<DataStateSegment> ranges;
};

class RecoveryPolicy {
public:
    virtual ~RecoveryPolicy() = default;
    [[nodiscard]] virtual RecoveryPlan decide(const RecoveryContext& context) const = 0;
};

class NoopRecoveryPolicy final : public RecoveryPolicy {
public:
    [[nodiscard]] RecoveryPlan decide(const RecoveryContext&) const override {
        return RecoveryPlan{RecoveryAction::NoAction, {}};
    }
};

} // namespace quant::ingestion
