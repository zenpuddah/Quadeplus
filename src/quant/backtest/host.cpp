#include "quant/backtest/host.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/strategies/stochastic.hpp"
#include "quant/backtest/detail.hpp"
#include "native_files.hpp"
#include "plan_projection.hpp"
#include "quant/lexongine/components.hpp"
#include <fcntl.h>
#include <sstream>
#include <unistd.h>

namespace quant::backtest {
namespace {
namespace lx = ::lexongine;
using detail::take;
using detail::fail;
template <class T> T engine(std::variant<T, lx::Diagnostic> result) {
    if (const auto *e = std::get_if<lx::Diagnostic>(&result)) {
        fail(ErrorCode::SourceBinding, e->text());
    }
    return std::get<T>(std::move(result));
}
std::string hex(const lx::Sha256 &digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : digest) {
        auto v = std::to_integer<unsigned>(b);
        out += digits[v >> 4];
        out += digits[v & 15];
    }
    return out;
}
void text(std::string_view value, std::size_t max) {
    if (value.empty() || value.size() > max || value.find('\0') != std::string_view::npos) {
        fail(ErrorCode::InvalidConfig, "invalid bounded host text");
    }
}
std::string configuration(const SuiteConfiguration &suite, const research::DailyDataset &data,
                          const lx::PreparedPlan &plan, const lx::HostBuild &build) {
    std::ostringstream out;
    auto field = [&](std::string_view v) { out << v.size() << ':' << v << ';'; };
    field("quadeplus_financial_host_v1");
    field(data.manifest_sha256());
    field(execution_profile);
    field(dividend_profile);
    const auto &id = build.identity();
    field(hex(id.binary_digest));
    field(hex(id.source_build_digest));
    field(hex(id.dependencies_digest));
    field(id.sdk_build_contract_id);
    field(hex(engine(lx::plan_digest(plan, data.account(),
                                     {{67108864, 4194304, 64, 65536, 536870912}, 134217728}))));
    out << suite.funding_key << ',' << suite.evaluation_begin << ',' << suite.evaluation_end << ','
        << suite.legacy_funding_close << ';';
    out << suite.limits.max_trials << ',' << suite.limits.max_ledger_events << ','
        << suite.limits.max_trace_records << ',' << suite.limits.max_evidence_bytes << ';';
    for (const auto &t : suite.trials) {
        out << t.id << ';';
        field(t.strategy_id);
        field(t.strategy_configuration);
        field(t.initial_cash.currency().value());
        out << t.initial_cash.amount() << ',' << t.lot_step.raw() << ','
            << t.execution.commission_bps << ',' << t.execution.slippage_bps << ','
            << static_cast<int>(t.terminal) << ',' << t.risk.drawdown_bps.value_or(0) << ','
            << t.risk.daily_loss_bps.value_or(0) << ';';
        for (const auto &r : t.observations) {
            field(r.id);
            out << r.begin << ',' << r.end << ',' << static_cast<int>(r.kind) << ';';
        }
    }
    return out.str();
}
BacktestResult<void> sticky(BacktestResult<void> result, FinancialRun &financial) {
    if (const auto *e = std::get_if<BacktestError>(&result); e && !financial.failure) {
        financial.failure = *e;
    }
    return result;
}
struct FeatureInbox {
    lx::Reservation charge;
    std::vector<PriceFeature> values;
    std::vector<RatioFeature> ratios;
    std::size_t maximum = 0;
    std::uint64_t expected_key = 0;
    bool collecting = false;
};
} // namespace
struct HostedSimulation::State {
    research::DailyDataset dataset;
    std::shared_ptr<const lx::PreparedPlan> plan;
    SuiteConfiguration config;
    std::vector<DecisionCallback> decisions;
    lx::RegisteredType<data::Price> price;
    std::shared_ptr<FinancialRun> financial;
    std::shared_ptr<FeatureInbox> inbox;
    std::vector<ExecutionCallback> executions;
    std::optional<lx::RegisteredType<strategies::StochasticValue>> ratio_type;
    std::uint64_t next_key = 0;
    bool opening = false, bound = false, started = false;
    void healthy() {
        if (financial->failure || financial->finalized) {
            fail(ErrorCode::InvalidData, "financial host is stopped or finalized");
        }
    }
    void record(std::size_t trial, std::uint64_t key, std::string kind, std::string reason,
                const ValuationProjection *mark = nullptr, const Fill *fill = nullptr,
                std::uint64_t id = 0) {
        if (financial->events.size() == config.limits.max_trace_records) {
            fail(ErrorCode::ResourceExceeded, "host trace record ceiling exceeded");
        }
        const auto &t = financial->trials[trial];
        const auto &a = t.account;
        std::optional<std::int64_t> equity;
        if (a && mark) {
            equity = take(mark_account(*a, *mark, mark->phase())).equity.amount();
        }
        const Intent *intent = fill && fill->intent ? &*fill->intent
                               : id && t.pending    ? &*t.pending
                                                    : nullptr;
        std::string policy = "none";
        std::int64_t budget = 0, quantity = fill ? fill->quantity.raw() : 0;
        if (intent) {
            if (const auto *b = std::get_if<BuyCashBudget>(&intent->order)) {
                policy = "cash_budget";
                budget = b->budget.amount();
            } else if (const auto *q = std::get_if<BuyFixedQuantity>(&intent->order)) {
                policy = "fixed_quantity";
                if (!fill) {
                    quantity = q->quantity.raw();
                }
            } else {
                policy = "sell_all";
            }
        }
        financial->events.push_back(
            {config.trials[trial].id,
             key,
             fill ? fill->id : id,
             std::move(kind),
             std::move(reason),
             a ? a->cash.amount() : 0,
             a ? a->units.raw() : 0,
             a ? a->basis.amount() : 0,
             a ? a->realized.amount() : 0,
             a ? a->dividends.amount() : 0,
             a ? a->fees.amount() : 0,
             equity,
             mark ? std::optional{mark->phase()} : std::nullopt,
             fill   ? fill->executed_price.raw()
             : mark ? mark->price().raw()
                    : 0,
             quantity,
             fill ? fill->gross.amount() : 0,
             fill ? fill->fee.amount() : 0,
             intent ? std::optional{intent->decision_key} : std::nullopt,
             intent ? std::optional{intent->earliest_open_key} : std::nullopt,
             std::move(policy),
             budget});
    }
    void queue(std::size_t trial, std::uint64_t key, Action action, const RiskDecision &risk) {
        auto &t = financial->trials[trial];
        auto &a = *t.account;
        if (action.kind == ActionKind::Hold && !risk.liquidate) {
            return;
        }
        if (t.pending) {
            fail(ErrorCode::InvalidData, "more than one pending intent");
        }
        if (risk.liquidate) {
            action = {ActionKind::TargetCash, data::Quantity::from_raw(0), "risk_exit"};
        }
        if (risk.block_entries &&
            (action.kind == ActionKind::TargetLong || action.kind == ActionKind::FixedEntry)) {
            record(trial, key, "suppressed", "risk_latched");
            return;
        }
        if ((action.kind == ActionKind::TargetLong || action.kind == ActionKind::FixedEntry) &&
            a.units.raw()) {
            return;
        }
        if (action.kind == ActionKind::TargetCash && !a.units.raw()) {
            return;
        }
        text(action.reason, 128);
        if (action.reason.find_first_of("\n\r\t") != std::string::npos) {
            fail(ErrorCode::InvalidConfig, "invalid action reason");
        }
        std::variant<BuyCashBudget, BuyFixedQuantity, SellAll> order = SellAll{};
        if (action.initial_stop_distance && action.kind != ActionKind::FixedEntry) {
            fail(ErrorCode::InvalidConfig, "protective distance requires a fixed entry");
        }
        if (action.kind == ActionKind::TargetLong) {
            order = take(freeze_cash_budget(a));
        } else if (action.kind == ActionKind::FixedEntry) {
            order = BuyFixedQuantity{action.quantity, action.initial_stop_distance};
        } else if (action.kind != ActionKind::TargetCash) {
            fail(ErrorCode::InvalidConfig, "invalid action kind");
        }
        const auto id = t.next_id;
        t.next_id = detail::next(id);
        t.pending =
            Intent{a.trial, id, a.source, key, detail::next(key), action.reason, std::move(order)};
        record(trial, key, "submitted", action.reason, nullptr, nullptr, id);
    }
};
lx::RequiredEvidenceSpec financial_evidence_spec(std::string path, const HostLimits &limits) {
    return {
        "financial_ledger",        "quadeplus_financial_ledger_v1", std::move(path), "create_new",
        limits.max_evidence_bytes, limits.max_trace_records,        "on_close",      "stop"};
}
BacktestResult<HostedSimulation> HostedSimulation::create(
    research::DailyDataset data, std::shared_ptr<const lx::PreparedPlan> plan,
    const lx::HostBuild &build, SuiteConfiguration config, std::vector<DecisionCallback> decisions,
    const lx::RegisteredType<data::Price> &price, std::vector<ExecutionCallback> executions,
    std::optional<lx::RegisteredType<strategies::StochasticValue>> ratio) noexcept {
    return detail::guard([&]() -> BacktestResult<HostedSimulation> {
        const auto &l = config.limits;
        if (executions.empty()) {
            executions.resize(config.trials.size());
        }
        if (executions.size() != config.trials.size()) {
            fail(ErrorCode::InvalidConfig, "execution callbacks must match independent trials");
        }
        if (!plan || !plan->admitted() || plan->configuration().run.driver != lx::Driver::Hosted ||
            !plan->rejections().empty() || !l.max_trials || l.max_trials > 128 ||
            config.trials.empty() || config.trials.size() > l.max_trials ||
            decisions.size() != config.trials.size() || !l.max_ledger_events ||
            l.max_ledger_events > 100000 || !l.max_trace_records || l.max_trace_records > 1000000 ||
            !l.max_evidence_bytes || l.max_evidence_bytes > 67108864) {
            fail(ErrorCode::InvalidConfig,
                 "host requires a fully admitted hosted plan and explicit bounds");
        }
        const auto &source = plan->source().metadata();
        if (config.funding_key == UINT64_MAX || config.funding_key + 1 != config.evaluation_begin ||
            config.evaluation_begin >= config.evaluation_end ||
            config.evaluation_end != data.rows().size() || source.validated_scope.begin != 0 ||
            source.validated_scope.end != data.rows().size() ||
            source.validated_scope.clock != research::clock_profile ||
            source.version != data.manifest_sha256() ||
            source.provider_implementation != build.identity() ||
            price.policy.contract != quant::lexongine::price_contract()) {
            fail(ErrorCode::SourceBinding,
                 "host source/build/type or funding/evaluation scope mismatch");
        }
        if (data.rows()[config.evaluation_begin].session < data.specification().evaluation_begin ||
            data.rows()[config.evaluation_begin].session > data.specification().evaluation_end) {
            fail(ErrorCode::InvalidConfig,
                 "financial evaluation is outside the dataset's permitted range");
        }
        if (ratio &&
            (ratio->policy.contract != strategies::stochastic_type_contract() ||
             ratio->witness.type != std::type_index(typeid(strategies::StochasticValue)))) {
            fail(ErrorCode::InvalidConfig, "invalid registered ratio observation type");
        }
        const auto supported = [&](const lx::TypeContract &type) {
            return type == price.policy.contract || (ratio && type == ratio->policy.contract);
        };
        if (config.financial_evidence !=
                financial_evidence_spec(config.financial_evidence.absolute_path, l) ||
            std::count(plan->required_evidence().begin(), plan->required_evidence().end(),
                       config.financial_evidence) != 1) {
            fail(ErrorCode::InvalidConfig, "financial evidence must be frozen in preparation");
        }
        for (std::size_t i = 0; i < config.trials.size(); ++i) {
            const auto &t = config.trials[i];
            text(t.strategy_id, 128);
            text(t.strategy_configuration, 4096);
            if (!decisions[i] || t.observations.size() > 128 ||
                (t.terminal != TerminalMode::Mark && t.terminal != TerminalMode::SyntheticClose)) {
                fail(ErrorCode::InvalidConfig, "invalid trial decision/evaluation configuration");
            }
            detail::policy(t.execution);
            take(validate_risk_policy(t.risk));
            if (t.initial_cash.amount() <= 0 ||
                t.initial_cash.currency().value() != data.specification().currency ||
                !t.lot_step.raw()) {
                fail(ErrorCode::InvalidConfig, "invalid trial funding/lot policy");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (t.id == config.trials[j].id) {
                    fail(ErrorCode::InvalidConfig, "duplicate trial ID");
                }
            }
            for (std::size_t j = 0; j < t.observations.size(); ++j) {
                const auto &wanted = t.observations[j];
                text(wanted.id, 128);
                for (std::size_t k = 0; k < j; ++k) {
                    if (wanted.id == t.observations[k].id) {
                        fail(ErrorCode::InvalidConfig, "duplicate required trial observation");
                    }
                }
                const auto found =
                    std::find_if(plan->observations().begin(), plan->observations().end(),
                                 [&](const auto &o) { return o.request.id == wanted.id; });
                if (wanted.kind != FeatureKind::Price && wanted.kind != FeatureKind::Ratio) {
                    fail(ErrorCode::InvalidConfig, "unsupported required observation kind");
                }
                if (wanted.kind == FeatureKind::Ratio && !ratio) {
                    fail(ErrorCode::InvalidConfig, "required ratio type was not registered");
                }
                const auto &expected = wanted.kind == FeatureKind::Price ? price.policy.contract
                                                                         : ratio->policy.contract;
                if (found == plan->observations().end() ||
                    found->request.evaluation !=
                        lx::SequenceScope{source.validated_scope.clock, wanted.begin, wanted.end} ||
                    plan->outputs(found->producer)[found->port].semantic != expected) {
                    fail(ErrorCode::InvalidConfig,
                         "required trial observation was rejected or its scope/type changed");
                }
            }
        }
        for (const auto &o : plan->observations()) {
            if (!supported(plan->outputs(o.producer)[o.port].semantic)) {
                fail(ErrorCode::Unsupported, "unsupported typed financial observation");
            }
        }
        auto charge = engine(data.account()->reserve(
            lx::ResourceKind::Graph,
            65536 + config.trials.size() * (l.max_ledger_events * 4096 + 65536) +
                l.max_trace_records * 4096 + l.max_evidence_bytes * 3));
        auto financial = std::make_shared<FinancialRun>();
        financial->charge = std::move(charge);
        financial->trials.resize(config.trials.size());
        financial->events.reserve(static_cast<std::size_t>(l.max_trace_records));
        financial->effective_configuration = configuration(config, data, *plan, build);
        financial->experiment = engine(lx::files::digest(financial->effective_configuration));
        auto receipt_charge = engine(data.account()->reserve(
            lx::ResourceKind::Graph, 8192 + plan->observations().size() * 2048));
        auto inbox = std::make_shared<FeatureInbox>();
        inbox->charge = std::move(receipt_charge);
        inbox->maximum = plan->observations().size();
        inbox->values.reserve(inbox->maximum);
        inbox->ratios.reserve(inbox->maximum);
        auto state = std::make_shared<State>(
            State{std::move(data), std::move(plan), std::move(config), std::move(decisions), price,
                  std::move(financial), std::move(inbox), std::move(executions), std::move(ratio)});
        return HostedSimulation{std::move(state)};
    });
}
std::shared_ptr<const FinancialRun> HostedSimulation::financial() const noexcept {
    return state_->financial;
}
lx::IntegrationCompletion HostedSimulation::completion() const {
    const auto &f = *state_->financial;
    const bool success = f.finalized && f.exhausted && !f.failure;
    return {success ? lx::IntegrationOutcome::Completed : lx::IntegrationOutcome::Failed,
            success
                ? std::optional<lx::Diagnostic>{}
                : lx::diagnostic(lx::FailureCode::CallbackException,
                                 f.failure ? f.failure->text() : "financial completion missing"),
            "quadeplus.financial_host", 1, f.experiment};
}
BacktestResult<lx::RunServices>
HostedSimulation::services(lx::ExecutionEvidence retention,
                           std::vector<lx::RequiredEvidenceSink> extra) noexcept {
    return detail::guard([&]() -> BacktestResult<lx::RunServices> {
        auto s = state_;
        s->healthy();
        if (s->bound) {
            fail(ErrorCode::InvalidConfig, "host services already bound");
        }
        std::vector<lx::ObservationAdapter> observers;
        observers.reserve(s->plan->observations().size());
        for (const auto &o : s->plan->observations()) {
            if (s->plan->outputs(o.producer)[o.port].semantic == s->price.policy.contract) {
                observers.push_back(lx::make_observer<data::Price>(
                    o.request.id, s->price,
                    [inbox = s->inbox](const auto &value,
                                       const lx::RequestContext &context) -> lx::Result<void> {
                        return lx::guarded([&]() -> lx::Result<void> {
                            if (!inbox->collecting || context.key.sequence != inbox->expected_key ||
                                inbox->values.size() + inbox->ratios.size() == inbox->maximum) {
                                return lx::diagnostic(lx::FailureCode::UpdateProtocol,
                                                      "unexpected financial observation receipt");
                            }
                            inbox->values.push_back({context.request, value.value,
                                                     context.key.sequence,
                                                     value.publication().available_at_ns});
                            return lx::ok();
                        });
                    }));
            } else {
                observers.push_back(lx::make_observer<strategies::StochasticValue>(
                    o.request.id, *s->ratio_type,
                    [inbox = s->inbox](const auto &value,
                                       const lx::RequestContext &context) -> lx::Result<void> {
                        return lx::guarded([&]() -> lx::Result<void> {
                            if (!inbox->collecting || context.key.sequence != inbox->expected_key ||
                                inbox->values.size() + inbox->ratios.size() == inbox->maximum ||
                                !strategies::valid_stochastic(value.value)) {
                                return lx::diagnostic(lx::FailureCode::UpdateProtocol,
                                                      "invalid ratio receipt");
                            }
                            inbox->ratios.push_back({context.request, value.value,
                                                     context.key.sequence,
                                                     value.publication().available_at_ns});
                            return lx::ok();
                        });
                    }));
            }
        }
        struct Sink {
            std::shared_ptr<State> state;
            int fd = -1;
            ~Sink() {
                if (fd >= 0) {
                    static_cast<void>(::close(fd));
                }
            }
        };
        auto sink = std::make_shared<Sink>();
        sink->state = s;
        lx::RequiredEvidenceSink financial{
            {},
            s->config.financial_evidence,
            [sink]() -> lx::Result<void> {
                sink->fd = ::open(sink->state->config.financial_evidence.absolute_path.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (sink->fd < 0) {
                    return lx::diagnostic(lx::FailureCode::EvidenceIO,
                                          "financial ledger create-new failed");
                }
                return lx::ok();
            },
            {},
            {},
            [sink](const lx::RunReport &report) -> lx::Result<lx::EvidenceReference> {
                return lx::guarded([&]() -> lx::Result<lx::EvidenceReference> {
                    const auto &state = *sink->state;
                    const auto &f = *state.financial;
                    const auto &spec = state.config.financial_evidence;
                    if (report.integration == lx::IntegrationOutcome::Completed &&
                        (!f.finalized || !f.exhausted || f.failure)) {
                        return lx::diagnostic(lx::FailureCode::EvidenceIO,
                                              "financial evidence is incomplete");
                    }
                    std::string output =
                        "quadeplus_financial_ledger_v1\nexperiment=" + hex(f.experiment) +
                        "\nfinancial_state=" +
                        (f.finalized ? std::string{"completed"} : std::string{"partial"}) +
                        "\nconfiguration_bytes=" +
                        std::to_string(f.effective_configuration.size()) + ":" +
                        f.effective_configuration + "\n";
                    if (output.size() > spec.max_bytes) {
                        return lx::diagnostic(lx::FailureCode::EvidenceQuota,
                                              "financial configuration exceeds evidence bound");
                    }
                    for (const auto &e : f.events) {
                        std::ostringstream row;
                        row << e.trial << ' ' << e.key << ' ' << e.id << ' ' << std::quoted(e.kind)
                            << ' ' << std::quoted(e.reason) << ' ' << e.cash << ' ' << e.units
                            << ' ' << e.basis << ' ' << e.realized << ' ' << e.dividends << ' '
                            << e.fees << ' ' << (e.equity ? std::to_string(*e.equity) : "none")
                            << ' '
                            << (e.phase ? std::to_string(static_cast<int>(*e.phase)) : "none")
                            << ' ' << e.price << ' ' << e.quantity << ' ' << e.gross << ' ' << e.fee
                            << ' ' << (e.decision_key ? std::to_string(*e.decision_key) : "none")
                            << ' ' << (e.eligible_key ? std::to_string(*e.eligible_key) : "none")
                            << ' ' << e.quantity_policy << ' ' << e.budget << '\n';
                        auto text = row.str();
                        if (text.size() > spec.max_bytes - output.size()) {
                            return lx::diagnostic(lx::FailureCode::EvidenceQuota,
                                                  "financial ledger byte ceiling exceeded");
                        }
                        output += text;
                    }
                    return lx::files::finish_open(sink->fd, spec.name, spec.absolute_path, output,
                                                  state.dataset.account(), spec.max_bytes,
                                                  lx::FailureCode::EvidenceIO);
                });
            }};
        std::vector<lx::RequiredEvidenceSink> required;
        required.reserve(s->plan->required_evidence().size());
        for (const auto &spec : s->plan->required_evidence()) {
            if (spec == financial.specification) {
                required.push_back(financial);
            } else {
                auto found = std::find_if(extra.begin(), extra.end(), [&](const auto &value) {
                    return value.specification == spec;
                });
                if (found == extra.end()) {
                    fail(ErrorCode::InvalidConfig, "required numerical evidence adapter missing");
                }
                required.push_back(std::move(*found));
                extra.erase(found);
            }
        }
        if (!extra.empty()) {
            fail(ErrorCode::InvalidConfig, "undeclared extra evidence adapter");
        }
        s->bound = true;
        return lx::RunServices{std::move(observers),
                               std::move(retention),
                               {lx::IntegrationOutcome::Completed,
                                {},
                                "quadeplus.financial_host",
                                1,
                                s->financial->experiment},
                               std::move(required)};
    });
}
BacktestResult<void> HostedSimulation::before_open(std::uint64_t key) noexcept {
    auto result = detail::guard([&]() -> BacktestResult<void> {
        auto &s = *state_;
        s.healthy();
        if (!s.bound || s.opening || key != s.next_key || key >= s.config.evaluation_end) {
            fail(ErrorCode::InvalidData, "out-of-order financial opening");
        }
        s.inbox->values.clear();
        s.inbox->ratios.clear();
        s.inbox->expected_key = key;
        s.inbox->collecting = true;
        s.opening = true;
        if (key < s.config.evaluation_begin) {
            return ok();
        }
        auto opening =
            take(s.dataset.opening(key, s.dataset.rows()[key].bar.interval_start.unix_nanos()));
        auto previous =
            take(s.dataset.closing(key - 1, s.dataset.rows()[key - 1].close_available_ns));
        auto prior = take(prior_close_valuation(previous, opening));
        for (std::size_t i = 0; i < s.financial->trials.size(); ++i) {
            auto &t = s.financial->trials[i];
            if (!t.account || !t.ledger) {
                fail(ErrorCode::InvalidData, "evaluation began before funding");
            }
            auto &a = *t.account;
            take(apply_dividend(a, *t.ledger,
                                {a.trial, prior, s.dataset.rows()[key].dividend, a.units}));
            take(verify_financial_invariants(
                a, *t.ledger, take(mark_account(a, prior, ValuationPhase::PriorClose))));
            s.record(i, key, "dividend", "ex_date_cash", &prior);
            if (t.pending) {
                const auto id = t.pending->id;
                auto quote = take(quote_fill(*t.pending, opening, a, s.config.trials[i].execution));
                if (auto *fill = std::get_if<Fill>(&quote)) {
                    s.record(i, key, "accepted", fill->reason, &fill->mark, fill);
                    take(apply_fill(a, *t.ledger, *fill));
                    take(verify_financial_invariants(
                        a, *t.ledger, take(mark_account(a, fill->mark, ValuationPhase::Open))));
                    s.record(i, key, "filled", fill->reason, &fill->mark, fill);
                } else {
                    s.record(
                        i, key, "rejected",
                        std::to_string(static_cast<int>(std::get<OrderRejection>(quote).reason)),
                        nullptr, nullptr, id);
                }
                if (s.executions[i]) {
                    take(s.executions[i](quote, a));
                }
                t.pending.reset();
            }
        }
        return ok();
    });
    return sticky(std::move(result), *state_->financial);
}
BacktestResult<void> HostedSimulation::after_step(const lx::StepReceipt &receipt) noexcept {
    auto result = detail::guard([&]() -> BacktestResult<void> {
        auto &s = *state_;
        s.healthy();
        if (receipt.status == lx::StepStatus::Exhausted) {
            if (s.financial->exhausted || s.opening || s.next_key != s.config.evaluation_end ||
                !receipt.completed_scope ||
                *receipt.completed_scope != s.plan->source().metadata().validated_scope) {
                fail(ErrorCode::Coverage, "source exhaustion did not close exact financial scope");
            }
            s.financial->exhausted = true;
            return ok();
        }
        if (receipt.status != lx::StepStatus::Processed || !s.opening || !receipt.key ||
            receipt.key->sequence != s.next_key || receipt.key->clock != research::clock_profile ||
            receipt.available_at_ns != s.dataset.rows()[s.next_key].close_available_ns) {
            fail(ErrorCode::SourceBinding,
                 "financial close requires a successful matching graph step");
        }
        s.inbox->collecting = false;
        const auto key = s.next_key;
        auto close = take(s.dataset.closing(key, *receipt.available_at_ns));
        auto mark = take(closing_valuation(close));
        for (const auto &feature : s.inbox->values) {
            if (feature.key != key || feature.available_at_ns > *receipt.available_at_ns) {
                fail(ErrorCode::FutureRead, "future or stale feature receipt");
            }
        }
        for (const auto &feature : s.inbox->ratios) {
            if (feature.key != key || feature.available_at_ns > *receipt.available_at_ns) {
                fail(ErrorCode::FutureRead, "future or stale ratio receipt");
            }
        }
        for (std::size_t i = 0; i < s.config.trials.size(); ++i) {
            const auto &config = s.config.trials[i];
            auto &t = s.financial->trials[i];
            if (key == s.config.funding_key) {
                t.account = take(
                    fund_account(config.id, close.source(), config.initial_cash, config.lot_step));
                t.ledger = take(Ledger::create(
                    *t.account, static_cast<std::size_t>(s.config.limits.max_ledger_events)));
                t.risk = take(initialize_risk(config.initial_cash));
                s.record(i, key, "funded", "initial_cash", &mark);
            }
            std::vector<PriceFeature> selected;
            std::vector<RatioFeature> ratios;
            selected.reserve(config.observations.size());
            ratios.reserve(config.observations.size());
            for (const auto &required : config.observations) {
                if (key >= required.begin && key < required.end) {
                    auto found =
                        std::find_if(s.inbox->values.begin(), s.inbox->values.end(),
                                     [&](const auto &f) { return f.request == required.id; });
                    auto ratio =
                        std::find_if(s.inbox->ratios.begin(), s.inbox->ratios.end(),
                                     [&](const auto &f) { return f.request == required.id; });
                    if (found != s.inbox->values.end()) {
                        selected.push_back(*found);
                    } else if (ratio != s.inbox->ratios.end()) {
                        ratios.push_back(*ratio);
                    } else {
                        fail(ErrorCode::SourceBinding,
                             "required current typed feature was not delivered");
                    }
                }
            }
            RiskDecision risk{false, false, false, RiskReason::None};
            if (t.account) {
                auto equity = take(mark_account(*t.account, mark, ValuationPhase::Close));
                take(verify_financial_invariants(*t.account, *t.ledger, equity));
                if (key >= s.config.evaluation_begin) {
                    risk = take(evaluate_risk(*t.risk, *t.account, equity, config.risk));
                }
                s.record(i, key, "close", "ordinary", &mark);
                if (risk.new_trigger) {
                    s.record(i, key, "risk", std::to_string(static_cast<int>(risk.reason)), &mark);
                }
            }
            const bool may_submit = key >= s.config.evaluation_begin ||
                                    (s.config.legacy_funding_close && key == s.config.funding_key);
            auto action = take(s.decisions[i]({key, may_submit, t.account.has_value(), ratios},
                                              t.account ? &*t.account : nullptr, selected));
            if (!may_submit && action.kind != ActionKind::Hold) {
                fail(ErrorCode::InvalidData,
                     "warmup/funding callback attempted an unauthorized order");
            }
            if (may_submit) {
                s.queue(i, key, std::move(action), risk);
            }
        }
        s.opening = false;
        ++s.next_key;
        return ok();
    });
    return sticky(std::move(result), *state_->financial);
}
BacktestResult<void> HostedSimulation::finalize_financial() noexcept {
    auto result = detail::guard([&]() -> BacktestResult<void> {
        auto &s = *state_;
        s.healthy();
        if (!s.financial->exhausted || s.opening) {
            fail(ErrorCode::Coverage, "final Bar alone is not numerical exhaustion");
        }
        const auto key = s.config.evaluation_end - 1;
        auto close = take(s.dataset.closing(key, s.dataset.rows()[key].close_available_ns));
        auto mark = take(closing_valuation(close));
        for (std::size_t i = 0; i < s.config.trials.size(); ++i) {
            auto &t = s.financial->trials[i];
            if (t.pending) {
                s.record(i, key, "expired", "end_of_scope", nullptr, nullptr, t.pending->id);
                t.pending.reset();
            }
            if (s.config.trials[i].terminal == TerminalMode::SyntheticClose &&
                t.account->units.raw()) {
                const auto id = t.next_id;
                t.next_id = detail::next(id);
                auto quote =
                    take(quote_terminal_liquidation(close, *t.account, s.config.trials[i].execution,
                                                    {TerminalMode::SyntheticClose, id}));
                const auto *fill = std::get_if<Fill>(&quote);
                if (!fill) {
                    fail(ErrorCode::InvalidData, "terminal liquidation rejected");
                }
                take(apply_fill(*t.account, *t.ledger, *fill));
                s.record(i, key, "terminal", fill->reason, &mark, fill);
                if (s.executions[i]) {
                    take(s.executions[i](quote, *t.account));
                }
            }
            take(verify_financial_invariants(
                *t.account, *t.ledger,
                take(mark_account(*t.account, mark, ValuationPhase::Close))));
            s.record(i, key, "final", "endpoint", &mark);
        }
        s.financial->finalized = true;
        return ok();
    });
    return sticky(std::move(result), *state_->financial);
}
BacktestResult<HostReport>
HostedSimulation::run(lx::ExecutionEvidence retention,
                      std::vector<lx::RequiredEvidenceSink> extra) noexcept {
    return detail::guard([&]() -> BacktestResult<HostReport> {
        auto &s = *state_;
        if (s.started || s.bound) {
            fail(ErrorCode::InvalidConfig, "host can run only once");
        }
        s.started = true;
        auto run =
            engine(lx::instantiate(s.plan, take(services(std::move(retention), std::move(extra)))));
        auto work = detail::guard([&]() -> BacktestResult<void> {
            while (!s.financial->exhausted) {
                if (s.next_key < s.config.evaluation_end) {
                    take(before_open(s.next_key));
                }
                auto receipt = engine(lx::advance_one(run));
                take(after_step(receipt));
            }
            take(finalize_financial());
            return ok();
        });
        if (const auto *failure = std::get_if<BacktestError>(&work)) {
            if (!s.financial->failure) {
                s.financial->failure = *failure;
            }
            engine(lx::fail_host(
                run, lx::diagnostic(lx::FailureCode::CallbackException, failure->text())));
        }
        auto report = engine(lx::finish(run, completion()));
        return HostReport{s.financial, std::move(report)};
    });
}
} // namespace quant::backtest
