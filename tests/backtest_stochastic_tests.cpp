#include "quant/strategies/stochastic.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/storage/file_integrity.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
namespace lx = ::lexongine;
namespace qs = quant::strategies;
namespace qb = quant::backtest;
namespace qd = quant::data;
using K = qs::StochasticValue;
constexpr std::int64_t scale = 1000000000;

template <class T, class E> T take(std::variant<T, E> result) {
    if (const auto *error = std::get_if<E>(&result)) {
        throw std::runtime_error(std::string{error->text()});
    }
    return std::get<T>(std::move(result));
}
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
template <class T> void fails(std::variant<T, qb::BacktestError> result, qb::ErrorCode code) {
    const auto *error = std::get_if<qb::BacktestError>(&result);
    require(error && error->code == code, "missing/wrong financial failure");
}
template <class T> void fails(std::variant<T, lx::Diagnostic> result) {
    require(std::holds_alternative<lx::Diagnostic>(result), "missing engine failure");
}
qd::Price price(std::int64_t value) {
    return qd::Price::from_raw(value * scale);
}
qd::Money money(std::int64_t cents) {
    return qd::Money::from_raw(qd::CurrencyCode{"USD"}, cents);
}
lx::Contract fact(std::string value) {
    return {std::move(value), 1, {1, {}}};
}
std::shared_ptr<lx::ResourceAccount> resources() {
    return take(lx::ResourceAccount::create(
        {{16777216, 16777216, 16777216, 16777216, 16777216, 33554432, 16777216},
         4096,
         4096,
         65536,
         128}));
}
lx::ParamSet parameters() {
    return {{{"window", "5", {"stochastic-test", 1}},
             {"zero_range", "1/2", {"stochastic-test", 1}},
             {"coverage", "strict_complete_bars", {"stochastic-test", 1}}}};
}
lx::UpdateContext context(std::uint64_t key) {
    const auto t = static_cast<std::int64_t>(key * 10);
    return {{"bars", key}, lx::Interval{t, t + 1}, t + 1, t + 1};
}
qd::Bar bar(std::uint64_t key, std::int64_t low = 50 * scale, std::int64_t high = 150 * scale,
            std::int64_t close = 100 * scale) {
    const auto t = static_cast<std::int64_t>(key * 10);
    return {{qd::InstrumentId{1},
             qd::VenueId{1},
             qd::Timestamp::from_unix_nanos(t + 1),
             {},
             {},
             {},
             0,
             qd::SourceId{1}},
            qd::Timestamp::from_unix_nanos(t),
            qd::Timestamp::from_unix_nanos(t + 1),
            qd::Price::from_raw(close),
            qd::Price::from_raw(high),
            qd::Price::from_raw(low),
            qd::Price::from_raw(close),
            qd::Quantity::from_raw(1)};
}
template <class T>
void publish(const std::shared_ptr<lx::ResourceAccount> &account, const lx::RegisteredType<T> &type,
             lx::ResultSlot &slot, const lx::UpdateContext &at, const T &value) {
    lx::PublicationAllocator allocator{account, 0};
    auto pending = take(lx::PendingOutcome::create(account, at, 1));
    take(pending.set_output(
        0, take(lx::ErasedValue::from(take(allocator.freeze(value, type)), type.policy.contract))));
    slot.commit(take(lx::validate_pending(std::move(pending), slot, at)));
}
struct Numerical {
    std::shared_ptr<lx::ResourceAccount> account = resources();
    lx::Registry registry = take(lx::Registry::create());
    quant::lexongine::ComponentRegistration build{{lx::Sha256{std::byte{1}},
                                                   lx::Sha256{std::byte{2}},
                                                   lx::Sha256{std::byte{3}}, "stochastic-test-v1"},
                                                  {},
                                                  lx::MissingIntervalPolicy::Strict};
    quant::lexongine::ComponentTypes types =
        take(quant::lexongine::register_components(registry, build));
    lx::RegisteredType<K> type = take(qs::register_stochastic(registry, types, build));
    std::shared_ptr<lx::ResultSlot> input, output;
    Numerical() {
        take(registry.freeze());
        input = take(lx::ResultSlot::create(
            account, {take(registry.describe_type<qd::Bar>(types.bar.policy.contract))}, 0,
            lx::Sha256{std::byte{10}}, {}));
        output = take(
            lx::ResultSlot::create(account, {take(registry.describe_type<K>(type.policy.contract))},
                                   0, lx::Sha256{std::byte{11}}, {}));
    }
    auto definition() {
        return take(registry.resolve({"quadeplus", "stochastic5", 1, "stochastic-v1"}));
    }
    lx::PreparationFacts facts() {
        return {{"bars", 0, 100},
                {fact("empty_at_step_0"), {"bars", 0, 0}, {1, {}}},
                lx::MissingIntervalPolicy::Strict,
                lx::Alignment::SameStep,
                {65536, 262144, 65536, 262144, 65536, 16384, 65536},
                account};
    }
    lx::PreparedKernel prepare() {
        return take(definition()->prepare(parameters(), facts()));
    }
    lx::KernelInstance instance(const lx::PreparedKernel &prepared) {
        lx::InstanceServices services{account, {{input, 0, 0}}};
        return take(prepared.instantiate(services));
    }
    std::optional<K> step(lx::KernelInstance &kernel, const qd::Bar &value,
                          const lx::UpdateContext &at) {
        publish(account, types.bar, *input, at, value);
        output->commit(take(lx::validate_pending(take(kernel.step(at)), *output, at)));
        if (output->current()->kind == lx::OutcomeKind::Warming) {
            return {};
        }
        return take(output->read<K>(0, at)).value;
    }
};

