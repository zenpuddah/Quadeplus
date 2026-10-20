#!/usr/bin/env python3
"""Native S1/S3 fresh exact/disabled-sharing equivalence on authored input.

Run four native CLI cases against one immutable 120-Bar trend fixture. Each S1
case passes verify_sma.verify. Each S3 case uses the independent Fraction strategy
and control references plus representation-only host/state adapters;
the historical-only SPY/archive admission helpers are deliberately not invoked.
Every financial event and CSV cell also matches between sharing modes. Rich report
metrics compare exactly, including serialized statistical values; native experiment
identity and report metadata are checked per run rather than erased from evidence.

This is mode equivalence for the existing S1/S3 graphs, which have no duplicated
branches to merge. It makes no producer-reduction, persisted Ratio reuse, historical
archive parity, or speedup claim. Native output directories are read-only after the
CLI returns; captured stdout/stderr are optional sibling diagnostics.
"""

import argparse
from fractions import Fraction
from pathlib import Path
import shlex
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import verify_sma
import verify_stochastic as s3
import verify_suite as suite

if not __debug__:
    raise SystemExit("Sharing verification requires assertions; do not use Python -O")

same = s3.same
same_rows = s3.same_rows


def execute(command):
    result = subprocess.run(
        [str(arg) for arg in command], text=True, capture_output=True, timeout=300
    )
    assert result.returncode == 0, f"command failed: {command}\n{result.stdout}\n{result.stderr}"
    return result


def fixture_inputs(dataset):
    manifest = {}
    lines = s3.bounded_bytes(dataset / "manifest.qdaily", s3.MB).decode().splitlines()
    same(lines[0], "quadeplus-daily-research-v1", "fixture manifest schema")
    for line in lines[1:]:
        fields = shlex.split(line)
        assert len(fields) == 2 and fields[0] not in manifest, (
            "duplicate/malformed fixture manifest field"
        )
        manifest[fields[0]] = fields[1]
    manifest_hash = s3.digest(dataset / "manifest.qdaily", s3.MB)
    same(
        s3.bounded_bytes(dataset / "COMPLETE", 256).decode().strip(),
        f"quadeplus-daily-research-v1 {manifest_hash}",
        "fixture completion",
    )
    for name, field in (
        ("prices.csv", "prices_sha256"),
        ("sessions.csv", "sessions_sha256"),
        ("normalization.csv", "normalizations_sha256"),
    ):
        same(s3.digest(dataset / name, s3.MB), manifest[field], f"fixture retained {name}")
    for field, wanted in dict(
        symbol="TEST",
        currency="USD",
        schedule="synthetic_session_grid_v1",
        normalization=s3.ref.NORMALIZATION,
        missing_sessions="reject",
        clock="date_utc_midnight_close_plus_second_v1",
    ).items():
        same(manifest[field], wanted, f"authored fixture {field}")
    bars, _, _ = s3.ref.load_prices(
        dataset / "prices.csv", manifest["numerical_origin"], manifest["evaluation_end"]
    )
    _, grid = s3.csv_read(dataset / "sessions.csv", s3.MB, 128)
    same([bar.date for bar in bars], [row["Date"] for row in grid], "full authored dated grid")
    same(len(bars), 120, "authored source rows")
    begin = next(i for i, bar in enumerate(bars) if bar.date == manifest["evaluation_begin"])
    same(begin, 1, "authored funding/evaluation split")
    return manifest, manifest_hash, bars, begin


def configure(template, family, sharing, manifest, path):
    tree = ET.parse(template)
    root = tree.getroot()
    same(root.tag, "backtest", "domain configuration root")
    assert not list(root), "unexpected domain configuration children"
    same(
        root.attrib["strategy"],
        "sma20_50_long_cash_v1" if family == "sma" else "spy_stochastic5_v1",
        "strategy template",
    )
    root.attrib.update(
        symbol="TEST",
        currency="USD",
        profile="fresh_research_v1",
        numerical_origin=manifest["numerical_origin"],
        evaluation_begin=manifest["evaluation_begin"],
        evaluation_end=manifest["evaluation_end"],
        initial_cash_cents="1000000",
        lot_step_raw="1",
        commission_bps="5",
        slippage_bps="5",
        terminal="mark",
        sharing=sharing,
        cache="disabled",
    )
    with path.open("xb") as stream:
        tree.write(stream, encoding="utf-8", xml_declaration=True)
    return dict(root.attrib)


