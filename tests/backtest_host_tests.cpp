#include "backtest_host_support.hpp"

namespace {
using namespace backtest_host_test;
void causal_trials() {
    std::optional<std::pair<std::int64_t, std::int64_t>> first;
    for (bool changed : {false, true}) {
        Environment e(changed);
        auto config = e.suite();
        auto plan = e.prepare(config);
        auto host = take(qb::HostedSimulation::create(e.data, plan, e.build, config, callbacks(),
                                                      e.types.price));
        auto report = take(host.run(e.retention(plan), e.extra(plan)));
        require(report.engine.acceptance == lx::AcceptanceStatus::Accepted &&
                    report.financial->exhausted && report.financial->finalized,
                "financial host not accepted");
        std::size_t fills = 0, submitted = 0;
        for (const auto &event : report.financial->events) {
            if (event.kind == "filled") {
                ++fills;
                require(event.key == 2 && event.trial == 1, "signal did not fill next open");
                auto result = std::pair{event.price, event.quantity};
                if (first) {
                    require(*first == result,
                            "current HLC changed an already eligible opening fill");
                } else {
                    first = result;
                }
            }
            if (event.kind == "submitted") {
                ++submitted;
            }
        }
        require(fills == 1 && submitted == 1, "persistent target resized or duplicated entry");
        require(report.financial->trials[1].account->cash.amount() == 2000000 &&
                    report.financial->trials[1].account->units.raw() == 0,
                "trial accounts shared state");
        require(report.financial->trials[0].account->dividends.amount() == 0,
                "ex-date new buyer received dividend");
        std::cout << "HOST_EVIDENCE=" << e.path << " events=" << report.financial->events.size()
                  << " accepted=1\n";
        if (!changed) {
            for (const auto &event : report.financial->events) {
                if (event.trial == 1) {
                    std::cout << "TRACE " << event.key << ' ' << event.kind
                              << " cash=" << event.cash << " units=" << event.units << " equity="
                              << (event.equity ? std::to_string(*event.equity) : "none") << '\n';
                }
            }
        }
    }
}
void risk_boundaries() {
    Environment e;
    auto opening = take(e.data.opening(0, e.data.rows()[0].bar.interval_start.unix_nanos()));
    auto a = take(qb::fund_account(1, opening.source(), money(10000), qd::Quantity::from_raw(1)));
    auto risk = take(qb::initialize_risk(a.initial_cash));
    for (const auto [key, cash] :
         std::vector<std::pair<std::uint64_t, std::int64_t>>{{0, 10000}, {1, 12000}, {2, 9600}}) {
        a.cash = money(cash);
        a.realized = money(cash - 10000);
        auto mark = take(qb::closing_valuation(
            take(e.data.closing(key, e.data.rows()[key].close_available_ns))));
        auto decision = take(qb::evaluate_risk(
            risk, a, take(qb::mark_account(a, mark, qb::ValuationPhase::Close)), {{2000}, {}}));
        require(decision.block_entries == (key == 2),
                "drawdown must use high water, not initial cash");
    }
    require(risk.high_water.amount() == 12000 && risk.latched, "20-percent equality not latched");
    auto daily = take(qb::initialize_risk(money(10000)));
    a.cash = money(9500);
    a.realized = money(-500);
    auto close =
        take(qb::closing_valuation(take(e.data.closing(3, e.data.rows()[3].close_available_ns))));
    require(take(qb::evaluate_risk(daily, a,
                                   take(qb::mark_account(a, close, qb::ValuationPhase::Close)),
                                   {{}, {500}}))
                .new_trigger,
            "5-percent daily equality not triggered");
}
void admission_and_exhaustion() {
    Environment e;
    auto config = e.suite();
    auto partial = e.prepare(config, true);
    require(!partial->rejections().empty() && !partial->observations().empty(),
            "fixture is not partial admission");
    rejects(
        qb::HostedSimulation::create(e.data, partial, e.build, config, callbacks(), e.types.price));
    require(!std::filesystem::exists(e.path / "ledger.txt"),
            "rejected suite opened/funded evidence");
    Environment complete;
    auto suite = complete.suite();
    auto p = complete.prepare(suite);
    auto wrong_type = suite;
    wrong_type.trials[0].observations[0].kind = qb::FeatureKind::Ratio;
    rejects(qb::HostedSimulation::create(complete.data, p, complete.build, wrong_type, callbacks(),
                                         complete.types.price));
    require(!std::filesystem::exists(complete.path / "ledger.txt"),
            "wrong required type funded/opened evidence");
    auto host = take(qb::HostedSimulation::create(complete.data, p, complete.build, suite,
                                                  callbacks(), complete.types.price));
    auto run =
        take(lx::instantiate(p, take(host.services(complete.retention(p), complete.extra(p)))));
    for (std::uint64_t key = 0; key < 5; ++key) {
        take(host.before_open(key));
        auto step = take(lx::advance_one(run));
        take(host.after_step(step));
    }
    auto claimed = host.completion();
    claimed.kind = lx::IntegrationOutcome::Completed;
    claimed.cause.reset();
    rejects(lx::finish(run, claimed));
    rejects(host.finalize_financial());
    take(lx::fail_host(run,
                       lx::diagnostic(lx::FailureCode::CallbackException, "missing exhaustion")));
    auto report = take(lx::finish(run, host.completion()));
    require(report.acceptance == lx::AcceptanceStatus::Rejected, "last Bar accepted as exhausted");
}
void failure_and_identity() {
    Environment e;
    auto config = e.suite();
    auto p = e.prepare(config);
    auto normal =
        take(qb::HostedSimulation::create(e.data, p, e.build, config, callbacks(), e.types.price));
    auto changed = config;
    changed.trials[0].initial_cash = money(2000000);
    auto other =
        take(qb::HostedSimulation::create(e.data, p, e.build, changed, callbacks(), e.types.price));
    require(normal.financial()->experiment != other.financial()->experiment,
            "financial config absent from identity");
    config.trials[1].strategy_configuration = "injected_failure_at_key2";
    auto failures = callbacks();
    failures[1] = [](const qb::DecisionContext &c, const auto *,
                     auto) -> qb::BacktestResult<qb::Action> {
        if (c.key == 2) {
            return qb::error(qb::ErrorCode::Integrity, "injected trial failure");
        }
        return hold();
    };
    auto failing = take(qb::HostedSimulation::create(e.data, p, e.build, config,
                                                     std::move(failures), e.types.price));
    auto failed = take(failing.run(e.retention(p), e.extra(p)));
    require(failed.engine.acceptance == lx::AcceptanceStatus::Rejected &&
                failed.financial->failure.has_value(),
            "host callback failure accepted");
    Environment limited;
    auto small = limited.suite(64);
    auto small_plan = limited.prepare(small);
    auto host = take(qb::HostedSimulation::create(limited.data, small_plan, limited.build, small,
                                                  callbacks(), limited.types.price));
    auto report = take(host.run(limited.retention(small_plan), limited.extra(small_plan)));
    require(report.engine.acceptance == lx::AcceptanceStatus::Rejected &&
                report.engine.numerical == lx::NumericalOutcome::Completed,
            "required ledger failure accepted");
}
void risk_execution_and_terminal() {
    Environment e;
    auto config = e.suite();
    config.trials[0].risk.daily_loss_bps = 500;
    auto p = e.prepare(config);
    auto host =
        take(qb::HostedSimulation::create(e.data, p, e.build, config, callbacks(), e.types.price));
    auto report = take(host.run(e.retention(p), e.extra(p)));
    require(report.engine.acceptance == lx::AcceptanceStatus::Accepted &&
                report.financial->trials[0].risk->latched &&
                report.financial->trials[0].account->units.raw() == 0,
            "risk did not liquidate/latch");
    bool exit = false, suppressed = false;
    for (const auto &event : report.financial->events) {
        exit |= event.kind == "filled" && event.reason == "risk_exit" && event.key == 4;
        suppressed |= event.kind == "suppressed" && event.key == 4;
    }
    require(exit && suppressed, "risk exit lost precedence or permitted reentry");
    Environment terminal;
    auto suite = terminal.suite();
    suite.trials[0].strategy_configuration = "fixed100_at_key3";
    suite.trials[0].terminal = qb::TerminalMode::SyntheticClose;
    suite.trials[0].execution = {5, 0};
    suite.trials[0].risk.drawdown_bps = 6;
    auto endplan = terminal.prepare(suite);
    auto cb = callbacks();
    cb[0] = [](const qb::DecisionContext &c, const auto *, auto) -> qb::BacktestResult<qb::Action> {
        if (c.key == 3) {
            return qb::Action{qb::ActionKind::FixedEntry, qd::Quantity::from_integer(100),
                              "final_morning"};
        }
        return hold();
    };
    auto ending = take(qb::HostedSimulation::create(terminal.data, endplan, terminal.build, suite,
                                                    std::move(cb), terminal.types.price));
    auto ended = take(ending.run(terminal.retention(endplan), terminal.extra(endplan)));
    require(ended.engine.acceptance == lx::AcceptanceStatus::Accepted &&
                ended.financial->trials[0].account->cash.amount() == 999200 &&
                !ended.financial->trials[0].risk->latched,
            "terminal fee created retroactive risk or wrong cash");
    Environment rejection;
    auto rejected_suite = rejection.suite();
    rejected_suite.trials[0].strategy_configuration = "fixed1000_at_key1";
    auto rp = rejection.prepare(rejected_suite);
    auto rcb = callbacks();
    rcb[0] = [](const qb::DecisionContext &c, const auto *,
                auto) -> qb::BacktestResult<qb::Action> {
        if (c.key == 1) {
            return qb::Action{qb::ActionKind::FixedEntry, qd::Quantity::from_integer(1000),
                              "unaffordable"};
        }
        return hold();
    };
    auto rh = take(qb::HostedSimulation::create(rejection.data, rp, rejection.build, rejected_suite,
                                                std::move(rcb), rejection.types.price));
    auto rr = take(rh.run(rejection.retention(rp), rejection.extra(rp)));
    require(rr.engine.acceptance == lx::AcceptanceStatus::Accepted &&
                rr.financial->trials[0].account->units.raw() == 0 &&
                rr.financial->trials[0].account->cash.amount() == 1000000,
            "rejected entry created holdings/cash change");
}
void partial_numerical_step() {
    Environment e;
    auto config = e.suite();
    auto p = e.prepare(config);
    auto cb = callbacks();
    std::uint64_t decisions = 0;
    auto delegate = cb[0];
    cb[0] = [&](const auto &c, const auto *a, auto f) {
        ++decisions;
        return delegate(c, a, f);
    };
    auto host = take(
        qb::HostedSimulation::create(e.data, p, e.build, config, std::move(cb), e.types.price));
    auto services = take(host.services(e.retention(p), e.extra(p)));
    auto deliver = services.observers[1].deliver;
    services.observers[1].deliver = [deliver](const auto &request, const auto &slot, auto port,
                                              const auto &context) -> lx::Result<void> {
        auto delivered = deliver(request, slot, port, context);
        if (context.key.sequence == 2) {
            return lx::diagnostic(lx::FailureCode::CallbackException, "late observer failure");
        }
        return delivered;
    };
    auto run = take(lx::instantiate(p, std::move(services)));
    for (std::uint64_t key = 0; key < 2; ++key) {
        take(host.before_open(key));
        take(host.after_step(take(lx::advance_one(run))));
    }
    take(host.before_open(2));
    auto stopped = take(lx::advance_one(run));
    require(stopped.status == lx::StepStatus::Stopped, "observer did not stop numerical step");
    rejects(host.after_step(stopped));
    require(decisions == 2, "partial numerical step invoked financial decision");
    for (const auto &event : host.financial()->events) {
        require(!(event.key == 2 && event.kind == "close"),
                "failed graph step marked current close");
    }
    take(lx::fail_host(run, lx::diagnostic(lx::FailureCode::CallbackException, "partial step")));
    require(take(lx::finish(run, host.completion())).acceptance == lx::AcceptanceStatus::Rejected,
            "partial graph accepted");
}
void native_acceptance_gate() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        const bool fail_ledger = mode == 1;
        Environment e;
        auto suite = e.suite(fail_ledger ? 64 : 1048576);
        auto decisions = callbacks();
        std::vector<qb::ExecutionCallback> executions;
        if (mode == 3) {
            suite.trials[0].strategy_configuration = "execution_notification_failure";
            executions.resize(2);
            executions[0] = [](const qb::FillQuote &,
                               const qb::Account &) -> qb::BacktestResult<void> {
                return qb::error(qb::ErrorCode::Integrity, "post-fill notification failed");
            };
        }
        if (mode == 2) {
            suite.trials[1].strategy_configuration = "fail_key2";
            decisions[1] = [](const qb::DecisionContext &c, const auto *,
                              auto) -> qb::BacktestResult<qb::Action> {
                if (c.key == 2) {
                    return qb::error(qb::ErrorCode::Integrity, "native hosted failure");
                }
                return hold();
            };
        }
        lx::LocalCacheConfiguration cache{"automatic_read_write",
                                          "local_files",
                                          (e.path / "cache").string(),
                                          "exact_full_scope",
                                          "same_build_local_writer",
                                          "sha256",
                                          "single_process_lock",
                                          "fail_preparation",
                                          "bypass_new_entries",
                                          "explicit_only",
                                          64,
                                          8388608,
                                          1048576,
                                          1048576};
        lx::FileStoreLimits limits{{{1048576, 1024, 32768, 4194304}, 32768, 4096, 4096, 4096, 4096},
                                   {{1048576, 65536, 64, 4096, 8388608}, 4194304},
                                   {1048576, 65536, 1024, 4096, 4096, 1048576, 16777216},
                                   1048576,
                                   512,
                                   4194304,
                                   metadata_limits()};
        auto store =
            take(lx::FileResultStore::create(cache, e.types.price, e.build, account(), limits));
        auto p = e.prepare(suite, false, store.service());
        auto host = take(qb::HostedSimulation::create(
            e.data, p, e.build, suite, std::move(decisions), e.types.price, std::move(executions)));
        auto report = take(host.run(e.retention(p), e.extra(p)));
        std::size_t markers = 0;
        for (const auto &file : std::filesystem::directory_iterator(e.path / "cache")) {
            if (file.path().filename().string().ends_with(".accepted.xml")) {
                ++markers;
            }
        }
        if (mode) {
            require(report.engine.acceptance == lx::AcceptanceStatus::Rejected && markers == 0 &&
                        report.engine.cache == lx::CachePublicationStatus::NotPublished,
                    "failed financial host/evidence published a cache origin");
        } else {
            require(report.engine.acceptance == lx::AcceptanceStatus::Accepted && markers == 1 &&
                        report.engine.cache == lx::CachePublicationStatus::Published,
                    "native financial marker missing");
            require(report.engine.evidence.size() == 3,
                    "financial evidence missing from accepted inventory");
            for (const auto &evidence : report.engine.evidence) {
                take(lx::files::verify(evidence, e.data.account(), 4194304));
            }
            std::cout << "HOST_ACCEPTED_CACHE=" << e.path << '\n';
        }
        if (mode == 3) {
            require(report.financial->failure && !report.financial->finalized &&
                        report.engine.complete_updates == 2 &&
                        report.financial->trials[0].account->units.raw() > 0,
                    "post-fill failure lost its posted prefix");
            unsigned fills = 0;
            for (const auto &event : report.financial->events) {
                fills += event.kind == "filled";
                require(!(event.key >= 2 && event.kind == "close"),
                        "post-fill failure reached a close decision");
            }
            require(fills == 1, "post-fill callback failure duplicated or removed the fill");
        }
    }
}
void funding_expiry_and_frozen_budget() {
    Environment e;
    auto config = e.suite();
    config.legacy_funding_close = true;
    config.trials[0].strategy_configuration = "long_from_funding_close";
    auto p = e.prepare(config);
    auto host =
        take(qb::HostedSimulation::create(e.data, p, e.build, config, callbacks(), e.types.price));
    auto report = take(host.run(e.retention(p), e.extra(p)));
    bool first_open = false;
    for (const auto &event : report.financial->events) {
        first_open |= event.kind == "filled" && event.key == 1;
    }
    require(first_open, "legacy funding close did not act at first evaluation open");
    Environment last;
    auto suite = last.suite();
    suite.trials[0].strategy_configuration = "enter_only_final_close";
    auto plan = last.prepare(suite);
    auto cb = callbacks();
    cb[0] = [](const qb::DecisionContext &c, const auto *, auto) -> qb::BacktestResult<qb::Action> {
        if (c.key == 4) {
            return qb::Action{qb::ActionKind::TargetLong, qd::Quantity::from_raw(0), "too_late"};
        }
        return hold();
    };
    auto end = take(qb::HostedSimulation::create(last.data, plan, last.build, suite, std::move(cb),
                                                 last.types.price));
    auto expired = take(end.run(last.retention(plan), last.extra(plan)));
    bool found = false;
    for (const auto &event : expired.financial->events) {
        found |= event.kind == "expired";
        require(event.kind != "filled", "expired intent executed");
    }
    require(found && !expired.financial->trials[0].pending &&
                expired.engine.acceptance == lx::AcceptanceStatus::Accepted,
            "final pending intent did not expire");
    auto opening = take(last.data.opening(1, last.data.rows()[1].bar.interval_start.unix_nanos()));
    auto a = take(qb::fund_account(9, opening.source(), money(1000000), qd::Quantity::from_raw(1)));
    qb::Intent frozen{9, 1, a.source, 0, 1, "frozen", take(qb::freeze_cash_budget(a))};
    a.cash = money(1000100);
    a.realized = money(100);
    a.revision = a.cash_revision = 1;
    a.last_event = qb::Account::EventLocation{0, qb::ValuationPhase::Close};
    auto quote = take(qb::quote_fill(frozen, opening, a, {5, 5}));
    require(std::holds_alternative<qb::OrderRejection>(quote) &&
                std::get<qb::OrderRejection>(quote).reason == qb::RejectionReason::CashChanged,
            "unsupported intervening flow changed frozen budget semantics");
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "scratch root required");
        root = argv[1];
        std::filesystem::create_directories(root);
        inspected_build = take(qb::inspect_financial_host(account(), {134217728, 1048576, 512}));
        causal_trials();
        risk_boundaries();
        admission_and_exhaustion();
        failure_and_identity();
        risk_execution_and_terminal();
        partial_numerical_step();
        native_acceptance_gate();
        funding_expiry_and_frozen_budget();
        std::cout
            << "PASS 8 hosted groups: causality, risk, admission/exhaustion, identity/failure, "
               "execution/terminal, partial step, native acceptance, funding/expiry\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