void numerical_window() {
    Numerical n;
    const auto &contract = n.definition()->contract();
    require(contract.sharing.classification == lx::SharingClass::StatefulDeclared &&
                contract.sharing.effect_free && contract.sharing.replay_safe &&
                !contract.sharing.cacheable && !contract.outputs[0].cacheable,
            "stochastic sharing/cache contract changed");
    require(contract.outputs[0].codec.id == "none" &&
                contract.outputs[0].type != n.types.price.policy.contract,
            "oscillator disguised as Price/codec");
    require(n.type.policy.storage == lx::StorageKind::ScalarCopy &&
                n.type.policy.freeze_strategy == lx::FreezeStrategy::ScalarCopy,
            "wrong ratio storage policy");
    for (K invalid : {K{-1, 1}, K{0, 0}, K{1, -1}, K{2, 1}}) {
        fails(n.type.policy.validate(invalid));
    }
    take(n.type.policy.validate(K{0, 1}));
    take(n.type.policy.validate(K{1, 1}));
    auto prepared = n.prepare();
    require(std::get<lx::CountHistory>(prepared.history()).preceding == 4,
            "five-Bar readiness history changed");
    auto kernel = n.instance(prepared);
    std::vector<qd::Bar> history;
    for (std::uint64_t key = 0; key < 32; ++key) {
        const auto low = static_cast<std::int64_t>(key + 1) * scale;
        const auto high = low + 100 * scale;
        const auto value = bar(key, low, high, low + static_cast<std::int64_t>(key % 11) * scale);
        history.push_back(value);
        const auto got = n.step(kernel, value, context(key));
        if (key < 4) {
            require(!got && n.output->current()->remaining_preceding == 4 - key,
                    "incorrect Warming countdown");
            continue;
        }
        std::int64_t lo = value.low.raw(), hi = value.high.raw();
        for (std::size_t j = static_cast<std::size_t>(key - 4); j <= key; ++j) {
            lo = std::min(lo, history[j].low.raw());
            hi = std::max(hi, history[j].high.raw());
        }
        require(got == K{value.close.raw() - lo, hi - lo},
                "rolling extrema/close differ from independent window");
        const auto parents = n.output->current()->parents.entries();
        require(parents.size() == 5, "five input certificates not retained");
        for (std::size_t j = 0; j < 5; ++j) {
            require(parents[j].metadata().key.sequence == key - 4 + j,
                    "parents are not bounded oldest-first");
        }
    }
    // New instance from the same prepared kernel must not inherit rolling state.
    Numerical flat;
    auto flat_prepared = flat.prepare();
    auto flat_kernel = flat.instance(flat_prepared);
    for (std::uint64_t key = 0; key < 5; ++key) {
        const auto got = flat.step(flat_kernel, bar(key, scale, scale, scale), context(key));
        require(key == 4 ? got == K{1, 2} : !got, "flat/fresh kernel state invalid");
    }
    auto second = flat.instance(flat_prepared);
    // Use a separate source slot at its origin; share the prepared definition only.
    flat.input = take(lx::ResultSlot::create(
        flat.account, {take(flat.registry.describe_type<qd::Bar>(flat.types.bar.policy.contract))},
        0, lx::Sha256{std::byte{12}}, {}));
    lx::InstanceServices fresh_services{flat.account, {{flat.input, 0, 0}}};
    second = take(flat_prepared.instantiate(fresh_services));
    publish(flat.account, flat.types.bar, *flat.input, context(0), bar(0));
    auto first = take(second.step(context(0)));
    auto fresh_output = take(lx::ResultSlot::create(
        flat.account, {take(flat.registry.describe_type<K>(flat.type.policy.contract))}, 0,
        lx::Sha256{std::byte{13}}, {}));
    fresh_output->commit(take(lx::validate_pending(std::move(first), *fresh_output, context(0))));
    require(fresh_output->current()->kind == lx::OutcomeKind::Warming,
            "fresh instance inherited ready state");
    Numerical boundaries;
    auto bp = boundaries.prepare();
    auto bk = boundaries.instance(bp);
    const std::array<std::int64_t, 8> closes{100, 100, 100, 100, 70, 69, 130, 131};
    for (std::uint64_t key = 0; key < closes.size(); ++key) {
        const auto got = boundaries.step(bk, bar(key, 50 * scale, 150 * scale, closes[key] * scale),
                                         context(key));
        if (key >= 4) {
            require(got == K{(closes[key] - 50) * scale, 100 * scale},
                    "typed K20/K80 boundary changed");
        }
    }
    std::cout << "PASS numerical rolling/readiness/type/provenance/fresh state\n";
}