def check_graph(output, family, cfg, count, manifest_hash):
    graph = ET.fromstring(s3.bounded_bytes(output / "graph.xml", s3.MB))
    same(graph.find("run").attrib["sharing"], cfg["sharing"], "native graph sharing mode")
    same(graph.find("cache").attrib, {"mode": "disabled"}, "fresh cache-disabled graph")
    sources = graph.findall("sources/source")
    same(len(sources), 1, "one immutable Bar source")
    for field, wanted in dict(
        id="s",
        operation="quadeplus.daily_research",
        source_version=manifest_hash,
        begin_step="0",
        end_step=str(count),
        missing_intervals="strict",
    ).items():
        same(sources[0].attrib[field], wanted, f"Bar source {field}")
    if family == "stochastic":
        # Checks the actual stochastic operation's Bar -> k port, [4,N) request,
        # ratio-only graph, two producer counters and zero cached deliveries.
        # This helper has no historical SPY restriction and writes no artifacts.
        s3.check_numerical_evidence(output, count, cfg, manifest_hash)
        return
    nodes = {node.attrib["id"]: node for node in graph.findall("nodes/node")}
    same(set(nodes), {"close", "sma20", "sma50"}, "S1 Price graph inventory")
    same(nodes["close"].attrib["operation"], "qaedalus.bar_close", "S1 Bar-to-Price operation")
    same(
        [
            (p.attrib["port"], p.attrib["producer"], p.attrib["output"])
            for p in nodes["close"].findall("input")
        ],
        [("bar", "s", "bar")],
        "S1 source Bar port",
    )
    observations = graph.findall("observations/observe")
    same(len(observations), 2, "S1 required observation count")
    for window in (20, 50):
        name = f"sma{window}"
        node = nodes[name]
        same(node.attrib["operation"], "statistics.sma_price", "S1 typed Price mean")
        same(
            [
                (p.attrib["port"], p.attrib["producer"], p.attrib["output"])
                for p in node.findall("input")
            ],
            [("price", "close", "price")],
            "S1 Price input",
        )
        params = {p.attrib["name"]: p.attrib["value"] for p in node.findall("param")}
        same(params["window"], str(window), "fixed S1 window")
        observation = next(o for o in observations if o.attrib["id"] == name)
        same(
            observation.attrib,
            dict(
                id=name,
                producer=name,
                output="mean",
                begin_step=str(window - 1),
                end_step=str(count),
                absence="require_value",
                prior_outputs="0",
                missing_intervals="strict",
            ),
            "S1 required Price scope",
        )


def verify_s3(output, cfg, bars, begin, manifest_hash):
    references = s3.references(bars, begin, cfg)
    events, experiment = s3.financial_read(output / "financial.txt")
    same_rows(
        events, s3.financial_expected(references), "synthetic S3/control exact financial oracle"
    )
    trace = s3.trace_read(output / "stochastic.csv", len(bars))
    same_rows(
        trace,
        s3.trace_expected(references, bars, begin, "fresh"),
        "synthetic S3 exact K/episode/state oracle",
    )
    s3.same_json(
        s3.json_read(output / "summary.json"),
        s3.summary_expected(references, bars, begin, cfg, manifest_hash, experiment),
        "synthetic S3/control exact summary",
    )
    assert references[0]["fills"], "authored fixture must exercise S3 trading"
    assert any(event[0] == 1 and event[3:5] == ["filled", "timer_exit"] for event in events), (
        "authored fixture did not exercise strict S3 episode timer exit"
    )
    print(
        f"S3_SYNTHETIC_EXACT_PASS bars={len(bars)} financial_rows={len(events)} "
        f"state_rows={len(trace)}"
    )


class ReportFiles:
    def __init__(self, output):
        self.output = output

    def read(self, name, maximum):
        return s3.bounded_bytes(self.output / name, maximum)


