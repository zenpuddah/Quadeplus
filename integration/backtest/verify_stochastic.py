#!/usr/bin/env python3
"""Exact native S3/controls gate; stdlib only, never writes into native bundles.

Existing run:
  --dataset BUNDLE --config XML --output RUN [--research-bundle ARCHIVE] [--scratch DIR]
Five historical CLI runs (explicit, not the fast CTest gate):
  --runner CLI --research-bundle ARCHIVE --dataset BUNDLE --scratch EXISTING_DIR
Fast mathematical checks without claiming native verification:
  --synthetic-only

The strategy reference is verify_stochastic_reference.simulate(). This module
adapts its events to the native host's 21 fields and its state to stochastic.csv's
35 columns. Controls have a separate cash/buy-and-hold accounting loop. No native
indicator or financial values feed expected results. Unreduced native K is compared
as an exact Fraction. All other integers, reasons, dates and row ordering match
exactly. Archive drift is itemized, never accepted with a monetary tolerance.

native_verified means these supplied native artifacts passed actual comparisons.
Only suite mode additionally observes the CLI's successful RUN_ACCEPTED result;
an existing completed summary alone is not an authoritative acceptance marker.
Existing-run verification also admits legacy 20+20 bps as a native-policy
diagnostic. It has no original archived float baseline; only that comparison is
skipped, with archive_reference_available=false and no invented monetary delta.
"""

import argparse
from collections import defaultdict
import csv
from datetime import date
from fractions import Fraction as F
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import verify_stochastic_reference as ref


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ARCHIVE = ROOT / "build/backtest-reference/strategy-execution-2026-10-18"
MB = 1024**2
STRATEGIES = ("spy_stochastic5_v1", "cash_usd_no_interest_v1", "buy_hold95_next_open_v1")
TRACE_FIELDS = (
    "trial,key,date,kind,may_submit,funded,k_numerator,k_denominator,k_available_at_ns,"
    "episode_start,episode_action,target,entry_consumed,held_units_raw,pending_side,"
    "pending_decision_key,parity_failed,action,reason,order_id,decision_key,eligible_key,"
    "side,synthetic,rejection_code,quantity_raw,price_raw,gross_cents,fee_cents,cash_cents,"
    "units_raw,basis_cents,realized_cents,dividend_cents,total_fee_cents"
).split(",")
TRACE_TEXT = {"date", "kind", "episode_action", "pending_side", "reason", "side"}
BALANCES = (
    "cash_cents",
    "units_raw",
    "basis_cents",
    "realized_cents",
    "dividend_cents",
    "fee_cents",
)
NATIVE_FILES = {
    "financial.txt": 16 * MB,
    "stochastic.csv": 32 * MB,
    "summary.json": MB,
    "effective-config.xml": 65536,
    "graph.xml": MB,
    "producers.csv": MB,
    "identities.csv": MB,
    "input-identity.txt": 65536,
    "retention.log": 512 * MB,
}
if not __debug__:
    raise SystemExit("Exact verification requires assertions; do not use Python -O")