void numerical_guards() {
    Numerical n;
    auto bad = parameters();
    bad.values[0].text = "6";
    fails(n.definition()->prepare(bad, n.facts()));
    bad = parameters();
    bad.values[1].text = "0";
    fails(n.definition()->prepare(bad, n.facts()));
    auto facts = n.facts();
    facts.missing_intervals = lx::MissingIntervalPolicy::Skip;
    fails(n.definition()->prepare(parameters(), facts));
    facts = n.facts();
    facts.processing_scope.begin = 5;
    facts.initialization.actual_prefix = {"bars", 5, 5};
    fails(n.definition()->prepare(parameters(), facts));
    auto prepared = n.prepare();
    auto kernel = n.instance(prepared);
    auto mismatch = bar(0);
    mismatch.interval_end = qd::Timestamp::from_unix_nanos(2);
    publish(n.account, n.types.bar, *n.input, context(0), mismatch);
    fails(kernel.step(context(0)));
    Numerical early;
    auto ep = early.prepare();
    auto ek = early.instance(ep);
    auto before_close = context(0);
    before_close.activation_available_at_ns = before_close.decision_time_ns = 0;
    publish(early.account, early.types.bar, *early.input, before_close, bar(0));
    fails(ek.step(before_close));
    Numerical gap;
    auto gp = gap.prepare();
    auto gk = gap.instance(gp);
    require(!gap.step(gk, bar(0), context(0)), "first Bar ready");
    publish(gap.account, gap.types.bar, *gap.input, context(2), bar(2));
    fails(gk.step(context(2)));
    Numerical wide;
    auto wp = wide.prepare();
    auto wk = wide.instance(wp);
    for (std::uint64_t key = 0; key < 5; ++key) {
        const auto got = wide.step(wk, bar(key, 1, INT64_MAX, INT64_MAX), context(key));
        if (key == 4) {
            require(got == K{INT64_MAX - 1, INT64_MAX - 1}, "wide Price9 ratio overflowed");
        }
    }
    lx::PublicationAllocator allocator{n.account, 0};
    auto invalid = bar(0);
    invalid.close = price(200);
    fails(allocator.freeze(invalid, n.types.bar));
    fails(allocator.freeze(K{5, 4}, n.type));
    std::cout << "PASS numerical parameter/origin/coverage/extent/availability/domain guards\n";
}