def rich_reports(output, events, summary, manifest, manifest_hash, bars, begin, experiment):
    reports = []
    same(
        [trial["id"] for trial in summary["trials"]],
        [1, 2, 3],
        "independent strategy/cash/buy-hold trial IDs",
    )
    same(summary["financial_state"], "completed", "financial completion")
    same(summary["evaluation_sessions"], len(bars) - begin, "summary evaluation scope")
    for trial, scalar in enumerate(summary["trials"], 1):
        document = suite.parse_json(
            s3.bounded_bytes(output / f"report-{trial}.json", 16 * s3.MB).decode()
        )
        same(
            set(document),
            {"schema", "financial_state", "metadata", "metrics"},
            "rich report envelope",
        )
        same(document["schema"], "quadeplus-report-evidence-v1", "rich report schema")
        same(document["financial_state"], "completed", "rich report completion")
        metadata, metrics = document["metadata"], document["metrics"]
        for field, wanted in dict(
            trial_id=str(trial),
            strategy_id=scalar["strategy"],
            symbol="TEST",
            currency="USD",
            profile="fresh_research_v1",
            experiment_sha256=experiment,
            dataset_manifest_sha256=manifest_hash,
            prices_sha256=manifest["prices_sha256"],
            sessions_sha256=manifest["sessions_sha256"],
            numerical_origin=bars[0].date,
            numerical_end=bars[-1].date,
            numerical_source_rows=str(len(bars)),
            funding_date=bars[begin - 1].date,
            funding_key=str(begin - 1),
            evaluation_begin=bars[begin].date,
            evaluation_end=bars[-1].date,
            evaluation_begin_key=str(begin),
            evaluation_end_key_exclusive=str(len(bars)),
            declared_evaluation_sessions=str(len(bars) - begin),
            initial_cash_cents="1000000",
            lot_step_raw="1",
            commission_bps="5",
            slippage_bps="5",
            terminal_policy="mark",
            drawdown_breaker_bps="disabled",
            daily_loss_breaker_bps="disabled",
            control_timing="first_evaluation_close_intent_may_fill_next_open",
        ).items():
            same(metadata[field], wanted, f"trial {trial} metadata {field}")
        suite.all_metrics(metrics)  # Finite statistics and explicit undefined representations.
        same(
            metrics["sessions"],
            len(bars) - begin,
            "funding excluded from report session denominator",
        )
        same(metrics["funded_date"], bars[begin - 1].date, "report funding date")
        same(metrics["final_date"], bars[-1].date, "report endpoint date")
        same(metrics["funded_equity_cents"], 1000000, "separate report funding baseline")
        same(metrics["terminal_policy"], "mark", "report endpoint policy")
        same(metrics["terminal_cash_cents"], None, "no synthetic reporting sale")
        mapping = dict(
            final_cash_cents="cash_cents",
            final_units_raw="units_raw",
            final_equity_cents="equity_cents",
            net_pnl_cents="pnl_cents",
            realized_trading_pnl_cents="realized_cents",
            unrealized_pnl_cents="unrealized_cents",
            dividends_cents="dividend_cents",
            fees_cents="fee_cents",
        )
        for field, source in mapping.items():
            same(metrics[field], scalar[source], f"report/verified-summary {field}")
        same(
            suite.metric(metrics["total_return"]),
            Fraction(scalar["equity_cents"] - 1000000, 1000000),
            "funded total-return denominator",
        )
        closes = [
            event
            for event in events
            if event[0] == trial and event[3] == "close" and event[1] >= begin
        ]
        same(len(metrics["daily"]), len(closes), "report daily coverage")
        previous, peak = 1000000, 1000000
        for daily, event in zip(metrics["daily"], closes):
            key, cash, units, equity = event[1], event[5], event[6], event[11]
            same(daily["date"], bars[key].date, "report daily source date")
            same(daily["ordinary_equity_cents"], equity, "ordinary report equity")
            same(daily["reporting_equity_cents"], equity, "marked report equity")
            same(daily["invested_units_raw"], units, "report position units")
            same(daily["invested_value_cents"], equity - cash, "report marked position")
            same(
                suite.metric(daily["daily_return"]),
                Fraction(equity - previous, previous),
                "daily funded return",
            )
            peak = max(peak, equity)
            same(
                suite.metric(daily["drawdown"]),
                Fraction(peak - equity, peak),
                "daily high-water drawdown",
            )
            previous = equity
        if trial == 2:
            same(scalar["cash_cents"], 1000000, "independent no-interest cash capital")
            same(scalar["units_raw"], 0, "cash control units")
            same(
                metrics["sharpe_zero_risk_free"],
                {"value": None, "undefined_reason": "zero_return_variance"},
                "cash Sharpe undefined",
            )
            same(
                metrics["profit_factor"],
                {"value": None, "undefined_reason": "no_losing_trade_pnl"},
                "cash profit factor undefined",
            )
        suite.projections(ReportFiles(output), f"report-{trial}", document)
        reports.append(metrics)
    return reports


