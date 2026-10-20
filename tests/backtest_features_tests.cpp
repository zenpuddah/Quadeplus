#include "quant/strategies/features.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/data/validation.hpp"
#include <iostream>
#include <stdexcept>

namespace {
namespace lx = ::lexongine;
namespace qd = quant::data;
template <class T, class E> T take(std::variant<T, E> value) {
    if (auto *e = std::get_if<E>(&value)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(value));
}
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
lx::Contract fact(std::string value) {
    return {std::move(value), 1, {1, {}}};
}
lx::ParamSet params(std::string_view kind) {
    lx::ParamSet p{{{"max_input_raw", "10000000000000", {"test", 1}},
                    {"rounding", "nearest_ties_even", {"test", 1}},
                    {"overflow", "fail", {"test", 1}}}};
    if (kind == "ema20") {
        p.values.push_back({"seed", "mean_first_20", {"test", 1}});
        p.values.push_back({"recurrence", "2_current_19_previous_over_21", {"test", 1}});
    } else if (kind == "atr10") {
        p.values.push_back({"seed", "mean_first_10", {"test", 1}});
        p.values.push_back({"recurrence", "9_previous_current_over_10", {"test", 1}});
    } else if (kind == "true_range") {
        p.values.push_back({"seed", "first_high_low", {"test", 1}});
    } else {
        p.values.push_back({"numerator", "3", {"test", 1}});
        p.values.push_back({"denominator", "2", {"test", 1}});
    }
    return p;
}
template <class T>
void publish(std::shared_ptr<lx::ResourceAccount> account, const lx::RegisteredType<T> &type,
             lx::ResultSlot &slot, const lx::UpdateContext &context, const T &value) {
    lx::PublicationAllocator alloc{account, 0};
    auto pending = take(lx::PendingOutcome::create(account, context, 1));
    take(pending.set_output(
        0, take(lx::ErasedValue::from(take(alloc.freeze(value, type)), type.policy.contract))));
    slot.commit(take(lx::validate_pending(std::move(pending), slot, context)));
}
void run(std::string name, bool changing) {
    auto account = take(lx::ResourceAccount::create(
        {{16777216, 16777216, 16777216, 16777216, 16777216, 33554432, 16777216},
         4096,
         4096,
         32768,
         128}));
    auto registry = take(lx::Registry::create());
    quant::lexongine::ComponentRegistration build{{lx::Sha256{std::byte{1}},
                                                   lx::Sha256{std::byte{2}},
                                                   lx::Sha256{std::byte{3}}, "feature-unit-v1"},
                                                  {},
                                                  lx::MissingIntervalPolicy::Strict};
    auto types = take(quant::lexongine::register_components(registry, build));
    take(quant::strategies::register_keltner_features(registry, types, build));
    take(registry.freeze());
    auto slot = [&](bool bar, unsigned id, std::uint64_t history = 0) {
        return take(lx::ResultSlot::create(
            account,
            {bar ? take(registry.describe_type<qd::Bar>(types.bar.policy.contract))
                 : take(registry.describe_type<qd::Price>(types.price.policy.contract))},
            history, lx::Sha256{static_cast<std::byte>(id)}, {}));
    };
    auto bars = slot(true, 10, 1), close = slot(false, 11), tr = slot(false, 12),
         ema = slot(false, 13), atr = slot(false, 14), upper = slot(false, 15);
    auto prepare = [&](std::string kind, bool stateful) {
        lx::PreparationFacts f{
            {"bars", 0, 81},
            {fact(stateful ? "empty_at_step_0" : "stateless"), {"bars", 0, 0}, {1, {}}},
            lx::MissingIntervalPolicy::Strict,
            lx::Alignment::SameStep,
            {65536, 262144, 65536, 262144, 65536, 16384, 65536},
            account};
        return take(
            take(registry.resolve({"quadeplus", kind, 1, "keltner-v1"}))->prepare(params(kind), f));
    };
    auto pe = prepare("ema20", true), pt = prepare("true_range", true), pa = prepare("atr10", true),
         pu = prepare("upper_channel", false);
    require(std::get<lx::CountHistory>(pe.history()).preceding == 19 &&
                std::get<lx::CountHistory>(pa.history()).preceding == 9,
            "feature readiness changed");
    lx::InstanceServices es{account, {{close, 0, 0}}}, ts{account, {{bars, 0, 1}}},
        as{account, {{tr, 0, 0}}}, us{account, {{ema, 0, 0}, {atr, 0, 0}}};
    auto ek = take(pe.instantiate(es)), tk = take(pt.instantiate(ts)),
         ak = take(pa.instantiate(as)), uk = take(pu.instantiate(us));
    for (std::uint64_t key = 0; key < 80; ++key) {
        const auto c = changing ? (key == 0   ? 100
                                   : key == 1 ? 110
                                              : 100 + static_cast<std::int64_t>((key * 7) % 13))
                                : 100;
        const auto scale = 1000000000LL;
        lx::UpdateContext context{{"bars", key},
                                  lx::Interval{static_cast<std::int64_t>(key * 10),
                                               static_cast<std::int64_t>(key * 10 + 1)},
                                  static_cast<std::int64_t>(key * 10 + 1),
                                  static_cast<std::int64_t>(key * 10 + 1)};
        qd::Bar bar{{qd::InstrumentId{1},
                     qd::VenueId{1},
                     qd::Timestamp::from_unix_nanos(context.activation_available_at_ns),
                     {},
                     {},
                     {},
                     0,
                     qd::SourceId{1}},
                    qd::Timestamp::from_unix_nanos(static_cast<std::int64_t>(key * 10)),
                    qd::Timestamp::from_unix_nanos(context.activation_available_at_ns),
                    qd::Price::from_raw(c * scale),
                    qd::Price::from_raw((c + 1) * scale),
                    qd::Price::from_raw((c - 1) * scale),
                    qd::Price::from_raw(c * scale),
                    qd::Quantity::from_raw(1)};
        publish(account, types.bar, *bars, context, bar);
        publish(account, types.price, *close, context, bar.close);
        tr->commit(take(lx::validate_pending(take(tk.step(context)), *tr, context)));
        ema->commit(take(lx::validate_pending(take(ek.step(context)), *ema, context)));
        atr->commit(take(lx::validate_pending(take(ak.step(context)), *atr, context)));
        const auto tr_value = take(tr->read<qd::Price>(0, context)).value.raw();
        require(tr->current()->parents.entries().size() == (key ? 2U : 1U),
                "TR previous-Bar provenance missing");
        if (changing && key == 1) {
            require(tr_value == 11 * scale, "TR gap example changed");
        }
        if (key < 19) {
            require(ema->current()->kind == lx::OutcomeKind::Warming &&
                        ema->current()->remaining_preceding == 19 - key,
                    "EMA warming differs from CountHistory");
        } else {
            require(ema->current()->parents.entries().size() == (key == 19 ? 20U : 1U),
                    "EMA seed/recurrence parents invalid");
        }
        if (key < 9) {
            require(atr->current()->kind == lx::OutcomeKind::Warming &&
                        atr->current()->remaining_preceding == 9 - key,
                    "ATR warming differs from CountHistory");
        } else {
            require(atr->current()->parents.entries().size() == (key == 9 ? 10U : 1U),
                    "ATR seed/recurrence parents invalid");
        }
        std::cout << name << ',' << key << ',' << bar.high.raw() << ',' << bar.low.raw() << ','
                  << bar.close.raw() << ',' << tr_value << ',';
        if (key >= 19) {
            auto value = take(ema->read<qd::Price>(0, context)).value.raw();
            if (!changing) {
                require(value == 100 * scale, "constant EMA moved");
            }
            std::cout << value;
        }
        std::cout << ',';
        if (key >= 9) {
            auto value = take(atr->read<qd::Price>(0, context)).value.raw();
            if (!changing) {
                require(value == 2 * scale, "constant ATR moved");
            }
            std::cout << value;
        }
        std::cout << ',';
        if (key >= 19) {
            upper->commit(take(lx::validate_pending(take(uk.step(context)), *upper, context)));
            std::cout << take(upper->read<qd::Price>(0, context)).value.raw();
        }
        std::cout << '\n';
    }
    lx::UpdateContext half{{"bars", 80}, lx::Interval{800, 801}, 801, 801};
    publish(account, types.price, *ema, half, qd::Price::from_raw(1));
    publish(account, types.price, *atr, half, qd::Price::from_raw(1));
    upper->commit(take(lx::validate_pending(take(uk.step(half)), *upper, half)));
    require(take(upper->read<qd::Price>(0, half)).value.raw() == 2,
            "channel rounded its product prematurely");
}
} // namespace
int main() {
    try {
        std::cout << "case,key,high,low,close,tr,ema,atr,upper\n";
        run("constant", false);
        run("changing", true);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