struct Environment {
    quant::research::DailyDataset data;
    auto open(std::uint64_t key) const {
        return take(data.opening(key, data.rows()[key].bar.interval_start.unix_nanos()));
    }
    auto close(std::uint64_t key) const {
        return take(data.closing(key, data.rows()[key].close_available_ns));
    }
    auto account(std::int64_t cash = 1000000) const {
        return take(qb::fund_account(1, open(0).source(), money(cash), qd::Quantity::from_raw(1)));
    }
};
Environment fixture(const std::filesystem::path &root) {
    namespace qr = quant::research;
    namespace files = quant::storage::files;
    require(std::filesystem::create_directory(root), "test fixture directory already exists");
    std::ostringstream csv, grid;
    csv << "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n";
    grid << "Date\n";
    std::vector<std::string> dates;
    const auto origin = std::chrono::sys_days{std::chrono::year{2000} / 1 / 1};
    for (unsigned i = 0; i < 25; ++i) {
        const auto d = std::chrono::year_month_day{origin + std::chrono::days{i}};
        std::ostringstream text;
        text << static_cast<int>(d.year()) << '-' << std::setfill('0') << std::setw(2)
             << static_cast<unsigned>(d.month()) << '-' << std::setw(2)
             << static_cast<unsigned>(d.day());
        dates.push_back(text.str());
        grid << dates.back() << '\n';
        const auto c = i < 4 ? 100 : 60;
        csv << dates.back() << ",100,150,50," << c << ",1," << c << ','
            << (i == 5 || i == 15 ? "1.889" : "0") << '\n';
    }
    const auto prices = csv.str(), sessions = grid.str();
    files::write_text(root / "prices.csv", prices);
    files::write_text(root / "sessions.csv", sessions);
    qr::DatasetSpec spec{"stochastic-unit-v1",
                         "TEST",
                         "USD",
                         "authored-stochastic-fixture",
                         1,
                         1,
                         1,
                         "synthetic_session_grid_v1",
                         std::string{qr::adjustment_profile},
                         std::string{qr::decimal_profile},
                         std::string{qr::volume_profile},
                         std::string{qr::clock_profile},
                         dates.front(),
                         dates[4],
                         dates.back()};
    return {take(qr::prepare_daily_dataset({root / "prices.csv",
                                            root / "sessions.csv",
                                            root / "dataset",
                                            files::sha256(prices),
                                            files::sha256(sessions),
                                            spec,
                                            {1048576, 32, 64},
                                            resources()}))};
}
struct Simulation {
    qs::StochasticState state;
    std::optional<qb::Account> account;
    std::optional<qb::Ledger> ledger;
    std::vector<qb::Fill> fills;
    std::vector<std::uint64_t> rejected;
    std::vector<qb::Action> decisions;
    std::vector<std::int64_t> payments;
    std::optional<qb::BacktestError> failure;
    bool expired = false;
};
Simulation run(const Environment &e, qs::StochasticProfile profile, std::uint64_t begin,
               std::uint64_t end, std::int64_t cash = 1000000,
               qb::TerminalMode terminal = qb::TerminalMode::Mark,
               qb::ExecutionPolicy costs = {0, 0},
               const std::vector<std::optional<K>> &override_k = {}) {
    Simulation result;
    result.state.profile = profile;
    std::optional<qb::Intent> pending;
    std::uint64_t id = 1;
    for (std::uint64_t key = 0; key <= end; ++key) {
        if (key == begin - 1) {
            result.account = e.account(cash);
            result.ledger = take(qb::Ledger::create(*result.account, 128));
        }
        if (key >= begin) {
            auto &a = *result.account;
            const auto opening = e.open(key);
            auto mark = take(qb::prior_close_valuation(e.close(key - 1), opening));
            const auto d = e.data.rows()[key].dividend;
            result.payments.push_back(take(qb::settle_dividend(d, a.units)).amount());
            take(qb::apply_dividend(a, *result.ledger, {a.trial, mark, d, a.units}));
            if (pending) {
                auto quote = take(qb::quote_fill(*pending, opening, a, costs));
                if (const auto *f = std::get_if<qb::Fill>(&quote)) {
                    take(qb::apply_fill(a, *result.ledger, *f));
                    result.fills.push_back(*f);
                } else {
                    result.rejected.push_back(key);
                }
                const auto notified = qs::stochastic_execution(result.state, quote, a);
                if (const auto *error = std::get_if<qb::BacktestError>(&notified)) {
                    result.failure = *error;
                    return result;
                }
                pending.reset();
            }
        }
        const bool may_submit =
            key >= begin || (profile == qs::StochasticProfile::Legacy && key == begin - 1);
        const auto k = override_k.empty() ? (key < 4 ? std::optional<K>{} : std::optional{K{1, 10}})
                                          : override_k.at(key);
        auto decision = take(
            qs::stochastic_decision(result.state, {key, may_submit, result.account.has_value()},
                                    result.account ? &*result.account : nullptr, k));
        result.decisions.push_back(decision);
        if (result.account) {
            const auto &a = *result.account;
            auto mark = take(qb::closing_valuation(e.close(key)));
            take(qb::verify_financial_invariants(
                a, *result.ledger, take(qb::mark_account(a, mark, qb::ValuationPhase::Close))));
            if (decision.kind == qb::ActionKind::TargetLong) {
                pending = qb::Intent{a.trial,
                                     id++,
                                     a.source,
                                     key,
                                     key + 1,
                                     decision.reason,
                                     take(qb::freeze_cash_budget(a))};
            } else if (decision.kind == qb::ActionKind::TargetCash) {
                pending = qb::Intent{a.trial,         id++,         a.source, key, key + 1,
                                     decision.reason, qb::SellAll{}};
            }
        }
    }
    result.expired = pending.has_value();
    if (terminal == qb::TerminalMode::SyntheticClose && result.account->units.raw() > 0) {
        auto quote = take(
            qb::quote_terminal_liquidation(e.close(end), *result.account, costs, {terminal, id}));
        const auto fill = std::get<qb::Fill>(quote);
        take(qb::apply_fill(*result.account, *result.ledger, fill));
        take(qs::stochastic_execution(result.state, quote, *result.account));
        result.fills.push_back(fill);
    }
    auto mark = take(qb::closing_valuation(e.close(end)));
    take(qb::verify_financial_invariants(
        *result.account, *result.ledger,
        take(qb::mark_account(*result.account, mark, qb::ValuationPhase::Close))));
    return result;
}

