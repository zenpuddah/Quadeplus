#pragma once

#include "quant/data/value_types.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quant::data {

struct ReferenceVersion {
    InstrumentId instrument_id;
    Timestamp valid_from;
    std::optional<Timestamp> valid_until;
    InstrumentType type;
    std::string symbol;
    std::optional<CurrencyCode> currency;
    std::optional<Price> tick_size;
    std::optional<Quantity> lot_size;
};

class ReferenceHistory {
public:
    explicit ReferenceHistory(const InstrumentId instrument_id) : instrument_id_(instrument_id) {}

    void append(ReferenceVersion version) {
        if (version.instrument_id != instrument_id_) {
            throw std::invalid_argument("ReferenceVersion instrument does not match history");
        }
        if (version.symbol.empty()) {
            throw std::invalid_argument("ReferenceVersion symbol must not be empty");
        }
        if (version.valid_until && *version.valid_until <= version.valid_from) {
            throw std::invalid_argument("ReferenceVersion validity interval must be positive");
        }
        if (version.tick_size && !version.tick_size->is_positive()) {
            throw std::invalid_argument("ReferenceVersion tick size must be positive");
        }
        if (version.lot_size && version.lot_size->is_zero()) {
            throw std::invalid_argument("ReferenceVersion lot size must be positive");
        }

        if (!versions_.empty()) {
            const auto& previous = versions_.back();
            if (!previous.valid_until) {
                throw std::invalid_argument("Cannot append after an open reference interval");
            }
            if (version.valid_from < *previous.valid_until) {
                throw std::invalid_argument("ReferenceVersion intervals must not overlap");
            }
        }

        versions_.push_back(std::move(version));
    }

    [[nodiscard]] const ReferenceVersion* at(const Timestamp time) const noexcept {
        const auto candidate = std::upper_bound(
            versions_.begin(),
            versions_.end(),
            time,
            [](const Timestamp lookup_time, const ReferenceVersion& version) {
                return lookup_time < version.valid_from;
            });
        if (candidate == versions_.begin()) {
            return nullptr;
        }

        const auto& version = *(candidate - 1);
        if (!version.valid_until || time < *version.valid_until) {
            return &version;
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return versions_.size(); }

private:
    InstrumentId instrument_id_;
    std::vector<ReferenceVersion> versions_;
};

} // namespace quant::data
