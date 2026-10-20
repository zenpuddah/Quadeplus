#!/usr/bin/env python3
"""Independent S1 windows, decisions, fills, ledger and control oracle."""

import argparse
import csv
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from verify_reference import affordable, amounts, even, execution


def execute(args, success=True):
    result = subprocess.run([str(a) for a in args], text=True, capture_output=True)
    if (result.returncode == 0) != success:
        raise AssertionError(
            f"Unexpected CLI result {result.returncode}: {result.stdout}\n{result.stderr}"
        )
    return result


def verify(dataset, config, output):
    cfg = ET.parse(config).getroot().attrib
    with (dataset / "prices.csv").open(newline="") as source:
        prices = [
            row
            for row in csv.DictReader(source)
            if cfg["numerical_origin"] <= row["Date"] <= cfg["evaluation_end"]
        ]
    assert (
        prices[0]["Date"] == cfg["numerical_origin"] and prices[-1]["Date"] == cfg["evaluation_end"]
    )
    begin = next(i for i, row in enumerate(prices) if row["Date"] == cfg["evaluation_begin"])
    close = [even(Fraction(row["Close"]) * 10**9) for row in prices]
    opening = [even(Fraction(row["Open"]) * 10**9) for row in prices]
    dividend = [even(Fraction(row["Dividend"]) * 10**9) for row in prices]
    fast = [
        None if i < 19 else even(Fraction(sum(close[i - 19 : i + 1]), 20))
        for i in range(len(prices))
    ]
    slow = [
        None if i < 49 else even(Fraction(sum(close[i - 49 : i + 1]), 50))
        for i in range(len(prices))
    ]
    expected_decisions = []
    for key in range(len(prices)):
        allowed = key >= begin
        action = (
            0
            if not allowed or fast[key] is None or slow[key] is None
            else 1
            if fast[key] > slow[key]
            else 2
        )
        reason = (
            "warmup"
            if not allowed
            else "warming"
            if fast[key] is None or slow[key] is None
            else "sma20_gt_sma50"
            if action == 1
            else "sma20_le_sma50"
        )
        expected_decisions.append(
            dict(
                key=str(key),
                may_submit=str(int(allowed)),
                funded=str(int(key >= begin - 1)),
                sma20_raw="" if fast[key] is None else str(fast[key]),
                sma50_raw="" if slow[key] is None else str(slow[key]),
                action=str(action),
                reason=reason,
            )
        )
    with (output / "decisions.csv").open(newline="") as source:
        actual_decisions = list(csv.DictReader(source))
    assert actual_decisions == expected_decisions, "SMA/readiness/decision trace mismatch"
    initial, lot, commission, slippage = (
        int(cfg[k])
        for k in ("initial_cash_cents", "lot_step_raw", "commission_bps", "slippage_bps")
    )
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
        )
        for _ in range(3)
    ]
    expected = []

    def emit(trial, key, kind, reason, mark=None, phase=None, order=None, fill=None):
        a = accounts[trial]
        price, quantity, gross, fee = fill if fill else (mark or 0, 0, 0, 0)
        equity = None if mark is None else a["cash"] + even(Fraction(mark * a["units"], 10**13))
        expected.append(
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
                equity,
                phase,
                price,
                quantity,
                gross,
                fee,
                order["decision"] if order else None,
                order["eligible"] if order else None,
                order["policy"] if order else "none",
                order["budget"] if order else 0,
            ]
        )
        if equity is not None:
            assert (
                equity - initial
                == a["realized"] + (equity - a["cash"] - a["basis"]) + a["dividends"]
            )
        if kind == "close" and key >= begin:
            a["peak"] = max(a["peak"], equity)
            loss = a["peak"] - equity
            drawdown = Fraction(loss, a["peak"])
            if drawdown > a["dd"]:
                a.update(dd=drawdown, dd_loss=loss, dd_peak=a["peak"])

    for key in range(len(prices)):
        if key >= begin:
            for trial, a in enumerate(accounts):
                payment = even(Fraction(a["units"] * dividend[key], 10**13))
                a["cash"] += payment
                a["dividends"] += payment
                emit(trial, key, "dividend", "ex_date_cash", close[key - 1], 0)
                order = a["pending"]
                if order:
                    buying = order["policy"] == "cash_budget"
                    price = execution(opening[key], "buy" if buying else "sell", slippage)
                    quantity = (
                        affordable(price, order["budget"], lot, commission)
                        if buying
                        else a["units"]
                    )
                    if quantity == 0:
                        emit(trial, key, "rejected", "0", order=order)
                    else:
                        gross, fee = amounts(price, quantity, commission)
                        fill = (price, quantity, gross, fee)
                        emit(trial, key, "accepted", order["reason"], opening[key], 1, order, fill)
                        if buying:
                            assert a["units"] == 0 and gross + fee <= order["budget"]
                            a["cash"] -= gross + fee
                            a["units"], a["basis"] = quantity, gross + fee
                        else:
                            a["cash"] += gross - fee
                            a["realized"] += gross - fee - a["basis"]
                            a["units"], a["basis"] = 0, 0
                        a["fees"] += fee
                        emit(trial, key, "filled", order["reason"], opening[key], 1, order, fill)
                    a["pending"] = None
        for trial, a in enumerate(accounts):
            if key == begin - 1:
                emit(trial, key, "funded", "initial_cash", close[key], 2)
            if key >= begin - 1:
                emit(trial, key, "close", "ordinary", close[key], 2)
            if key < begin:
                continue
            if trial == 0:
                action, reason = (
                    int(expected_decisions[key]["action"]),
                    expected_decisions[key]["reason"],
                )
            else:
                action, reason = (
                    (1, "buy_hold_control") if trial == 2 and key == begin else (0, "hold")
                )
            if (action == 1 and a["units"] == 0) or (action == 2 and a["units"] > 0):
                order = dict(
                    id=a["next_id"],
                    decision=key,
                    eligible=key + 1,
                    reason=reason,
                    policy="cash_budget" if action == 1 else "sell_all",
                    budget=a["cash"] * 95 // 100 if action == 1 else 0,
                )
                a["next_id"] += 1
                a["pending"] = order
                emit(trial, key, "submitted", reason, order=order)
    for trial, a in enumerate(accounts):
        if a["pending"]:
            emit(trial, len(prices) - 1, "expired", "end_of_scope", order=a["pending"])
            a["pending"] = None
        emit(trial, len(prices) - 1, "final", "endpoint", close[-1], 2)
    actual = []
    for line in (output / "financial.txt").read_text().splitlines():
        if not line or not line[0].isdigit():
            continue
        fields = shlex.split(line)
        assert len(fields) == 21, fields
        actual.append(
            [
                v if i in (3, 4, 19) else None if v == "none" else int(v)
                for i, v in enumerate(fields)
            ]
        )
    assert len(actual) == len(expected), (len(actual), len(expected))
    for index, (got, wanted) in enumerate(zip(actual, expected)):
        assert got == wanted, (index, got, wanted)
    summary = json.loads((output / "summary.json").read_text())
    assert (
        summary["financial_state"] == "completed"
        and summary["evaluation_sessions"] == len(prices) - begin
    )
    assert (
        summary["dataset_sha256"]
        == hashlib.sha256((dataset / "manifest.qdaily").read_bytes()).hexdigest()
    )
    assert len(summary["trials"]) == 3
    for trial, (reported, a) in enumerate(zip(summary["trials"], accounts), 1):
        equity = a["cash"] + even(Fraction(a["units"] * close[-1], 10**13))
        for field, value in dict(
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
        ).items():
            assert reported[field] == value, (trial, field, reported[field], value)
    print(
        f"S1_EXACT_ORACLE_PASS decisions={len(prices)} financial_rows={len(actual)} trials=3 dataset={dataset}"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--scratch", type=Path)
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.dataset:
        verify(args.dataset, args.config, args.output)
        return
    args.scratch.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="sma.", dir=args.scratch)).resolve()
    execute([args.fixture])
    for index, (mode, begin, cash) in enumerate(
        (("trend", 1, 1000000), ("flat", 1, 1000000), ("trend", 60, 1000000), ("trend", 1, 1))
    ):
        case = root / f"case{index}"
        execute([args.fixture, "--fixture", case, mode, begin])
        with (case / "prices.csv").open(newline="") as source:
            rows = list(csv.DictReader(source))
        config = ET.parse(args.config)
        config.getroot().attrib.update(
            symbol="TEST",
            numerical_origin=rows[0]["Date"],
            evaluation_begin=rows[begin]["Date"],
            evaluation_end=rows[-1]["Date"],
            initial_cash_cents=str(cash),
        )
        path = case / "config.xml"
        config.write(path, encoding="unicode")
        result = execute(
            [
                args.runner,
                "run",
                "--config",
                path,
                "--dataset",
                case / "dataset",
                "--out",
                case / "run",
            ]
        )
        assert "RUN_ACCEPTED" in result.stdout
        verify(case / "dataset", path, case / "run")
        if index == 0:
            before = (case / "run/summary.json").read_bytes()
            execute(
                [
                    args.runner,
                    "run",
                    "--config",
                    path,
                    "--dataset",
                    case / "dataset",
                    "--out",
                    case / "run",
                ],
                False,
            )
            assert (case / "run/summary.json").read_bytes() == before
            original = path.read_text()
            bad_configs = [
                original.replace("<backtest ", '<backtest unknown_setting="1" '),
                original.replace('version="1"', 'version="2"'),
                original.replace('version="1"', 'version="1" version="1"'),
                original.replace('commission_bps="5"', 'commission_bps="10000"'),
                original.replace('commission_bps="5"', 'commission_bps="+5"'),
                original.replace("/>", "><child/></backtest>"),
            ]
            for bad_index, text in enumerate(bad_configs):
                invalid = case / f"invalid{bad_index}"
                (case / "bad.xml").write_text(text)
                execute(
                    [
                        args.runner,
                        "run",
                        "--config",
                        case / "bad.xml",
                        "--dataset",
                        case / "dataset",
                        "--out",
                        invalid,
                    ],
                    False,
                )
                assert not invalid.exists()
            shutil.copytree(case / "dataset", case / "corrupt")
            with (case / "corrupt/prices.csv").open("a") as target:
                target.write("corrupt\n")
            execute(
                [
                    args.runner,
                    "run",
                    "--config",
                    path,
                    "--dataset",
                    case / "corrupt",
                    "--out",
                    case / "corrupt-run",
                ],
                False,
            )
            assert not (case / "corrupt-run").exists()
    execute(
        [
            args.runner,
            "prepare",
            "--research-bundle",
            root / "absent",
            "--symbol",
            "SPY",
            "--schedule-profile",
            "retained_reference_grid_v1",
            "--out",
            root / "missing-input",
        ],
        False,
    )
    assert not (root / "missing-input").exists()
    (root / "wrong-package").mkdir()
    (root / "wrong-package/package_manifest.json").write_text("{}")
    execute(
        [
            args.runner,
            "prepare",
            "--research-bundle",
            root / "wrong-package",
            "--symbol",
            "SPY",
            "--schedule-profile",
            "retained_reference_grid_v1",
            "--out",
            root / "wrong-pin",
        ],
        False,
    )
    assert not (root / "wrong-pin").exists()
    print(f"S1_CLI_PASS evidence={root}")


if __name__ == "__main__":
    main()