void thresholds_and_timer(const Environment &e) {
    std::vector<std::optional<K>> k(9);
    k[4] = K{1, 5};
    k[5] = K{19, 100};
    k[6] = K{4, 5};
    k[7] = K{81, 100};
    k[8] = K{1, 10};
    const auto thresholds =
        run(e, qs::StochasticProfile::Fresh, 4, 8, 1000000, qb::TerminalMode::Mark, {0, 0}, k);
    require(thresholds.decisions[4].kind == qb::ActionKind::Hold, "K20 entered");
    require(thresholds.decisions[6].kind == qb::ActionKind::Hold, "K80 exited");
    require(thresholds.decisions[7].reason == "threshold_exit" && thresholds.fills.size() == 2 &&
                thresholds.fills[0].mark.key() == 6 && thresholds.fills[1].mark.key() == 8,
            "threshold next-open dates changed");
    const auto timer = run(e, qs::StochasticProfile::Fresh, 4, 18);
    require(timer.fills.size() == 3 && timer.fills[0].mark.key() == 5 &&
                timer.fills[1].mark.key() == 15 && timer.fills[2].mark.key() == 16,
            "timer holding sessions/next episode changed");
    require(timer.decisions[14].kind == qb::ActionKind::TargetCash &&
                timer.decisions[14].reason == "timer_exit" &&
                timer.decisions[15].kind == qb::ActionKind::TargetLong,
            "timer reset or same-close reentry");
    require(timer.fills[0].quantity.raw() == 95000050, "largest settled-affordable lot changed");
    // Full-width numerators/denominators must not overflow strict comparisons.
    const auto a = e.account();
    for (const auto denominator : {std::int64_t{5}, INT64_MAX - 2, INT64_MAX}) {
        qs::StochasticState s;
        const auto numerator = denominator / 5;
        auto value =
            take(qs::stochastic_decision(s, {4, true, true}, &a, K{numerator, denominator}));
        require((value.kind == qb::ActionKind::TargetLong) == (denominator % 5 != 0),
                "wide K20 boundary changed");
        qs::StochasticState upper;
        take(qs::stochastic_decision(upper, {4, true, true}, &a, K{0, 1}));
        take(qs::stochastic_execution(
            upper, qb::OrderRejection{a.trial, 1, qb::RejectionReason::BelowMinimumLot}, a));
        auto exit = take(qs::stochastic_decision(upper, {5, true, true}, &a,
                                                 K{denominator - denominator / 5, denominator}));
        require((exit.reason == "threshold_exit") == (denominator % 5 != 0),
                "wide K80 boundary changed");
    }
    std::cout << "PASS strict thresholds/ten-session timer/no same-close reentry/wide ratios\n";
}

