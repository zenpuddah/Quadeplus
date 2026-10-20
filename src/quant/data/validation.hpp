#pragma once

#include "quant/data/observations.hpp"

#include <string>
#include <utility>
#include <vector>

namespace quant::data {

struct ValidationIssue {
    std::string code;
    std::string message;
};

using ValidationIssues = std::vector<ValidationIssue>;

[[nodiscard]] inline bool is_valid(const ValidationIssues &issues) noexcept {
    return issues.empty();
}

inline void add_issue(ValidationIssues &issues, std::string code, std::string message) {
    issues.push_back(ValidationIssue{std::move(code), std::move(message)});
}

[[nodiscard]] inline ValidationIssues validate(const MboEvent &event) {
    ValidationIssues issues;

    if (event.side && *event.side != Side::Buy && *event.side != Side::Sell) {
        add_issue(issues, "unknown_side", "MBO side is outside the canonical vocabulary");
    }

    if (event.action == MboAction::Clear) {
        if (event.order_id || event.side || event.price || event.quantity) {
            add_issue(issues, "clear_has_order_fields", "Clear events must not carry order fields");
        }
        return issues;
    }

    if (!event.order_id) {
        add_issue(issues, "missing_order_id", "Non-clear MBO events require an order id");
    }

    switch (event.action) {
    case MboAction::Add:
        if (!event.side || !event.price || !event.quantity) {
            add_issue(issues, "incomplete_add", "Add events require side, price, and quantity");
        }
        break;
    case MboAction::Modify:
        if (!event.side && !event.price && !event.quantity) {
            add_issue(issues, "empty_modify", "Modify events require at least one order field");
        }
        break;
    case MboAction::Cancel:
    case MboAction::Execute:
        if (!event.quantity) {
            add_issue(issues, "missing_quantity", "Cancel and execute events require quantity");
        }
        break;
    case MboAction::Clear:
        break;
    default:
        add_issue(issues, "unknown_action", "MBO event has an unknown action");
        break;
    }

    return issues;
}

[[nodiscard]] inline ValidationIssues validate(const MboAdd &) {
    return {};
}

[[nodiscard]] inline ValidationIssues validate(const MboModify &event) {
    ValidationIssues issues;
    if (!event.side && !event.price && !event.quantity) {
        add_issue(issues, "empty_modify", "Modify events require at least one order field");
    }
    return issues;
}

[[nodiscard]] inline ValidationIssues validate(const MboCancel &) {
    return {};
}

[[nodiscard]] inline ValidationIssues validate(const MboExecute &) {
    return {};
}

[[nodiscard]] inline ValidationIssues validate(const MboClear &) {
    return {};
}

[[nodiscard]] inline ValidationIssues validate(const Trade &trade) {
    ValidationIssues issues;
    if (trade.quantity.is_zero()) {
        add_issue(issues, "zero_trade_quantity", "Trade quantity must be positive");
    }
    if (trade.aggressor_side && *trade.aggressor_side != Side::Buy &&
        *trade.aggressor_side != Side::Sell) {
        add_issue(issues, "unknown_side", "Trade side is outside the canonical vocabulary");
    }
    return issues;
}

[[nodiscard]] inline ValidationIssues validate(const OrderExecution &execution) {
    ValidationIssues issues;
    if (execution.executed_quantity.is_zero()) {
        add_issue(issues, "zero_execution_quantity", "Order execution quantity must be positive");
    }
    return issues;
}

[[nodiscard]] inline ValidationIssues validate(const Quote &quote) {
    ValidationIssues issues;
    if (quote.bid_price.has_value() != quote.bid_quantity.has_value()) {
        add_issue(issues, "incomplete_bid", "Bid price and quantity must be present together");
    }
    if (quote.ask_price.has_value() != quote.ask_quantity.has_value()) {
        add_issue(issues, "incomplete_ask", "Ask price and quantity must be present together");
    }
    if (quote.bid_price && quote.ask_price && *quote.bid_price > *quote.ask_price) {
        add_issue(issues, "crossed_quote", "Bid price must not exceed ask price");
    }
    return issues;
}

[[nodiscard]] inline ValidationIssues validate(const Bar &bar) {
    ValidationIssues issues;
    if (bar.interval_end <= bar.interval_start) {
        add_issue(issues, "invalid_bar_interval", "Bar interval end must be after its start");
    }
    if (bar.high < bar.low) {
        add_issue(issues, "invalid_bar_range", "Bar high must not be below its low");
    }
    if (bar.open < bar.low || bar.open > bar.high) {
        add_issue(issues, "open_outside_range", "Bar open must be within its high-low range");
    }
    if (bar.close < bar.low || bar.close > bar.high) {
        add_issue(issues, "close_outside_range", "Bar close must be within its high-low range");
    }
    return issues;
}

} // namespace quant::data
