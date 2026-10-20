#!/usr/bin/env python3
"""Independent integer/Fraction S2 oracle and financial equivalence gate.

Usage: verify_keltner.py --dataset BUNDLE --config XML --output RUN
       [--compare OTHER_RUN ...]

No native indicator, sizing, strategy or accounting code is called by the oracle.
Only the independent exact settlement helpers are shared. CSV field
translation lives in check_keltner_csv/check_producers; the mathematical reference
and host-event reconstruction do not depend on the runner's representation.

Expected keltner.csv schema (CSV integers; missing values are empty): trial,key,
kind,may_submit,funded,close_raw,tr_raw,ema_raw,atr_raw,upper_raw,action,reason,
quantity_raw,stop_distance_raw,held,threshold_raw,highest_close_raw. Kinds are
decision/execution. Decisions capture state after the close policy, before host
risk override; executions capture state after the callback with posted account.
Execution feature/action/context fields are empty. Optional execution fields are
id,rejected,rejection,side,price_raw; optional account fields cash_cents,units_raw
apply to both kinds, and latched applies to decisions. Stop distance is the frozen
entry distance; exit distance is empty. Rejections retain the requested quantity.
Execution reason is the numeric RejectionReason or the filled intent reason.

producers.csv uses the engine's id,physical,advances,publications,aliases,waiting,
cached_deliveries fields, including source and aliases. graph.xml supplies operation
identities for exact per-producer readiness/advance/publication checks. summary.json
uses the existing S1 financial fields for six accounts. Equivalence ignores native
experiment/configuration header hashes, but compares every host event and all
feature/decision/execution rows. Each compared run must also pass the exact oracle.
identities.csv contains id,digest for every graph node. Synthetic cache checks
change the recurrence origin and the retained price succession independently,
while retaining a full increasing dated grid and staying below ReuseMaxSourceRows512.
"""

import argparse
import csv
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from verify_reference import affordable, amounts, even, execution

WIDTHS = ((3, 2), (2, 1), (5, 2), (2, 1))