void profiles_and_rejections(const Environment &e) {
    const auto legacy = run(e, qs::StochasticProfile::Legacy, 9, 18);
    const auto fresh = run(e, qs::StochasticProfile::Fresh, 9, 18);
    require(legacy.fills[0].mark.key() == 9 && legacy.fills[1].mark.key() == 15,
            "legacy funding-mid-episode reset timer");
    require(fresh.fills.size() == 1 && fresh.fills[0].mark.key() == 10 &&
                fresh.state.episode_start == 9,
            "fresh profile inherited pre-evaluation episode");
    const auto rejected =
        run(e, qs::StochasticProfile::Fresh, 4, 18, 1, qb::TerminalMode::Mark, {5, 5});
    require(rejected.rejected == std::vector<std::uint64_t>{5, 16} && rejected.fills.empty() &&
                rejected.decisions[14].reason == "timer_exit",
            "rejected fresh episode retried/reset timer");
    const auto failed =
        run(e, qs::StochasticProfile::Legacy, 9, 18, 1, qb::TerminalMode::Mark, {5, 5});
    require(failed.failure && failed.failure->code == qb::ErrorCode::Integrity &&
                failed.state.parity_failed && failed.rejected == std::vector<std::uint64_t>{9} &&
                failed.fills.empty(),
            "legacy rejection passed parity");
    std::cout
        << "PASS legacy/fresh initialization/rejected episode consumed/fatal legacy rejection\n";
}