def same(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: actual={actual!r}; expected={expected!r}")


def same_rows(actual, expected, label):
    same(len(actual), len(expected), f"{label} row count")
    for index, (got, wanted) in enumerate(zip(actual, expected)):
        same(got, wanted, f"{label} row {index}")


def same_json(actual, expected, label):
    # Python normally equates 1, 1.0 and True. Monetary JSON coefficients must
    # retain their declared integer type rather than benefiting from that rule.
    same(type(actual), type(expected), f"{label} JSON type")
    if isinstance(expected, dict):
        same(set(actual), set(expected), f"{label} fields")
        for key in expected:
            same_json(actual[key], expected[key], f"{label}.{key}")
    elif isinstance(expected, list):
        same(len(actual), len(expected), f"{label} length")
        for index, (got, wanted) in enumerate(zip(actual, expected)):
            same_json(got, wanted, f"{label}[{index}]")
    else:
        same(actual, expected, label)


def bounded_bytes(path, maximum):
    assert path.is_file() and not path.is_symlink(), f"missing/nonregular evidence: {path}"
    assert path.stat().st_size <= maximum, f"oversized evidence: {path}"
    with path.open("rb") as stream:
        value = stream.read(maximum + 1)
    assert len(value) <= maximum
    return value


def digest(path, maximum):
    assert path.is_file() and not path.is_symlink() and path.stat().st_size <= maximum, path
    result, count = hashlib.sha256(), 0
    with path.open("rb") as stream:
        while chunk := stream.read(MB):
            count += len(chunk)
            assert count <= maximum, path
            result.update(chunk)
    return result.hexdigest()


def csv_read(path, maximum=32 * MB, max_rows=100000):
    reader = csv.DictReader(io.StringIO(bounded_bytes(path, maximum).decode("utf-8"), newline=""))
    fields = reader.fieldnames
    assert fields and len(fields) == len(set(fields)), f"invalid CSV header: {path}"
    rows = []
    for row in reader:
        assert len(rows) < max_rows and None not in row and None not in row.values(), (
            f"invalid CSV row: {path}"
        )
        rows.append(row)
    return fields, rows


def integer(text):
    assert re.fullmatch(r"0|-?[1-9][0-9]*", text), f"noncanonical integer: {text!r}"
    return int(text)


def unique_json(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate JSON key: {key}"
        result[key] = value
    return result


def json_read(path):
    return json.loads(bounded_bytes(path, MB), object_pairs_hook=unique_json)


def write_report(path, value, protected, maximum=32 * MB):
    path = path.resolve()
    assert all(not path.is_relative_to(p.resolve()) for p in protected), (
        "reports must be outside protected bundles"
    )
    payload = json.dumps(value, indent=2, default=ref.json_default) + "\n"
    assert len(payload.encode()) <= maximum, "verification report exceeds bound"
    with path.open("x", encoding="utf-8") as stream:
        stream.write(payload)


def config_read(path):
    root = ET.fromstring(bounded_bytes(path, 65536))
    assert root.tag == "backtest" and not list(root)
    cfg = root.attrib
    fixed = dict(
        version="1",
        strategy=STRATEGIES[0],
        symbol="SPY",
        currency="USD",
        numerical_origin=ref.ORIGIN,
        evaluation_end=ref.END,
        initial_cash_cents="1000000",
        lot_step_raw="1",
        allocation_bps="9500",
        window="5",
        zero_range="1/2",
        entry="20",
        exit="80",
        max_holding_bars="10",
        controls="cash,buy_hold",
        cache="disabled",
        resource_profile="stochastic_cli_bounds_v1",
    )
    for key, value in fixed.items():
        same(cfg[key], value, f"historical S3 config {key}")
    legacy = cfg["profile"] == "legacy_spy_2026_v1"
    assert legacy or cfg["profile"] == "fresh_research_v1"
    same(
        cfg["evaluation_begin"],
        "2026-08-17" if legacy else "2010-01-04",
        "profile evaluation begin",
    )
    same(cfg["terminal"], "synthetic_close" if legacy else "mark", "profile terminal")
    cost = integer(cfg["commission_bps"])
    assert cost in (5, 10, 20)
    same(integer(cfg["slippage_bps"]), cost, "paired historical costs")
    assert cfg["sharing"] in ("exact", "disabled") and integer(cfg["max_price_raw"]) > 0
    return cfg


def inputs(dataset, cfg):
    lines = bounded_bytes(dataset / "manifest.qdaily", MB).decode().splitlines()
    same(lines[0], "quadeplus-daily-research-v1", "dataset format")
    manifest = {}
    for line in lines[1:]:
        parts = shlex.split(line)
        assert len(parts) == 2 and parts[0] not in manifest
        manifest[parts[0]] = parts[1]
    manifest_hash = digest(dataset / "manifest.qdaily", MB)
    same(
        bounded_bytes(dataset / "COMPLETE", 256).decode().strip(),
        f"quadeplus-daily-research-v1 {manifest_hash}",
        "dataset completion",
    )
    for filename, field in (
        ("prices.csv", "prices_sha256"),
        ("sessions.csv", "sessions_sha256"),
        ("normalization.csv", "normalizations_sha256"),
    ):
        same(digest(dataset / filename, 16 * MB), manifest[field], f"dataset retained {filename}")
    for key, value in dict(
        prices_sha256=ref.SPY_SHA,
        symbol="SPY",
        currency="USD",
        numerical_origin=ref.ORIGIN,
        evaluation_end=ref.END,
        normalization=ref.NORMALIZATION,
        schedule="retained_reference_grid_v1",
        clock="date_utc_midnight_close_plus_second_v1",
        missing_sessions="reject",
        adjustment="split_adjusted_ohlc_dividends_same_share_basis_v1",
    ).items():
        same(manifest[key], value, f"dataset identity {key}")
    assert manifest["evaluation_begin"] <= cfg["evaluation_begin"] <= manifest["evaluation_end"]
    bars, original, changes = ref.load_prices(dataset / "prices.csv")
    same(len(bars), 5511, "numerical rows")
    same(len(changes), 799, "Price9 normalization count")
    same(max(abs(r["error"]) for r in changes), F(1, 2 * ref.P), "maximum Price9 normalization")
    _, sessions = csv_read(dataset / "sessions.csv", 16 * MB, 8000)
    same(
        [r["Date"] for r in sessions if ref.ORIGIN <= r["Date"] <= ref.END],
        [b.date for b in bars],
        "sealed session grid",
    )
    assert all(
        0 < p <= integer(cfg["max_price_raw"])
        for b in bars
        for p in (b.opening, b.high, b.low, b.close)
    )
    begin = next(i for i, b in enumerate(bars) if b.date == cfg["evaluation_begin"])
    return bars, original, begin, manifest, manifest_hash


def control_reference(bars, begin, buy_hold, profile, initial, lot, commission, slippage):
    """Independent controls: no signals, interest, rebalancing or risk overlays."""
    events, fills = [], []
    cash, units, basis, realized, dividends, fees = initial, 0, 0, 0, 0, 0
    pending, next_id = None, 1

    def emit(key, kind, mark, phase, **extra):
        equity = cash + ref.even(F(units * mark, 10**13))
        assert cash >= 0 and units >= 0 and units % lot == 0
        assert equity - initial == realized + equity - cash - basis + dividends
        event = dict(
            key=key,
            date=bars[key].date,
            kind=kind,
            mark_raw=mark,
            phase=phase,
            cash_cents=cash,
            units_raw=units,
            basis_cents=basis,
            realized_cents=realized,
            dividend_cents=dividends,
            fee_cents=fees,
            equity_cents=equity,
            **extra,
        )
        events.append(event)
        return event

    def fill(key, order, raw, phase):
        nonlocal cash, units, basis, realized, fees
        buying = order["side"] == "Buy"
        p = ref.execution(raw, buying, slippage)
        quantity = ref.affordable(p, order["budget_cents"], lot, commission) if buying else units
        if quantity == 0:
            emit(key, "rejected", raw, phase, **order, reason="BelowMinimumLot")
            return
        gross, fee = ref.amounts(p, quantity, commission)
        detail = dict(
            order,
            price_raw=p,
            quantity_raw=quantity,
            gross_cents=gross,
            commission_cents=fee,
            raw_price=raw,
        )
        emit(key, "accepted", raw, phase, **detail)
        if buying:
            assert (
                units == 0
                and cash == order["decision_cash_cents"]
                and gross + fee <= order["budget_cents"]
            )
            cash -= gross + fee
            units, basis = quantity, gross + fee
        else:
            cash += gross - fee
            realized += gross - fee - basis
            units, basis = 0, 0
        fees += fee
        fills.append(emit(key, "filled", raw, phase, **detail))

    entry = begin - 1 if profile == "legacy" else begin
    for key in range(begin - 1, len(bars)):
        if key == begin - 1:
            emit(key, "funded", bars[key].close, "Close")
        else:
            payment = ref.even(F(units * bars[key].dividend, 10**13))
            cash += payment
            dividends += payment
            emit(key, "dividend", bars[key - 1].close, "PriorClose", payment_cents=payment)
            if pending:
                fill(key, pending, bars[key].opening, "Open")
                pending = None
        emit(key, "close", bars[key].close, "Close")
        if buy_hold and key == entry:
            pending = dict(
                order_id=next_id,
                side="Buy",
                decision_key=key,
                signal_date=bars[key].date,
                eligible_key=key + 1,
                decision_reason="buy_hold_control",
                budget_cents=cash * 95 // 100,
                decision_cash_cents=cash,
                synthetic=False,
            )
            next_id += 1
            emit(key, "submitted", bars[key].close, "Close", **pending)
    key = len(bars) - 1
    if pending:
        emit(key, "expired", bars[key].close, "Close", **pending, reason="end_of_scope")
    if profile == "legacy" and units:
        fill(
            key,
            dict(
                order_id=next_id,
                side="Sell",
                decision_key=None,
                signal_date="",
                eligible_key=None,
                decision_reason="synthetic_terminal",
                budget_cents=0,
                decision_cash_cents=cash,
                synthetic=True,
            ),
            bars[key].close,
            "Terminal",
        )
    emit(key, "final", bars[key].close, "Close")
    return dict(events=events, fills=fills)


def references(bars, begin, cfg):
    profile = "legacy" if cfg["profile"] == "legacy_spy_2026_v1" else "fresh"
    arguments = dict(
        initial=integer(cfg["initial_cash_cents"]),
        lot=integer(cfg["lot_step_raw"]),
        commission=integer(cfg["commission_bps"]),
        slippage=integer(cfg["slippage_bps"]),
    )
    strategy = ref.simulate(bars, begin, profile, **arguments)
    controls = [
        control_reference(bars, begin, hold, profile, **arguments) for hold in (False, True)
    ]
    return [strategy, *controls]


def event_stage(event):
    if event["kind"] in ("expired", "final") or event["phase"] == "Terminal":
        return 2
    return 0 if event["phase"] in ("PriorClose", "Open") else 1


def native_event(event, trial):
    """Representation-only translation; all balances come from the oracle."""
    kind = event["kind"]
    synthetic = event.get("synthetic", False)
    if kind == "accepted" and synthetic:
        return None  # Native reporting sale posts one terminal event, no opening acceptance.
    filled = kind in ("accepted", "filled")
    has_order = kind in ("submitted", "accepted", "filled", "rejected", "expired")
    marked = kind not in ("submitted", "rejected", "expired")
    reason = {
        "funded": "initial_cash",
        "dividend": "ex_date_cash",
        "close": "ordinary",
        "final": "endpoint",
        "expired": "end_of_scope",
        "rejected": "0",
    }.get(kind)
    if reason is None:
        reason = "synthetic_terminal_liquidation" if synthetic else event["decision_reason"]
    phase = (
        {"PriorClose": 0, "Open": 1, "Close": 2, "Terminal": 2}[event["phase"]] if marked else None
    )
    policy = (
        ("cash_budget" if event["side"] == "Buy" else "sell_all")
        if has_order and not synthetic
        else "none"
    )
    return [
        trial,
        event["key"],
        event.get("order_id", 0) if has_order else 0,
        "terminal" if synthetic else kind,
        reason,
        *(event[k] for k in BALANCES),
        event["equity_cents"] if marked else None,
        phase,
        event["price_raw"] if filled else event["mark_raw"] if marked else 0,
        event["quantity_raw"] if filled else 0,
        event["gross_cents"] if filled else 0,
        event["commission_cents"] if filled else 0,
        event.get("decision_key") if has_order and not synthetic else None,
        event.get("eligible_key") if has_order and not synthetic else None,
        policy,
        event["budget_cents"] if policy == "cash_budget" else 0,
    ]


def financial_expected(results):
    ordered = []
    for trial, result in enumerate(results, 1):
        for index, event in enumerate(result["events"]):
            row = native_event(event, trial)
            if row is not None:
                ordered.append(((event["key"], event_stage(event), trial, index), row))
    return [row for _, row in sorted(ordered)]


def financial_read(path):
    lines = bounded_bytes(path, 16 * MB).decode().splitlines()
    same(lines[0], "quadeplus_financial_ledger_v1", "financial format")
    assert lines[1].startswith("experiment=") and lines[2] == "financial_state=completed"
    experiment = lines[1].split("=", 1)[1]
    assert re.fullmatch(r"[0-9a-f]{64}", experiment)
    prefix, configuration = lines[3].split(":", 1)
    same(
        prefix,
        f"configuration_bytes={len(configuration.encode())}",
        "financial configuration byte length",
    )
    same(
        hashlib.sha256(configuration.encode()).hexdigest(),
        experiment,
        "financial experiment identity",
    )
    rows = []
    for line in lines[4:]:
        assert line, "unexpected empty ledger record"
        fields = shlex.split(line)
        same(len(fields), 21, "financial field count")
        rows.append(
            [
                v if i in (3, 4, 19) else None if v == "none" else integer(v)
                for i, v in enumerate(fields)
            ]
        )
        assert len(rows) <= 128064
    return rows, experiment


def available_ns(day):
    return (date.fromisoformat(day) - date(1970, 1, 1)).days * 86400 * 10**9 + 10**9


def trace_expected(results, bars, begin, profile):
    by_key = []
    for result in results:
        lookup = defaultdict(list)
        for event in result["events"]:
            lookup[event["key"]].append(event)
        by_key.append(lookup)
    rows = []
    episode, consumed, pending_side, pending_key, held = None, False, None, None, 0

    def row_for(trial, key, kind, account=None):
        row = dict.fromkeys(TRACE_FIELDS)
        row.pop("k_numerator")
        row.pop("k_denominator")
        row.update(trial=trial, key=key, date=bars[key].date, kind=kind, k=None)
        if account:
            row.update({field: account[field] for field in BALANCES if field != "fee_cents"})
            row["total_fee_cents"] = account["fee_cents"]
        return row

    def state(row):
        row.update(
            episode_start=episode,
            target=int(episode is not None),
            entry_consumed=int(consumed),
            held_units_raw=held,
            pending_side=pending_side,
            pending_decision_key=pending_key,
            parity_failed=0,
        )

    def execution_row(trial, event):
        nonlocal pending_side, pending_key, held
        synthetic = event.get("synthetic", False)
        row = row_for(trial, event["key"], "terminal" if synthetic else event["kind"], event)
        row.update(
            funded=1,
            order_id=event["order_id"],
            decision_key=event.get("decision_key"),
            eligible_key=event.get("eligible_key"),
        )
        if event["kind"] == "rejected":
            row.update(reason="order_rejected", rejection_code=0)
        else:
            row.update(
                reason="synthetic_terminal_liquidation" if synthetic else event["decision_reason"],
                side=event["side"],
                synthetic=int(synthetic),
                quantity_raw=event["quantity_raw"],
                price_raw=event["price_raw"],
                gross_cents=event["gross_cents"],
                fee_cents=event["commission_cents"],
            )
        if trial == 1:
            held, pending_side, pending_key = event["units_raw"], None, None
            state(row)
        rows.append(row)

    for key in range(len(bars)):
        for trial in range(1, 4):
            for event in by_key[trial - 1][key]:
                if event_stage(event) == 0 and event["kind"] in ("filled", "rejected"):
                    execution_row(trial, event)
        for trial in range(1, 4):
            events = by_key[trial - 1][key]
            close = next((e for e in events if e["kind"] == "close"), None)
            order = next((e for e in events if e["kind"] == "submitted"), None)
            allowed = key >= (begin - 1 if profile == "legacy" else begin)
            row = row_for(trial, key, "decision", close)
            row.update(
                may_submit=int(allowed),
                funded=int(key >= begin - 1),
                decision_key=key,
                action=(1 if order["side"] == "Buy" else 2) if order else 0,
                eligible_key=key + 1 if order else None,
            )
            if trial == 1:
                decision = results[0]["decisions"][key]
                if decision["episode_start"] != episode:
                    consumed = False
                episode = decision["episode_start"]
                if order:
                    pending_side, pending_key = order["side"], key
                    if pending_side == "Buy":
                        consumed = True
                k = decision["k"]
                row.update(
                    k=k,
                    k_available_at_ns=available_ns(bars[key].date) if k is not None else None,
                    episode_action=decision["action"],
                    reason="warmup"
                    if not allowed and (profile == "fresh" or k is not None)
                    else "warming"
                    if k is None
                    else decision["action"],
                )
                state(row)
            else:
                row["reason"] = "cash_control" if trial == 2 else "buy_hold_control"
            rows.append(row)
    for trial in range(1, 4):
        for event in by_key[trial - 1][len(bars) - 1]:
            if event.get("synthetic") and event["kind"] == "filled":
                execution_row(trial, event)
    return rows


def trace_read(path, count):
    fields, rows = csv_read(path, 32 * MB, 5 * count + 32)
    same(fields, TRACE_FIELDS, "stochastic 35-column schema")
    result = []
    for row in rows:
        value = {
            k: None if v == "" else v if k in TRACE_TEXT else integer(v) for k, v in row.items()
        }
        numerator, denominator = value.pop("k_numerator"), value.pop("k_denominator")
        assert (numerator is None) == (denominator is None), "partial rational receipt"
        if numerator is not None:
            assert 0 <= numerator <= denominator and denominator > 0
        value["k"] = None if numerator is None else F(numerator, denominator)
        result.append(value)
    return result


def summary_expected(results, bars, begin, cfg, manifest_hash, experiment):
    initial = integer(cfg["initial_cash_cents"])
    expected = dict(
        format="quadeplus_stochastic_summary_v1",
        profile=cfg["profile"],
        financial_state="completed",
        dataset_sha256=manifest_hash,
        experiment=experiment,
        window_status="previously_inspected_research_not_holdout",
        terminal=cfg["terminal"],
        dividend_policy="ex_date_cash_compatibility_v1",
        numerical_origin=bars[0].date,
        funding_date=bars[begin - 1].date,
        evaluation_begin=bars[begin].date,
        evaluation_end=bars[-1].date,
        evaluation_sessions=len(bars) - begin,
        initial_cash_cents=initial,
        lot_step_raw=integer(cfg["lot_step_raw"]),
        commission_bps=integer(cfg["commission_bps"]),
        slippage_bps=integer(cfg["slippage_bps"]),
        trials=[],
    )
    for trial, result in enumerate(results, 1):
        final = result["events"][-1]
        assert final["kind"] == "final"
        closes = [e for e in result["events"] if e["kind"] == "close" and e["key"] >= begin]
        same(len(closes), len(bars) - begin, "reference evaluation close count")
        peak, loss, loss_peak = initial, 0, initial
        for event in closes:
            equity = (
                final["equity_cents"] if event["key"] == len(bars) - 1 else event["equity_cents"]
            )
            peak = max(peak, equity)
            if F(peak - equity, peak) > F(loss, loss_peak):
                loss, loss_peak = peak - equity, peak
        expected["trials"].append(
            dict(
                id=trial,
                strategy=STRATEGIES[trial - 1],
                cash_cents=final["cash_cents"],
                units_raw=final["units_raw"],
                equity_cents=final["equity_cents"],
                pnl_cents=final["equity_cents"] - initial,
                realized_cents=final["realized_cents"],
                unrealized_cents=final["equity_cents"] - final["cash_cents"] - final["basis_cents"],
                dividend_cents=final["dividend_cents"],
                fee_cents=final["fee_cents"],
                drawdown_loss_cents=loss,
                drawdown_peak_cents=loss_peak,
                completed_positions=sum(f["side"] == "Sell" for f in result["fills"]),
                rejected_orders=sum(e["kind"] == "rejected" for e in result["events"]),
                expired_orders=sum(e["kind"] == "expired" for e in result["events"]),
            )
        )
    return expected


def check_numerical_evidence(output, count, cfg, manifest_hash):
    graph = ET.fromstring(bounded_bytes(output / "graph.xml", MB))
    same(graph.find("cache").attrib, {"mode": "disabled"}, "S3 persistent cache disabled")
    same(graph.find("run").attrib["sharing"], cfg["sharing"], "sharing configuration")
    sources, nodes, observations = (
        graph.findall("sources/source"),
        graph.findall("nodes/node"),
        graph.findall("observations/observe"),
    )
    same((len(sources), len(nodes), len(observations)), (1, 1, 1), "source/ratio graph shape")
    source, node, obs = sources[0].attrib, nodes[0].attrib, observations[0].attrib
    for key, value in dict(
        id="s",
        operation="quadeplus.daily_research",
        source_version=manifest_hash,
        begin_step="0",
        end_step=str(count),
        missing_intervals="strict",
    ).items():
        same(source[key], value, f"numerical source {key}")
    for key, value in dict(
        id="k5",
        operation="quadeplus.stochastic5",
        semantic_version="1",
        implementation_id="stochastic-v1",
        initialization="empty_at_step_0",
    ).items():
        same(node[key], value, f"ratio operation {key}")
    parameters = nodes[0].findall("param")
    same(len(parameters), 3, "ratio parameter count")
    same(
        {p.attrib["name"]: p.attrib["value"] for p in parameters},
        dict(window="5", zero_range="1/2", coverage="strict_complete_bars"),
        "ratio parameter identity",
    )
    ports = nodes[0].findall("input")
    same(
        [p.attrib for p in ports],
        [
            dict(
                port="bar",
                producer="s",
                output="bar",
                prior_outputs="0",
                missing_intervals="strict",
            )
        ],
        "ratio Bar input",
    )
    same(
        obs,
        dict(
            id="k5",
            producer="k5",
            output="k",
            begin_step="4",
            end_step=str(count),
            absence="require_value",
            prior_outputs="0",
            missing_intervals="strict",
        ),
        "full historical ratio observation scope",
    )
    fields, rows = csv_read(output / "producers.csv", MB, 32)
    same(
        fields,
        "id,physical,advances,publications,aliases,waiting,cached_deliveries".split(","),
        "producer schema",
    )
    same({r["id"] for r in rows}, {"s", "k5"}, "producer inventory")
    same(len(rows), 2, "producer count")
    assert len({r["physical"] for r in rows}) == 2
    for row in rows:
        assert 0 <= integer(row["physical"]) < 32
        same(
            {k: integer(v) for k, v in row.items() if k not in ("id", "physical")},
            dict(
                advances=count,
                publications=count - (4 if row["id"] == "k5" else 0),
                aliases=0,
                waiting=0,
                cached_deliveries=0,
            ),
            f"{row['id']} producer counts",
        )
    fields, identities = csv_read(output / "identities.csv", MB, 32)
    same(fields, ["id", "digest"], "identity schema")
    same({row["id"] for row in identities}, {"s", "k5"}, "computation identity inventory")
    same(len(identities), 2, "identity count")
    assert all(re.fullmatch(r"[0-9a-f]{64}", row["digest"]) for row in identities)


def verify(dataset, config, output, archive, harness, label, acceptance_observed=False):
    """Verify real files only. Reports are siblings of native runs, never children."""
    protected = (dataset, output, archive)
    report_path, drift_path = (
        harness / f"{label}.verification.json",
        harness / f"{label}.reconciliation.json",
    )
    try:
        before = {name: digest(output / name, limit) for name, limit in NATIVE_FILES.items()}
        cfg = config_read(config)
        same(
            ET.fromstring(bounded_bytes(output / "effective-config.xml", 65536)).attrib,
            cfg,
            "effective domain config",
        )
        bars, original, begin, manifest, manifest_hash = inputs(dataset, cfg)
        profile = "legacy" if cfg["profile"] == "legacy_spy_2026_v1" else "fresh"
        decision_changes = ref.decision_audit(bars, original, begin, profile)
        same(decision_changes, [], "normalization/float threshold or episode changes block parity")
        results = references(bars, begin, cfg)
        same(results[0]["summary"]["parity_eligible"], True, "legacy rejection parity eligibility")
        if profile == "legacy":
            same(len(bars) - begin, 43, "legacy evaluation sessions exclude funding")
            same(bars[begin - 1].date, "2026-08-14", "legacy funding baseline date")
            same(len(results[0]["equity"]), 44, "legacy funding plus evaluation rows")
            same(len(results[0]["fills"]), 8, "legacy fills")
            same(
                [
                    (a["date"], b["date"])
                    for a, b in zip(results[0]["fills"][::2], results[0]["fills"][1::2])
                ],
                ref.EXPECTED_PAIRS,
                "legacy four trade date pairs",
            )
            assert all(f["decision_reason"] == "threshold_exit" for f in results[0]["fills"][1::2])
        actual_events, experiment = financial_read(output / "financial.txt")
        expected_events = financial_expected(results)
        same_rows(actual_events, expected_events, "native financial events (all three accounts)")
        actual_trace = trace_read(output / "stochastic.csv", len(bars))
        expected_trace = trace_expected(results, bars, begin, profile)
        same_rows(actual_trace, expected_trace, "native typed K/episode/decision/execution trace")
        expected_summary = summary_expected(results, bars, begin, cfg, manifest_hash, experiment)
        same_json(
            json_read(output / "summary.json"),
            expected_summary,
            "native summary including exact drawdown",
        )
        check_numerical_evidence(output, len(bars), cfg, manifest_hash)
        identity_lines = bounded_bytes(output / "input-identity.txt", 65536).decode().splitlines()
        same(identity_lines[0], "quadeplus_stochastic_input_v1", "input identity format")
        identity = dict(line.split("=", 1) for line in identity_lines[1:])
        same(len(identity), len(identity_lines) - 1, "duplicate input identity keys")
        same(
            identity,
            dict(
                manifest=manifest_hash,
                prices=manifest["prices_sha256"],
                sessions=manifest["sessions_sha256"],
                profile=cfg["profile"],
                rejection_policy="legacy_rejection_fails_parity_v1"
                if profile == "legacy"
                else "fresh_rejected_episode_consumed_v1",
                max_price_raw=cfg["max_price_raw"],
                numerical_begin="0",
                numerical_end=str(len(bars)),
                observer_begin="4",
                observer_end=str(len(bars)),
                funding_key=str(begin - 1),
                evaluation_begin=str(begin),
                evaluation_end=str(len(bars)),
                terminal=cfg["terminal"],
                dividend_policy="ex_date_cash_compatibility_v1",
            ),
            "input/scope/rejection identity",
        )
        cost = integer(cfg["commission_bps"])
        archive_available = not (profile == "legacy" and cost == 20)
        prefix = (
            (f"legacy_c{cost}" if profile == "legacy" else f"SPY_B0_c{cost}")
            if archive_available
            else None
        )
        drift = (
            ref.compare_archive(
                results[0],
                original,
                archive / f"results/{prefix}_fills.csv",
                archive / f"results/{prefix}_equity.csv",
            )
            if archive_available
            else None
        )
        archive_note = (
            None
            if archive_available
            else "Legacy 20+20 bps is a native-policy diagnostic; no original archived float baseline exists."
        )
        after = {name: digest(output / name, limit) for name, limit in NATIVE_FILES.items()}
        same(after, before, "native bundle changed during read-only verification")
        write_report(
            drift_path,
            dict(
                format="s3-native-float-reconciliation-v1",
                native_output=str(output),
                reference_prefix=prefix,
                difference_unit="dollars/units; exact Fraction text",
                native_verified=True,
                archive_reference_available=archive_available,
                archive_reference_note=archive_note,
                rows=drift,
            ),
            protected,
        )
        report = dict(
            format="s3-native-verification-v1",
            native_verified=True,
            cli_acceptance_observed=acceptance_observed,
            native_output=str(output),
            config=str(config),
            dataset=str(dataset),
            research_bundle=str(archive),
            dataset_manifest_sha256=manifest_hash,
            archive_manifest_sha256=ref.MANIFEST_SHA,
            oracle_sha256=digest(Path(ref.__file__).resolve(), MB),
            verifier_sha256=digest(Path(__file__).resolve(), MB),
            artifact_sha256=before,
            profile=cfg["profile"],
            cost_each_bps=cost,
            numerical_sessions=len(bars),
            evaluation_sessions=len(bars) - begin,
            funding_rows_per_trial=1,
            s3_close_rows=len(results[0]["equity"]),
            decision_rows=3 * len(bars),
            trace_rows=len(actual_trace),
            financial_rows=len(actual_events),
            s3_fills=len(results[0]["fills"]),
            decision_changes=decision_changes,
            s3_trade_dates=[
                dict(
                    date=f["date"],
                    signal_date=f["signal_date"],
                    side=f["side"],
                    reason=f["decision_reason"],
                )
                for f in results[0]["fills"]
            ],
            archive_reference_available=archive_available,
            archive_reference_note=archive_note,
            final_equity_delta_from_archive=drift["equity"][-1]["equity_delta"]
            if archive_available
            else None,
            reconciliation=str(drift_path),
            summary=expected_summary,
        )
        write_report(report_path, report, protected)
        print(
            f"S3_NATIVE_EXACT_PASS profile={profile} cost_each={cost} financial_rows={len(actual_events)} "
            f"trace_rows={len(actual_trace)} s3_fills={len(results[0]['fills'])} "
            f"archive_reference_available={str(archive_available).lower()} report={report_path}"
        )
        return report
    except Exception as error:
        if not report_path.exists():
            write_report(
                report_path,
                dict(
                    format="s3-native-verification-v1",
                    native_verified=False,
                    native_output=str(output),
                    error=str(error)[:8000],
                ),
                protected,
                MB,
            )
        raise


def synthetic_checks():
    checks = ref.self_test()
    bars = [
        ref.Bar(f"2000-01-0{i + 1}", p * ref.P, p * ref.P, p * ref.P, p * ref.P, d * ref.P)
        for i, (p, d) in enumerate(((100, 0), (100, 0), (100, 1), (110, 1)))
    ]
    control = control_reference(bars, 1, True, "fresh", 1000000, 1000000, 0, 0)
    same(control["fills"][0]["key"], 2, "fresh control following-first-close execution")
    same(control["fills"][0]["quantity_raw"], 95000000, "hand control quantity")
    same(control["events"][-1]["equity_cents"], 1104500, "hand marked control PnL")
    same(control["events"][-1]["dividend_cents"], 9500, "hand control dividend entitlement")
    terminal = control_reference(bars, 1, True, "legacy", 1000000, 1000000, 0, 0)
    same(terminal["fills"][0]["key"], 1, "legacy control funding-close execution")
    same(terminal["events"][-1]["equity_cents"], 1114000, "hand synthetic control PnL")
    mapped = [native_event(e, 3) for e in terminal["events"]]
    same(sum(r is None for r in mapped), 1, "terminal has no separate native accepted event")
    sale = next(r for r in mapped if r and r[3] == "terminal")
    same(sale[12], 2, "synthetic terminal uses Close phase")
    same(
        sale[17:], [None, None, "none", 0], "synthetic terminal has no fabricated order eligibility"
    )
    same(len(sale), 21, "native financial field width")
    print(
        f"S3_REFERENCE_SELF_TEST_PASS oracle_groups={len(checks)} control_adapter_groups=2 native_verified=false"
    )


def harness_root(scratch, protected):
    scratch = scratch.resolve()
    assert scratch.is_dir(), "--scratch must name an existing harness parent directory"
    assert all(not scratch.is_relative_to(p.resolve()) for p in protected), (
        "scratch is inside a protected bundle"
    )
    return Path(tempfile.mkdtemp(prefix="stochastic-verify.", dir=scratch)).resolve()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--research-bundle", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--scratch", type=Path)
    parser.add_argument("--synthetic-only", action="store_true")
    args = parser.parse_args()
    if args.synthetic_only:
        if args.runner or args.output or args.config:
            parser.error("--synthetic-only does not consume native runs")
        synthetic_checks()
        return
    if args.dataset is None:
        parser.error("--dataset is required")
    dataset, archive = args.dataset.resolve(), args.research_bundle.resolve()
    ref.verify_archive(archive)
    if args.runner:
        if args.config or args.output or args.scratch is None:
            parser.error("suite mode requires --scratch and uses its own five configs/outputs")
        root = harness_root(args.scratch, (dataset, archive))
        reports = []
        runner = args.runner.resolve()
        runner_hash = digest(runner, 256 * MB)
        print(f"S3_NATIVE_SUITE_START harness={root}")
        try:
            for profile, costs in (("legacy", (5, 10)), ("fresh", (5, 10, 20))):
                for cost in costs:
                    label = f"{profile}_c{cost}"
                    config = root / f"{label}.xml"
                    tree = ET.parse(ROOT / f"examples/backtest/stochastic-{profile}.xml")
                    tree.getroot().attrib.update(commission_bps=str(cost), slippage_bps=str(cost))
                    with config.open("xb") as stream:
                        tree.write(stream, encoding="utf-8", xml_declaration=True)
                    output = root / f"{label}.native"
                    command = [
                        str(runner),
                        "run",
                        "--config",
                        str(config),
                        "--dataset",
                        str(dataset),
                        "--out",
                        str(output),
                    ]
                    process = subprocess.run(command, capture_output=True, text=True, timeout=3600)
                    write_report(
                        root / f"{label}.cli.json",
                        dict(
                            command=command,
                            returncode=process.returncode,
                            stdout=process.stdout,
                            stderr=process.stderr,
                            runner_sha256=runner_hash,
                        ),
                        (dataset, archive, output),
                        4 * MB,
                    )
                    assert process.returncode == 0 and any(
                        line.startswith("RUN_ACCEPTED ") for line in process.stdout.splitlines()
                    ), f"native CLI did not accept {label}; see {root / (label + '.cli.json')}"
                    reports.append(verify(dataset, config, output, archive, root, label, True))
            same(digest(runner, 256 * MB), runner_hash, "native executable changed during suite")
            ref.verify_archive(archive)
            write_report(
                root / "suite-verification.json",
                dict(
                    native_verified=True,
                    runs=len(reports),
                    runner=str(runner),
                    runner_sha256=runner_hash,
                    reports=[
                        str(
                            root
                            / f"{r['profile'].split('_')[0]}_c{r['cost_each_bps']}.verification.json"
                        )
                        for r in reports
                    ],
                ),
                (dataset, archive),
                MB,
            )
            print(f"S3_NATIVE_HISTORICAL_SUITE_PASS runs=5 evidence={root}")
        except Exception as error:
            write_report(
                root / "suite-failure.json",
                dict(native_verified=False, completed_runs=len(reports), error=str(error)[:8000]),
                (dataset, archive),
                MB,
            )
            raise
    else:
        if args.output is None or args.config is None:
            parser.error("existing-run mode requires --config and --output")
        output, config = args.output.resolve(), args.config.resolve()
        root = harness_root(args.scratch or output.parent, (dataset, archive, output))
        print(f"S3_NATIVE_EXISTING_RUN_VERIFY harness={root}")
        verify(dataset, config, output, archive, root, "existing", False)
        ref.verify_archive(archive)


if __name__ == "__main__":
    main()