def verify_run(dataset, config_path, output, family, manifest, manifest_hash, bars, begin):
    cfg = ET.parse(config_path).getroot().attrib
    same(
        ET.fromstring(s3.bounded_bytes(output / "effective-config.xml", 65536)).attrib,
        cfg,
        "effective domain configuration",
    )
    check_graph(output, family, cfg, len(bars), manifest_hash)
    if family == "sma":
        verify_sma.verify(dataset, config_path, output)
        trace = s3.csv_read(output / "decisions.csv")
    else:
        verify_s3(output, cfg, bars, begin, manifest_hash)
        trace = s3.csv_read(output / "stochastic.csv")
    events, experiment = s3.financial_read(output / "financial.txt")
    summary = s3.json_read(output / "summary.json")
    same(summary["dataset_sha256"], manifest_hash, "summary source identity")
    same(summary["experiment"], experiment, "summary/ledger native experiment")
    for trial in (1, 2, 3):
        funded = [event for event in events if event[0] == trial and event[3] == "funded"]
        same(len(funded), 1, "exactly one funding event per independent account")
        same(funded[0][1], begin - 1, "funding key")
        same(funded[0][5:7], [1000000, 0], "independent initial cash/units")
    buys = [event for event in events if event[0] == 3 and event[3] == "filled"]
    same(len(buys), 1, "buy-and-hold has one entry without rebalancing")
    same(buys[0][1], begin + 1, "fresh buy-and-hold next-open timing")
    same(buys[0][17:20], [begin, begin + 1, "cash_budget"], "buy-and-hold decision/eligibility")
    reports = rich_reports(
        output, events, summary, manifest, manifest_hash, bars, begin, experiment
    )
    return dict(events=events, trace=trace, trials=summary["trials"], reports=reports)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--sma-config", required=True, type=Path)
    parser.add_argument("--stochastic-config", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="sharing.", dir=args.scratch)).resolve()
    runner = args.runner.resolve()
    runner_hash = s3.digest(runner, 256 * s3.MB)
    fixture = root / "fixture"
    execute([args.fixture.resolve(), "--fixture", fixture, "trend", 1])
    dataset = fixture / "dataset"
    manifest, manifest_hash, bars, begin = fixture_inputs(dataset)
    results = {}
    for family, template in (("sma", args.sma_config), ("stochastic", args.stochastic_config)):
        for sharing in ("exact", "disabled"):
            label = f"{family}-{sharing}"
            config_path, output = root / f"{label}.xml", root / label
            configure(template, family, sharing, manifest, config_path)
            result = execute(
                [runner, "run", "--config", config_path, "--dataset", dataset, "--out", output]
            )
            assert any(line.startswith("RUN_ACCEPTED ") for line in result.stdout.splitlines()), (
                result.stdout
            )
            # Diagnostic capture is a sibling; accepted native evidence is never modified.
            with (root / f"{label}.stdout.txt").open("x") as stream:
                stream.write(result.stdout)
            with (root / f"{label}.stderr.txt").open("x") as stream:
                stream.write(result.stderr)
            results[family, sharing] = verify_run(
                dataset, config_path, output, family, manifest, manifest_hash, bars, begin
            )
        left, right = (results[family, mode] for mode in ("exact", "disabled"))
        same_rows(left["events"], right["events"], f"{family} shared/unshared financial events")
        same(left["trace"][0], right["trace"][0], f"{family} shared/unshared trace columns")
        same_rows(left["trace"][1], right["trace"][1], f"{family} shared/unshared exact CSV cells")
        s3.same_json(left["trials"], right["trials"], f"{family} shared/unshared trial summaries")
        s3.same_json(left["reports"], right["reports"], f"{family} shared/unshared rich metrics")
        print(
            f"FRESH_SHARING_EQUIVALENCE_PASS family={family} financial_rows={len(left['events'])} "
            f"trace_rows={len(left['trace'][1])} reports=3 cache=disabled"
        )
    for trial in (2, 3):
        same_rows(
            [event for event in results["sma", "exact"]["events"] if event[0] == trial],
            [event for event in results["stochastic", "exact"]["events"] if event[0] == trial],
            "independent matched cash/buy-hold controls across families",
        )
    same(
        s3.digest(runner, 256 * s3.MB),
        runner_hash,
        "native binary remained unchanged during four-run gate",
    )
    same(fixture_inputs(dataset)[1], manifest_hash, "retained fixture remained unchanged")
    print(
        f"SHARING_GATE_PASS native_runs=4 independent_strategy_oracles=2 rich_reports=12 evidence={root}"
    )


if __name__ == "__main__":
    main()