void endpoint_dividend_and_guards(const Environment &e) {
    const auto marked =
        run(e, qs::StochasticProfile::Fresh, 4, 5, 1000000, qb::TerminalMode::Mark, {5, 5});
    const auto terminal = run(e, qs::StochasticProfile::Fresh, 4, 5, 1000000,
                              qb::TerminalMode::SyntheticClose, {5, 5});
    require(marked.fills.size() == 1 && marked.account->units.raw() > 0 &&
                terminal.fills.size() == 2 && terminal.account->units.raw() == 0 &&
                terminal.fills[1].synthetic_terminal && !terminal.fills[1].intent &&
                terminal.fills[0].mark.key() == terminal.fills[1].mark.key(),
            "final morning buy/terminal sale/Mark changed");
    require(terminal.state.terminal && terminal.state.held_units == 0,
            "synthetic sale notification lost");
    const auto expired = run(e, qs::StochasticProfile::Fresh, 4, 4);
    require(expired.expired && expired.fills.empty(), "last signal invented a fill");
    const auto dividends = run(e, qs::StochasticProfile::Fresh, 4, 15);
    require(dividends.payments[1] == 0, "ex-date buyer received dividend");
    require(
        dividends.payments.back() ==
                take(qb::settle_dividend(qb::DividendPerUnit9{qd::CurrencyCode{"USD"}, 1889000000},
                                         dividends.fills[0].quantity))
                    .amount() &&
            dividends.payments.back() > 0 && dividends.fills.back().mark.key() == 15,
        "ex-date seller lost prior holdings dividend");
    auto a = e.account();
    qs::StochasticState missing;
    take(qs::stochastic_decision(missing, {4, false, false}, nullptr, K{1, 2}));
    fails(qs::stochastic_decision(missing, {5, false, false}, nullptr, {}),
          qb::ErrorCode::SourceBinding);
    fails(qs::stochastic_decision(missing, {6, false, false}, nullptr, K{1, 2}),
          qb::ErrorCode::Coverage);
    qs::StochasticState late;
    late.profile = qs::StochasticProfile::Legacy;
    fails(qs::stochastic_decision(late, {100, true, true}, &a, K{1, 2}), qb::ErrorCode::Coverage);
    qs::StochasticState invalid;
    fails(qs::stochastic_decision(invalid, {4, true, true}, &a, K{2, 1}),
          qb::ErrorCode::InvalidData);
    fails(qs::stochastic_decision(invalid, {4, true, false}, nullptr, K{1, 2}),
          qb::ErrorCode::Integrity);
    fails(qs::stochastic_decision(invalid, {3, false, false}, nullptr, K{1, 2}),
          qb::ErrorCode::SourceBinding);
    auto duplicate = marked.state;
    fails(qs::stochastic_execution(duplicate, marked.fills[0], *marked.account),
          qb::ErrorCode::Integrity);
    auto mismatched = a;
    mismatched.units = qd::Quantity::from_raw(1);
    fails(qs::stochastic_decision(invalid, {4, true, true}, &mismatched, K{1, 2}),
          qb::ErrorCode::Integrity);
    auto wrong_trial = marked.state;
    auto other = *marked.account;
    other.trial = 2;
    fails(qs::stochastic_execution(
              wrong_trial, qb::OrderRejection{2, 1, qb::RejectionReason::BelowMinimumLot}, other),
          qb::ErrorCode::Integrity);
    qs::StochasticState overflow;
    take(qs::stochastic_decision(overflow, {UINT64_MAX, true, true}, &a, K{1, 2}));
    fails(qs::stochastic_decision(overflow, {0, true, true}, &a, K{1, 2}), qb::ErrorCode::Coverage);
    // A future close cannot change the already eligible buy; policy is invoked
    // only after the opening fill, with current typed K and immutable account.
    std::vector<std::optional<K>> changed(6);
    changed[4] = K{1, 10};
    changed[5] = K{99, 100};
    const auto future = run(e, qs::StochasticProfile::Fresh, 4, 5, 1000000, qb::TerminalMode::Mark,
                            {5, 5}, changed);
    require(future.fills[0] == marked.fills[0], "current close changed eligible opening fill");
    std::cout << "PASS dividend entitlement/terminal sale/Mark/expiry/causality/policy guards\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: backtest_stochastic_tests <new-fixture-directory>");
        numerical_window();
        numerical_guards();
        const auto environment = fixture(std::filesystem::absolute(argv[1]));
        thresholds_and_timer(environment);
        profiles_and_rejections(environment);
        endpoint_dividend_and_guards(environment);
        std::cout << "STOCHASTIC_COMPONENT_POLICY_PASS (direct kernel/accounting; hosted S3 "
                     "integration not exercised)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
