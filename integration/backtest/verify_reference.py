#!/usr/bin/env python3
"""Independent exact-policy oracle for synthetic native ledger evidence.

Uses Python integers/Fraction, not archived floating research code or native math.
Historical strategy parity is a later gate.
"""

import argparse
import csv
from fractions import Fraction
from pathlib import Path
import subprocess

MAX = (1 << 63) - 1
if not __debug__:
    raise SystemExit("Exact oracle requires assertions enabled; do not use Python -O.")


def even(value):
    q, r = divmod(value.numerator, value.denominator)
    return q + (2 * r > value.denominator or (2 * r == value.denominator and q % 2))


def ceil(value):
    return -(-value.numerator // value.denominator)


def execution(price, side, slippage):
    value = Fraction(price * (10000 + slippage if side == "buy" else 10000 - slippage), 10000)
    return ceil(value) if side == "buy" else value.numerator // value.denominator


def amounts(price, quantity, commission):
    cents = Fraction(price * quantity, 10**13)
    return even(cents), ceil(cents * Fraction(commission, 10000))


def affordable(price, budget, lot, commission):
    # A settled gross <= budget implies exact gross <= budget + half a cent.
    # Derive a tight independent bound before searching the lot lattice.
    left = 0
    right = ((2 * budget + 1) * 10**13 // (2 * price)) // lot + 1
    while left + 1 < right:
        middle = (left + right) // 2
        if sum(amounts(price, middle * lot, commission)) <= budget:
            left = middle
        else:
            right = middle
    assert sum(amounts(price, left * lot, commission)) <= budget
    assert sum(amounts(price, (left + 1) * lot, commission)) > budget
    return left * lot


def check_arithmetic(path):
    count = 0
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            p, q, c, s = (int(row[k]) for k in ("price", "quantity", "commission", "slippage"))
            gross, fee = amounts(p, q, c)
            expected = {
                "execution": execution(p, row["side"], s),
                "gross": gross,
                "fee": fee,
                "dividend": even(Fraction(p * q, 10**13)),
            }
            for key, value in expected.items():
                result = (
                    "error" if value > MAX or (key == "execution" and value <= 0) else str(value)
                )
                assert row[key] == result, (count, key, row, result)
            count += 1
    assert count == 512, f"incomplete arithmetic evidence: {count}"
    return count


def check_ledger(path):
    states = {}
    count = 0
    with path.open(newline="") as source:
        for raw in csv.DictReader(source):
            name, event, order = (raw[k] for k in ("scenario", "event", "order_kind"))
            row = {
                k: int(v) for k, v in raw.items() if k not in ("scenario", "event", "order_kind")
            }
            c, s, lot, price = (row[k] for k in ("commission", "slippage", "lot", "raw_price"))
            if event == "fund":
                assert name not in states
                states[name] = dict(
                    cash=row["initial"],
                    units=0,
                    basis=0,
                    realized=0,
                    dividends=0,
                    total_fees=0,
                    key=row["key"],
                    phase=row["phase"],
                    initial=row["initial"],
                    commission=c,
                    slippage=s,
                    lot=lot,
                )
            assert name in states, "event precedes funding"
            a = states[name]
            assert (a["commission"], a["slippage"], a["lot"], a["initial"]) == (
                c,
                s,
                lot,
                row["initial"],
            )
            assert row["key"] >= a["key"]
            if row["key"] == a["key"]:
                assert row["phase"] >= a["phase"]
            a["key"], a["phase"] = row["key"], row["phase"]
            if event == "dividend":
                assert row["phase"] == 0 and row["quantity"] == a["units"]
                payment = even(Fraction(a["units"] * row["per_unit"], 10**13))
                a["cash"] += payment
                a["dividends"] += payment
            elif event in ("buy", "sell", "terminal"):
                terminal = event == "terminal"
                assert bool(row["terminal"]) == terminal
                assert row["phase"] == (2 if terminal else 1)
                if terminal:
                    assert order == "none" and row["eligible"] == row["decision"] == 0
                else:
                    assert row["decision"] < row["eligible"] <= row["key"]
                side = "buy" if event == "buy" else "sell"
                executed = execution(price, side, s)
                assert 0 < executed <= MAX
                if side == "buy":
                    assert a["units"] == 0
                    cap = a["cash"] * 95 // 100
                    if order == "budget":
                        assert row["budget"] == cap
                        quantity = affordable(executed, cap, lot, c)
                    else:
                        assert order == "fixed"
                        quantity = row["requested_units"]
                else:
                    quantity = a["units"]
                assert quantity > 0 and quantity % lot == 0
                gross, fee = amounts(executed, quantity, c)
                assert (row["quantity"], row["executed_price"], row["gross"], row["fee"]) == (
                    quantity,
                    executed,
                    gross,
                    fee,
                ), (name, count, row)
                if side == "buy":
                    assert gross + fee <= cap
                    a["cash"] -= gross + fee
                    a["units"], a["basis"] = quantity, gross + fee
                else:
                    a["cash"] += gross - fee
                    a["realized"] += gross - fee - a["basis"]
                    a["units"], a["basis"] = 0, 0
                a["total_fees"] += fee
            else:
                assert event in ("fund", "mark")
            for field in ("cash", "units", "basis", "realized", "dividends", "total_fees"):
                assert row[field] == a[field], (name, count, field, row[field], a[field])
            position = even(Fraction(price * a["units"], 10**13))
            equity, unrealized = a["cash"] + position, position - a["basis"]
            assert (row["position_value"], row["equity"], row["unrealized"]) == (
                position,
                equity,
                unrealized,
            )
            assert equity - row["initial"] == a["realized"] + unrealized + a["dividends"]
            assert a["cash"] >= 0 and a["units"] >= 0
            count += 1
    assert len(states) == 9 and count == 64, (len(states), count)
    hand = states["hand"]
    assert (hand["cash"], hand["realized"], hand["dividends"]) == (1104500, 95000, 9500)
    for bps in (0, 5, 10, 20):
        assert states[f"cost{bps}_mark"]["units"] > 0
        assert states[f"cost{bps}_liquidate"]["units"] == 0
    return len(states), count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--scratch", type=Path)
    args = parser.parse_args()
    if args.executable:
        if args.evidence or not args.scratch:
            parser.error("--executable requires --scratch and excludes --evidence")
        run = subprocess.run(
            [str(args.executable.resolve()), str(args.scratch.resolve())],
            text=True,
            capture_output=True,
            check=True,
        )
        print(run.stdout, end="")
        paths = [
            line.removeprefix("ACCOUNTING_EVIDENCE=")
            for line in run.stdout.splitlines()
            if line.startswith("ACCOUNTING_EVIDENCE=")
        ]
        assert len(paths) == 1, "native run did not identify exact evidence"
        evidence = Path(paths[0])
    else:
        if not args.evidence:
            parser.error("supply --evidence or --executable with --scratch")
        evidence = args.evidence
    vectors = check_arithmetic(evidence / "arithmetic.csv")
    scenarios, rows = check_ledger(evidence / "ledger.csv")
    print(
        f"EXACT_ORACLE_PASS arithmetic_vectors={vectors} scenarios={scenarios} ledger_rows={rows}"
    )


if __name__ == "__main__":
    main()