def same(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: actual={actual!r}; expected={expected!r}")


def sequence_equal(actual, expected, label):
    same(len(actual), len(expected), f"{label} row count")
    for index, (got, wanted) in enumerate(zip(actual, expected)):
        same(got, wanted, f"{label} row {index}")


def read_csv(path):
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        fields = reader.fieldnames
        assert fields and len(set(fields)) == len(fields), f"invalid CSV header: {path}"
        rows = list(reader)
        assert all(None not in row and None not in row.values() for row in rows), (
            f"ragged CSV: {path}"
        )
        return fields, rows


def inputs(dataset, config):
    cfg = ET.parse(config).getroot().attrib
    for field, value in dict(
        strategy="keltner_atr_risk_v1",
        profile="fresh_research_v1",
        ema_window="20",
        atr_window="10",
        channel_widths="3/2,2/1,5/2,2/1",
        allocation_bps="9500",
        drawdown_bps="2000",
        terminal="mark",
        controls="cash,buy_hold",
    ).items():
        same(cfg[field], value, f"accepted S2 contract {field}")
    _, all_rows = read_csv(dataset / "prices.csv")
    rows = [r for r in all_rows if cfg["numerical_origin"] <= r["Date"] <= cfg["evaluation_end"]]
    assert (
        rows
        and rows[0]["Date"] == cfg["numerical_origin"]
        and rows[-1]["Date"] == cfg["evaluation_end"]
    )
    dates = [r["Date"] for r in rows]
    assert dates == sorted(set(dates)), "duplicate/out-of-order input sessions"
    begin = dates.index(cfg["evaluation_begin"])
    assert 0 < begin < len(rows), "fresh profile needs its funding close"
    bars = [
        {
            name.lower(): even(Fraction(row[name]) * 10**9)
            for name in ("Open", "High", "Low", "Close", "Dividend")
        }
        for row in rows
    ]
    for bar in bars:
        assert (
            0
            < bar["low"]
            <= min(bar["open"], bar["close"])
            <= max(bar["open"], bar["close"])
            <= bar["high"]
        )
        assert bar["dividend"] >= 0
    return cfg, bars, begin


def numerical(bars):
    """Recurrences round each complete weighted numerator exactly once."""
    result, closes, ranges = [], [], []
    ema = atr = None
    for key, bar in enumerate(bars):
        close = bar["close"]
        tr = bar["high"] - bar["low"]
        if key:
            tr = max(tr, abs(bar["high"] - closes[-1]), abs(bar["low"] - closes[-1]))
        closes.append(close)
        ranges.append(tr)
        if key == 19:
            ema = even(Fraction(sum(closes), 20))
        elif key > 19:
            ema = even(Fraction(2 * close + 19 * ema, 21))
        if key == 9:
            atr = even(Fraction(sum(ranges), 10))
        elif key > 9:
            atr = even(Fraction(9 * atr + tr, 10))
        upper = [
            None if ema is None else even(Fraction(den * ema + num * atr, den))
            for num, den in WIDTHS
        ]
        result.append(dict(close_raw=close, tr_raw=tr, ema_raw=ema, atr_raw=atr, uppers=upper))
    return result


def sized_quantity(cash, close, atr, lot, commission, slippage):
    assert cash >= 0 and atr > 0 and lot > 0
    risk_lots = cash * 10**15 // (40000 * atr * lot)
    cap_quantity = affordable(execution(close, "buy", slippage), cash * 95 // 100, lot, commission)
    return min(risk_lots * lot, cap_quantity)


def reference(bars, begin, cfg):
    initial, lot, commission, slippage = (
        int(cfg[k])
        for k in ("initial_cash_cents", "lot_step_raw", "commission_bps", "slippage_bps")
    )
    assert initial > 0 and lot > 0 and 0 <= commission < 10000 and 0 <= slippage < 10000
    numbers = numerical(bars)
    accounts = [
        dict(
            cash=initial,
            units=0,
            basis=0,
            realized=0,
            dividends=0,
            fees=0,
            next_id=1,
            pending=None,
            peak=initial,
            dd=Fraction(0),
            dd_loss=0,
            dd_peak=initial,
            latched=False,
            threshold=None,
            highest=None,
        )
        for _ in range(6)
    ]
    events, decisions, receipts = [], [], []

    def equity(a, mark):
        return a["cash"] + even(Fraction(mark * a["units"], 10**13))

    def emit(trial, key, kind, reason, mark=None, phase=None, order=None, fill=None):
        a = accounts[trial]
        p, q, gross, fee = (
            fill
            if fill
            else (
                mark or 0,
                order["quantity"] if order and order["policy"] == "fixed_quantity" else 0,
                0,
                0,
            )
        )
        value = None if mark is None else equity(a, mark)
        events.append(
            [
                trial + 1,
                key,
                order["id"] if order else 0,
                kind,
                reason,
                a["cash"],
                a["units"],
                a["basis"],
                a["realized"],
                a["dividends"],
                a["fees"],
                value,
                phase,
                p,
                q,
                gross,
                fee,
                order["decision"] if order else None,
                order["eligible"] if order else None,
                order["policy"] if order else "none",
                order["budget"] if order else 0,
            ]
        )
        assert a["cash"] >= 0 and a["units"] >= 0 and a["units"] % lot == 0
        if value is not None:
            same(
                value - initial,
                a["realized"] + value - a["cash"] - a["basis"] + a["dividends"],
                "PnL identity",
            )

    def receipt(trial, key, order, rejection, fill=None):
        a = accounts[trial]
        receipts.append(
            dict(
                trial=trial + 1,
                key=key,
                kind="execution",
                id=order["id"],
                reason=str(rejection) if rejection is not None else order["reason"],
                rejected=int(rejection is not None),
                rejection=rejection,
                side=None if fill is None else ("sell" if order["policy"] == "sell_all" else "buy"),
                price_raw=None if fill is None else fill[0],
                quantity_raw=order["quantity"] if fill is None else fill[1],
                stop_distance_raw=order["distance"],
                held=int(a["units"] > 0),
                threshold_raw=a["threshold"],
                highest_close_raw=a["highest"],
                cash_cents=a["cash"],
                units_raw=a["units"],
            )
        )

    for key, bar in enumerate(bars):
        if key >= begin:
            for trial, a in enumerate(accounts):
                payment = even(Fraction(a["units"] * bar["dividend"], 10**13))
                a["cash"] += payment
                a["dividends"] += payment
                emit(trial, key, "dividend", "ex_date_cash", bars[key - 1]["close"], 0)
                order = a["pending"]
                if order is None:
                    continue
                buying = order["policy"] != "sell_all"
                p = execution(bar["open"], "buy" if buying else "sell", slippage)
                q = (
                    affordable(p, order["budget"], lot, commission)
                    if order["policy"] == "cash_budget"
                    else order["quantity"]
                    if buying
                    else a["units"]
                )
                gross, fee = amounts(p, q, commission)
                rejection = (
                    0
                    if q == 0
                    else 7
                    if buying and order["distance"] is not None and p <= order["distance"]
                    else 1
                    if buying and gross + fee > a["cash"]
                    else 2
                    if buying and gross + fee > a["cash"] * 95 // 100
                    else None
                )
                if rejection is not None:
                    emit(trial, key, "rejected", str(rejection), order=order)
                else:
                    fill = (p, q, gross, fee)
                    emit(trial, key, "accepted", order["reason"], bar["open"], 1, order, fill)
                    if buying:
                        assert a["units"] == 0
                        a["cash"] -= gross + fee
                        a["units"], a["basis"] = q, gross + fee
                        if trial < 4:
                            a["threshold"], a["highest"] = p - order["distance"], None
                            assert a["threshold"] > 0
                    else:
                        a["cash"] += gross - fee
                        a["realized"] += gross - fee - a["basis"]
                        a["units"], a["basis"] = 0, 0
                        a["threshold"], a["highest"] = None, None
                    a["fees"] += fee
                    emit(trial, key, "filled", order["reason"], bar["open"], 1, order, fill)
                if trial < 4:
                    receipt(trial, key, order, rejection, None if rejection is not None else fill)
                a["pending"] = None
        for trial, a in enumerate(accounts):
            if key == begin - 1:
                emit(trial, key, "funded", "initial_cash", bar["close"], 2)
            trigger = False
            if key >= begin:
                value = equity(a, bar["close"])
                a["peak"] = max(a["peak"], value)
                loss = a["peak"] - value
                dd = Fraction(loss, a["peak"])
                if dd > a["dd"]:
                    a.update(dd=dd, dd_loss=loss, dd_peak=a["peak"])
                if trial < 4 and not a["latched"] and dd >= Fraction(1, 5):
                    a["latched"], trigger = True, True
            if key >= begin - 1:
                emit(trial, key, "close", "ordinary", bar["close"], 2)
                if trigger:
                    emit(trial, key, "risk", "1", bar["close"], 2)
            quantity, distance = 0, None
            if trial < 4:
                n = numbers[key]
                upper, ema, atr = n["uppers"][trial], n["ema_raw"], n["atr_raw"]
                action, reason = 0, "warming"
                if upper is not None:
                    reason = "warmup"
                    if key >= begin:
                        if a["units"]:
                            a["highest"] = max(bar["close"], a["highest"] or bar["close"])
                            a["threshold"] = max(a["threshold"], a["highest"] - 2 * atr)
                            if bar["close"] <= a["threshold"]:
                                action, reason = 2, "protective_close"
                            elif bar["close"] < ema:
                                action, reason = 2, "below_ema"
                            else:
                                reason = "held"
                        else:
                            previous_upper = numbers[key - 1]["uppers"][trial] if key else None
                            crossing = (
                                previous_upper is not None
                                and bars[key - 1]["close"] <= previous_upper
                                and bar["close"] > upper
                            )
                            reason = (
                                "no_crossing"
                                if not crossing
                                else "zero_atr"
                                if not atr
                                else "keltner_crossing"
                            )
                            if crossing and atr:
                                action, distance = 3, 2 * atr
                                quantity = sized_quantity(
                                    a["cash"], bar["close"], atr, lot, commission, slippage
                                )
                decisions.append(
                    dict(
                        trial=trial + 1,
                        key=key,
                        kind="decision",
                        may_submit=int(key >= begin),
                        funded=int(key >= begin - 1),
                        close_raw=n["close_raw"],
                        tr_raw=n["tr_raw"],
                        ema_raw=ema,
                        atr_raw=atr,
                        upper_raw=upper,
                        action=action,
                        reason=reason,
                        quantity_raw=quantity,
                        stop_distance_raw=distance,
                        held=int(a["units"] > 0),
                        threshold_raw=a["threshold"],
                        highest_close_raw=a["highest"],
                        cash_cents=a["cash"] if key >= begin - 1 else None,
                        units_raw=a["units"] if key >= begin - 1 else None,
                        latched=int(a["latched"]),
                    )
                )
            else:
                action, reason = (
                    (1, "buy_hold_control")
                    if trial == 5 and key == begin
                    else (0, "cash_control" if trial == 4 else "buy_hold_control")
                )
            if key < begin:
                continue
            if a["latched"] and a["units"]:
                action, reason = 2, "risk_exit"
            elif a["latched"] and action in (1, 3):
                emit(trial, key, "suppressed", "risk_latched")
                continue
            if (action in (1, 3) and not a["units"]) or (action == 2 and a["units"]):
                order = dict(
                    id=a["next_id"],
                    decision=key,
                    eligible=key + 1,
                    reason=reason,
                    policy="cash_budget"
                    if action == 1
                    else "fixed_quantity"
                    if action == 3
                    else "sell_all",
                    budget=a["cash"] * 95 // 100 if action == 1 else 0,
                    quantity=quantity if action == 3 else 0,
                    distance=distance if action == 3 else None,
                )
                a["next_id"] += 1
                a["pending"] = order
                emit(trial, key, "submitted", reason, order=order)
    for trial, a in enumerate(accounts):
        if a["pending"]:
            emit(trial, len(bars) - 1, "expired", "end_of_scope", order=a["pending"])
            a["pending"] = None
        emit(trial, len(bars) - 1, "final", "endpoint", bars[-1]["close"], 2)
    return numbers, decisions, receipts, events, accounts


def financial_rows(path):
    lines = path.read_text().splitlines()
    same(lines[0], "quadeplus_financial_ledger_v1", "financial format")
    assert "financial_state=completed" in lines, "financial evidence is partial"
    rows = []
    for line in lines:
        if not line or not line[0].isdigit():
            continue
        fields = shlex.split(line)
        same(len(fields), 21, "host-v1 field count")
        rows.append(
            [
                v if i in (3, 4, 19) else None if v == "none" else int(v)
                for i, v in enumerate(fields)
            ]
        )
    return rows


def check_keltner_csv(path, decisions, receipts):
    """Strict schema adapter; unknown columns must acquire explicit semantics."""
    fields, rows = read_csv(path)
    aliases = {
        "trial_id": "trial",
        "event": "kind",
        "initial_stop_distance_raw": "stop_distance_raw",
        "stop_raw": "threshold_raw",
        "highest_raw": "highest_close_raw",
        "executed_price_raw": "price_raw",
    }
    names = [aliases.get(f, f) for f in fields]
    assert len(set(names)) == len(names), "aliased duplicate Keltner CSV field"
    required = {
        "trial",
        "key",
        "kind",
        "may_submit",
        "funded",
        "close_raw",
        "tr_raw",
        "ema_raw",
        "atr_raw",
        "upper_raw",
        "action",
        "reason",
        "quantity_raw",
        "stop_distance_raw",
        "held",
        "threshold_raw",
        "highest_close_raw",
    }
    assert required <= set(names), f"Keltner schema missing {sorted(required - set(names))}"
    supported = set().union(*(r.keys() for r in decisions + receipts))
    assert set(names) <= supported, f"Keltner schema unknown {sorted(set(names) - supported)}"
    grouped = {"decision": [], "execution": []}
    causal_order = []
    for row in rows:
        canonical = {aliases.get(k, k): v for k, v in row.items()}
        kind = canonical["kind"]
        assert kind in grouped, f"unknown Keltner row kind {kind}"
        grouped[kind].append(canonical)
        causal_order.append(
            (int(canonical["key"]), 0 if kind == "execution" else 1, int(canonical["trial"]))
        )
    same(causal_order, sorted(causal_order), "execution-before-close evidence ordering")
    for kind, expected in (("decision", decisions), ("execution", receipts)):
        wanted = [
            {name: "" if row.get(name) is None else str(row[name]) for name in names}
            for row in expected
        ]
        sequence_equal(grouped[kind], wanted, f"Keltner {kind}")
    return rows


def check_summary(path, dataset, bars, begin, accounts, initial):
    summary = json.loads(path.read_text())
    same(summary["financial_state"], "completed", "summary completion")
    same(summary["evaluation_sessions"], len(bars) - begin, "evaluation scope")
    same(
        summary["dataset_sha256"],
        hashlib.sha256((dataset / "manifest.qdaily").read_bytes()).hexdigest(),
        "dataset identity",
    )
    same(len(summary["trials"]), 6, "independent trial count")
    for trial, (reported, a) in enumerate(zip(summary["trials"], accounts), 1):
        equity = a["cash"] + even(Fraction(a["units"] * bars[-1]["close"], 10**13))
        wanted = dict(
            id=trial,
            cash_cents=a["cash"],
            units_raw=a["units"],
            equity_cents=equity,
            pnl_cents=equity - initial,
            realized_cents=a["realized"],
            unrealized_cents=equity - a["cash"] - a["basis"],
            dividend_cents=a["dividends"],
            fee_cents=a["fees"],
            drawdown_loss_cents=a["dd_loss"],
            drawdown_peak_cents=a["dd_peak"],
        )
        for field, value in wanted.items():
            same(reported[field], value, f"summary trial {trial} {field}")
        if "risk_latched" in reported:
            same(reported["risk_latched"], a["latched"], f"summary trial {trial} risk latch")
    return summary


def check_producers(path, count, cfg):
    fields, rows = read_csv(path)
    required = {
        "id",
        "physical",
        "advances",
        "publications",
        "aliases",
        "waiting",
        "cached_deliveries",
    }
    same(set(fields), required, "producer statistics schema")
    assert rows and len({r["id"] for r in rows}) == len(rows), "empty/duplicate producer inventory"
    graph = ET.parse(path.parent / "graph.xml").getroot()
    operations = {
        node.attrib["id"]: node.attrib["operation"]
        for node in list(graph.findall("sources/source")) + list(graph.findall("nodes/node"))
    }
    same(set(operations), {r["id"] for r in rows}, "producer inventory versus declared graph")
    readiness = {
        "quadeplus.daily_research": (0, 0),
        "qaedalus.bar_close": (0, 0),
        "quadeplus.true_range": (0, 0),
        "quadeplus.ema20": (19, 0),
        "quadeplus.atr10": (9, 0),
        "quadeplus.upper_channel": (19, 19),
    }
    physical = set()
    aliases = 0
    for row in rows:
        for name in required - {"id", "physical"}:
            value = int(row[name])
            assert 0 <= value <= count, (row["id"], name, value, count)
        values = {k: int(row[k]) for k in required - {"id"}}
        operation = operations[row["id"]]
        assert operation in readiness, f"unknown numerical operation {operation}"
        missing, waiting = readiness[operation]
        if values["aliases"]:
            aliases += 1
            same(values["aliases"], count, f"{row['id']} aliases")
            same(
                sum(
                    values[k] for k in ("advances", "publications", "waiting", "cached_deliveries")
                ),
                0,
                "alias executed numerical work",
            )
            assert values["physical"] in physical, "alias precedes or lacks its physical producer"
        else:
            assert values["physical"] not in physical, "physical producer repeated without alias"
            physical.add(values["physical"])
            same(
                values["publications"],
                max(0, count - missing),
                f"{row['id']} publications/readiness",
            )
            same(values["waiting"], min(count, waiting), f"{row['id']} input warming")
            cached = values["cached_deliveries"]
            if cached:
                same(cached, count, "reuse must cover the complete origin scope including warming")
                same(values["advances"], 0, "cached producer recomputed")
                assert cfg["cache"] != "disabled" and operation != "quadeplus.daily_research"
            else:
                same(
                    values["advances"],
                    count - min(count, waiting),
                    f"{row['id']} producer advances",
                )
    if cfg["sharing"] == "disabled":
        same(aliases, 0, "sharing-disabled aliases")
    else:
        assert aliases > 0, "exact-sharing run did not expose any aliases"
    return rows


def check_identities(path, producers):
    fields, rows = read_csv(path)
    same(fields, ["id", "digest"], "canonical identity schema")
    identities = {row["id"]: row["digest"] for row in rows}
    same(len(identities), len(rows), "unique canonical identity rows")
    same(set(identities), {row["id"] for row in producers}, "complete canonical identity inventory")
    for name, digest in identities.items():
        assert len(digest) == 64 and all(c in "0123456789abcdef" for c in digest), (name, digest)
    for operation in ("ema20", "true_range", "atr10"):
        for trial in range(1, 4):
            same(
                identities[f"t{trial}_{operation}"],
                identities[f"t0_{operation}"],
                f"identical numerical {operation} identities",
            )
    same(
        identities["t1_upper_channel"],
        identities["t3_upper_channel"],
        "duplicate 2/1 channel identity",
    )
    same(
        len({identities[f"t{i}_upper_channel"] for i in range(3)}),
        3,
        "fixed widths must have distinct identities",
    )
    return identities


def check_separate_trial_ledgers(events):
    same({row[0] for row in events}, set(range(1, 7)), "separate financial trial identities")
    for trial in range(1, 7):
        rows = [row for row in events if row[0] == trial]
        same(sum(row[3] == "funded" for row in rows), 1, f"trial {trial} separate funding")
        same(sum(row[3] == "final" for row in rows), 1, f"trial {trial} separate endpoint")
        submitted = [row[2] for row in rows if row[3] == "submitted"]
        same(submitted, list(range(1, len(submitted) + 1)), f"trial {trial} local order IDs")
        resolved = [row[2] for row in rows if row[3] in ("filled", "rejected", "expired")]
        same(resolved, submitted, f"trial {trial} resolves each own order exactly once")
    sequence_equal(
        [r[1:] for r in events if r[0] == 2],
        [r[1:] for r in events if r[0] == 4],
        "duplicate 2/1 accounts",
    )


def verify(dataset, config, output):
    cfg, bars, begin = inputs(dataset, config)
    _, decisions, receipts, expected, accounts = reference(bars, begin, cfg)
    events = financial_rows(output / "financial.txt")
    sequence_equal(events, expected, "financial")
    keltner = check_keltner_csv(output / "keltner.csv", decisions, receipts)
    summary = check_summary(
        output / "summary.json", dataset, bars, begin, accounts, int(cfg["initial_cash_cents"])
    )
    producers = check_producers(output / "producers.csv", len(bars), cfg)
    identities = check_identities(output / "identities.csv", producers)
    check_separate_trial_ledgers(events)
    _, prices = read_csv(dataset / "prices.csv")
    dates = [
        row["Date"]
        for row in prices
        if cfg["numerical_origin"] <= row["Date"] <= cfg["evaluation_end"]
    ]
    print(
        f"S2_EXACT_ORACLE_PASS decisions={len(decisions)} executions={len(receipts)} financial_rows={len(events)} trials=6 output={output}"
    )
    return dict(
        events=events,
        keltner=keltner,
        summary=summary,
        producers=producers,
        config=cfg,
        identities=identities,
        dates=dates,
    )


def equivalent(left, right):
    operational = {"sharing", "cache", "cache_directory"}
    same(
        {k: v for k, v in left["config"].items() if k not in operational},
        {k: v for k, v in right["config"].items() if k not in operational},
        "equivalence financial configuration",
    )
    sequence_equal(left["events"], right["events"], "equivalent financial events")
    sequence_equal(left["keltner"], right["keltner"], "equivalent Keltner evidence")
    same(left["summary"]["trials"], right["summary"]["trials"], "equivalent trial summaries")
    same(left["identities"], right["identities"], "equivalent numerical identities")

    def totals(result):
        return {
            k: sum(int(row[k]) for row in result["producers"])
            for k in ("advances", "publications", "aliases", "waiting", "cached_deliveries")
        }

    print(f"S2_EQUIVALENCE_PASS left_counters={totals(left)} right_counters={totals(right)}")


def execute(command):
    result = subprocess.run([str(arg) for arg in command], text=True, capture_output=True)
    assert result.returncode == 0, f"command failed: {command}\n{result.stdout}\n{result.stderr}"
    return result


def run_fixture(runner, case, config, label):
    path = case / f"{label}.xml"
    config.write(path, encoding="unicode")
    output = case / label
    result = execute(
        [runner, "run", "--config", path, "--dataset", case / "dataset", "--out", output]
    )
    assert "RUN_ACCEPTED" in result.stdout, result.stdout
    # Optional delivered benchmark projection, outside the native accepted bundle.
    (case / f"{label}.stdout.txt").write_text(result.stdout)
    timing = next(line for line in result.stdout.splitlines() if line.startswith("TIMING_US "))
    print(f"S2_TIMING case={case.name} mode={label} {timing}")
    return verify(case / "dataset", path, output)


def cached_deliveries(result):
    return sum(int(row["cached_deliveries"]) for row in result["producers"])


def changed_source_identity(original, changed, label):
    same(
        {k: v for k, v in original["config"].items() if k != "numerical_origin"},
        {k: v for k, v in changed["config"].items() if k != "numerical_origin"},
        f"{label} preserves financial/numerical parameters and cache directory",
    )
    same(
        original["config"]["evaluation_begin"],
        changed["config"]["evaluation_begin"],
        f"{label} evaluation start",
    )
    same(
        original["config"]["evaluation_end"],
        changed["config"]["evaluation_end"],
        f"{label} evaluation end",
    )
    assert original["summary"]["dataset_sha256"] != changed["summary"]["dataset_sha256"], (
        f"{label}: unchanged dataset identity"
    )
    same(set(original["identities"]), set(changed["identities"]), f"{label} graph inventory")
    for node in original["identities"]:
        assert original["identities"][node] != changed["identities"][node], (
            f"{label}: unchanged source-dependent identity {node}"
        )
    same(cached_deliveries(changed), 0, f"{label} must miss prior source cache")

    # Compare actual native publications at the same date, not local sequence keys.
    def at_evaluation(result):
        key = result["dates"].index(result["config"]["evaluation_begin"])
        return next(
            row
            for row in result["keltner"]
            if row["kind"] == "decision" and row["trial"] == "1" and int(row["key"]) == key
        )

    before, after = at_evaluation(original), at_evaluation(changed)
    for field in ("ema_raw", "atr_raw"):
        assert before[field] and after[field] and before[field] != after[field], (
            f"{label}: no common-date {field} recurrence change"
        )
    print(
        f"S2_IDENTITY_INVALIDATION_PASS change={label} nodes={len(changed['identities'])} "
        f"cached_deliveries=0 common_date={changed['config']['evaluation_begin']}"
    )


def source_identity_cases(runner, fixture, template, root):
    cache = root / "source-identity-cache"
    cache.mkdir()
    original = None
    original_case = root / "source-original"
    for label, mode, origin_index in (
        ("original", "trend", 0),
        ("shifted-origin", "trend", 25),
        ("reordered-prices", "reordered", 0),
    ):
        case = root / f"source-{label}"
        execute([fixture, "--fixture", case, mode, 60, origin_index])
        fields, prices = read_csv(case / "dataset/prices.csv")
        same(len(prices), 160, "retain full source CSV despite selected origin")
        same(
            (case / "sessions.csv").read_bytes(),
            (case / "dataset/sessions.csv").read_bytes(),
            "retain full source grid",
        )
        assert len(prices) <= 512, "identity/reuse fixture exceeds native snapshot ceiling"
        config = ET.parse(template)
        config.getroot().attrib.update(
            symbol="TEST",
            numerical_origin=prices[origin_index]["Date"],
            evaluation_begin=prices[60]["Date"],
            evaluation_end=prices[-1]["Date"],
            initial_cash_cents="1000000",
            sharing="exact",
            cache="automatic_read_write",
            cache_directory=str(cache),
        )
        if label != "original":
            same(
                (case / "dataset/sessions.csv").read_bytes(),
                (original_case / "dataset/sessions.csv").read_bytes(),
                "variants retain identical dated grid",
            )
            _, before_prices = read_csv(original_case / "dataset/prices.csv")
            same(
                [r["Date"] for r in prices],
                [r["Date"] for r in before_prices],
                "variants preserve increasing dates",
            )
            if label == "shifted-origin":
                same(
                    (case / "dataset/prices.csv").read_bytes(),
                    (original_case / "dataset/prices.csv").read_bytes(),
                    "origin-only change retains identical original prices",
                )
            else:

                def tuples(rows):
                    return [tuple(row[f] for f in fields if f != "Date") for row in rows]

                same(
                    sorted(tuples(prices)),
                    sorted(tuples(before_prices)),
                    "price-order variant preserves tuple multiset",
                )
                assert tuples(prices) != tuples(before_prices), (
                    "price-order fixture failed to change succession"
                )
        result = run_fixture(runner, case, config, "cache-origin")
        if original is None:
            same(cached_deliveries(result), 0, "new identity cache starts empty")
            original = result
        else:
            changed_source_identity(original, result, label)
        # A genuine cold miss followed by an exact warm hit proves the variant
        # remained cache-eligible rather than accidentally disabling reuse.
        reused = run_fixture(runner, case, config, "cache-reused")
        equivalent(result, reused)
        assert cached_deliveries(reused) > 0, f"{label}: no repeat-run reuse"
    # Different source identities coexist; invalidation must not overwrite the
    # accepted original or accidentally bind its later lookup to a variant.
    original_config = ET.parse(original_case / "cache-origin.xml")
    restored = run_fixture(runner, original_case, original_config, "cache-after-variants")
    equivalent(original, restored)
    assert cached_deliveries(restored) > 0, "original lost reusable evidence after source variants"


def synthetic_suite(runner, fixture, template, scratch):
    """Generate immutable authored inputs, then inspect actual native output."""
    scratch.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="keltner-oracle.", dir=scratch)).resolve()
    execute([fixture, root])
    coverage = set()
    for index, (mode, begin, initial) in enumerate(
        (
            ("trend", 1, 1000000),
            ("flat", 1, 1000000),
            ("trend", 60, 1000000),
            ("trend", 1, 1),
            ("gap", 1, 1000000),
            ("crash", 1, 1000000),
        )
    ):
        case = root / f"case{index}-{mode}"
        execute([fixture, "--fixture", case, mode, begin])
        _, prices = read_csv(case / "prices.csv")
        config = ET.parse(template)
        config.getroot().attrib.update(
            symbol="TEST",
            numerical_origin=prices[0]["Date"],
            evaluation_begin=prices[begin]["Date"],
            evaluation_end=prices[-1]["Date"],
            initial_cash_cents=str(initial),
            sharing="exact",
            cache="disabled",
            cache_directory="none",
        )

        def run(label):
            return run_fixture(runner, case, config, label)

        baseline = run("shared")
        coverage.update((e[3], e[4]) for e in baseline["events"] if e[0] <= 4)
        if index == 0:
            config.getroot().set("sharing", "disabled")
            equivalent(baseline, run("unshared"))
            cache = case / "cache"
            cache.mkdir()
            config.getroot().attrib.update(
                sharing="exact", cache="automatic_read_write", cache_directory=str(cache)
            )
            equivalent(baseline, run("cache-origin"))
            reused = run("cache-reused")
            equivalent(baseline, reused)
            assert sum(int(r["cached_deliveries"]) for r in reused["producers"]) > 0, (
                "persisted mode did not reuse"
            )
    assert ("rejected", "0") in coverage, "below-lot fixture did not exercise entry rejection"
    assert ("rejected", "1") in coverage, "gap fixture did not exercise whole-order cash rejection"
    assert ("risk", "1") in coverage, "crash fixture did not trigger S2 high-water risk"
    assert ("submitted", "risk_exit") in coverage, "risk fixture did not override the strategy exit"
    source_identity_cases(runner, fixture, template, root)
    print(f"S2_CLI_PASS evidence={root}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--compare",
        type=Path,
        action="append",
        default=[],
        help="other run; uses its effective-config.xml",
    )
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--scratch", type=Path)
    args = parser.parse_args()
    if not args.dataset:
        if not all((args.runner, args.fixture, args.scratch)) or args.output or args.compare:
            parser.error("use --dataset/--output, or --runner/--fixture/--scratch")
        synthetic_suite(args.runner.resolve(), args.fixture.resolve(), args.config, args.scratch)
        return
    if args.output is None or any((args.runner, args.fixture, args.scratch)):
        parser.error("existing-output verification requires --dataset and --output")
    result = verify(args.dataset, args.config, args.output)
    for output in args.compare:
        equivalent(result, verify(args.dataset, output / "effective-config.xml", output))


if __name__ == "__main__":
    main()
