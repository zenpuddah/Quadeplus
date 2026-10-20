#!/usr/bin/env python3
"""Independent S3 integer/Fraction policy oracle (stdlib only).

This executes no archived Python and verifies no native implementation. Read-only
archive comparisons use its saved float CSV decimal tokens, not a tolerance.
API: load_prices(), simulate(), compare_archive(), compare_exact(), self_test().
simulate returns decisions, lifecycle events, fills and funding/close equity rows;
all financial coefficients are Price9/Quantity6/Money2 integers. Fractions in the
reconciliation are exact differences from retained decimal text, NOT assertions
that the original binary floats had those exact rational values.

Example (create-new output, outside the retained archive):
  python3 integration/backtest/verify_stochastic_reference.py \
    --research-bundle build/backtest-reference/strategy-execution-2026-10-18 \
    --out build/stochastic-reference-preparation

The retained reference implementation consists of engine.py,
research.py, evaluate.py, legacy/{reversion_signals,portfolio_engine}.py and
protocol.json. research.targets starts at max(199,start); fresh 2010 is later
than that cutoff. Legacy make_targets starts at the fifth valid bar. Neither
the 200-bar research gate nor archived invalid-bar resets are imported into S3.
"""

import argparse
import csv
from dataclasses import dataclass
from datetime import date, timedelta
from fractions import Fraction as F
import hashlib
import json
from pathlib import Path
import re


ORIGIN = "2004-11-18"
END = "2026-10-15"
NORMALIZATION = "research_decimal_to_price9_even_v1"
MANIFEST_SHA = "a907a8bb072405ef0bbb56b54e1e0cd6479e524880199c2258ed3dd3696e9bc4"
SPY_SHA = "28963337b6eba735cad1eb62dc333d497079cff73f02076d17d3439c48e167ae"
FILL_SHA = {
    5: "6f7a31b0da82406018dd6740c126e25c2fec5b87375c7ff88dd39730b5274609",
    10: "db290dc24c4755ca4f49bcacfb54a714f5c2e358ee81bf14d3ccc2f3e51873bc",
}
MAX = 2**63 - 1
P, Q, M = 10**9, 10**6, 100
EXPECTED_PAIRS = [
    ("2026-08-19", "2026-08-28"),
    ("2026-09-02", "2026-09-04"),
    ("2026-09-10", "2026-09-21"),
    ("2026-09-29", "2026-10-06"),
]
if not __debug__:
    raise SystemExit("Oracle requires assertions; do not use Python -O")


def even(x):
    x = F(x)
    whole, remainder = divmod(x.numerator, x.denominator)
    return whole + int(
        2 * remainder > x.denominator or (2 * remainder == x.denominator and whole % 2)
    )


def ceiling(x):
    x = F(x)
    return -(-x.numerator // x.denominator)


def bounded(x):
    assert isinstance(x, int) and -MAX <= x <= MAX, ("unsupported signed64 result", x)
    return x


def execution(raw, buying, slippage):
    assert raw > 0 and 0 <= slippage < 10000
    price = F(raw * (10000 + slippage if buying else 10000 - slippage), 10000)
    result = ceiling(price) if buying else price.numerator // price.denominator
    assert result > 0
    return bounded(result)


def amounts(price, quantity, commission):
    assert price > 0 and quantity >= 0 and 0 <= commission < 10000
    gross = F(price * quantity, 10**13)
    return bounded(even(gross)), bounded(ceiling(gross * F(commission, 10000)))


def affordable(price, budget, lot, commission):
    """Search settled costs, not floor(continuous budget / price-with-fee)."""
    assert budget >= 0 and lot > 0
    low, high = 0, (F(2 * budget + 1, 2) * 10**13 // price) // lot + 1
    while high - low > 1:
        mid = (low + high) // 2
        if sum(amounts(price, mid * lot, commission)) <= budget:
            low = mid
        else:
            high = mid
    quantity = bounded(low * lot)
    assert sum(amounts(price, quantity, commission)) <= budget
    assert sum(amounts(price, quantity + lot, commission)) > budget
    return quantity


def csv_rows(path):
    with Path(path).open(newline="") as source:
        return list(csv.DictReader(source))


@dataclass(frozen=True)
class Bar:
    date: str
    opening: int
    high: int
    low: int
    close: int
    dividend: int = 0


def load_prices(path, origin=ORIGIN, end=END):
    """Validate raw decimals before normalization; return bars, originals, changes.

    This oracle consumes an already pinned original grid, not a dataset importer
    or an independent exchange calendar. AdjClose is deliberately never used.
    """
    rows = csv_rows(path)
    dates = [r["Date"] for r in rows]
    assert dates == sorted(set(dates)) and origin in dates and end in dates
    bars, originals, changes = [], [], []
    fields = ("Open", "High", "Low", "Close", "Dividend")
    for row in rows:
        date.fromisoformat(row["Date"])
        if not origin <= row["Date"] <= end:
            continue
        assert all(re.fullmatch(r"\d+(?:\.\d+)?", row[k]) for k in (*fields, "Volume"))
        raw = {k: F(row[k]) for k in fields}
        o, h, low, c, d = (raw[k] for k in fields)
        assert 0 < low <= min(o, c) <= max(o, c) <= h and d >= 0
        volume = F(row["Volume"]) * Q
        assert volume.denominator == 1 and 0 <= volume <= MAX
        values = {k: bounded(even(raw[k] * P)) for k in fields}
        assert 0 < values["Low"] <= min(values["Open"], values["Close"])
        assert max(values["Open"], values["Close"]) <= values["High"]
        for k in fields:
            error = F(values[k], P) - raw[k]
            if error:
                changes.append(
                    dict(
                        date=row["Date"],
                        field=k,
                        source=row[k],
                        normalized_raw=values[k],
                        error=error,
                    )
                )
        bars.append(Bar(row["Date"], *(values[k] for k in fields)))
        originals.append(row)
    return bars, originals, changes


def stochastic(bars):
    result = []
    for i, bar in enumerate(bars):
        if i < 4:
            result.append(None)
            continue
        low = min(b.low for b in bars[i - 4 : i + 1])
        high = max(b.high for b in bars[i - 4 : i + 1])
        value = F(bar.close - low, high - low) if high != low else F(1, 2)
        assert 0 <= value <= 1
        result.append(value)
    return result


def episodes(values, start):
    """Market-only episode replay, including pre-funding legacy observations."""
    active = None
    rows = []
    for i, k in enumerate(values):
        action = "warmup" if i < start else "warming"
        if i >= start and k is not None:
            if active is None:
                if k < F(1, 5):
                    active, action = i, "activate"
                else:
                    action = "inactive"
            elif k > F(4, 5) or i - active >= 10:
                # Both predicates remain visible; the label is deterministic.
                action = "threshold_exit" if k > F(4, 5) else "timer_exit"
                active = None
            else:
                action = "active"
        rows.append(
            dict(key=i, k=k, episode_start=active, target=active is not None, action=action)
        )
    return rows


def simulate(
    bars, begin, profile="fresh", commission=5, slippage=5, initial=1000000, lot=1, terminal=None
):
    """Independent bounded S3 policy, no risk overlays or native code imports.

    begin is a session index, funding is begin-1. Fresh entries are attempted
    once per episode; rejection consumes it. Legacy rejection retains a diagnostic
    result with parity_eligible=False. Keys always refer to the numerical origin.
    Optional terminal override exists for synthetic endpoint tests only.
    """
    assert profile in ("legacy", "fresh") and 0 < begin < len(bars)
    assert initial > 0 and lot > 0 and 0 <= commission < 10000 and 0 <= slippage < 10000
    bounded(initial)
    bounded(lot)
    terminal = terminal or ("synthetic" if profile == "legacy" else "mark")
    assert terminal in ("synthetic", "mark")
    decisions = episodes(stochastic(bars), 0 if profile == "legacy" else begin)
    for row, bar in zip(decisions, bars):
        row.update(
            date=bar.date, may_submit=row["key"] >= (begin - 1 if profile == "legacy" else begin)
        )
    cash, units, basis, realized, dividends, fees = initial, 0, 0, 0, 0, 0
    pending, next_id = None, 1
    events, fills, equity = [], [], []
    rejected = 0

    def snapshot(key, kind, mark, phase, **extra):
        value = bounded(even(F(units * mark, 10**13)))
        total = bounded(cash + value)
        assert cash >= 0 and units >= 0 and basis >= 0
        assert total - initial == realized + value - basis + dividends
        result = dict(
            key=key,
            date=bars[key].date,
            kind=kind,
            phase=phase,
            mark_raw=mark,
            cash_cents=cash,
            units_raw=units,
            basis_cents=basis,
            realized_cents=realized,
            dividend_cents=dividends,
            fee_cents=fees,
            equity_cents=total,
            **extra,
        )
        events.append(result)
        return result.copy()

    def fill(key, order, raw, phase):
        nonlocal cash, units, basis, realized, fees, rejected
        buying = order["side"] == "Buy"
        price = execution(raw, buying, slippage)
        assert not buying or (units == 0 and cash == order["decision_cash_cents"])
        quantity = affordable(price, order["budget_cents"], lot, commission) if buying else units
        if quantity == 0:
            rejected += 1
            snapshot(key, "rejected", raw, phase, **order, reason="BelowMinimumLot")
            return
        gross, fee = amounts(price, quantity, commission)
        detail = dict(
            order,
            price_raw=price,
            quantity_raw=quantity,
            gross_cents=gross,
            commission_cents=fee,
            raw_price=raw,
        )
        snapshot(key, "accepted", raw, phase, **detail)
        if buying:
            assert gross + fee <= min(cash, order["budget_cents"])
            cash -= gross + fee
            units, basis = quantity, gross + fee
        else:
            cash = bounded(cash + gross - fee)
            realized = bounded(realized + gross - fee - basis)
            units, basis = 0, 0
        fees = bounded(fees + fee)
        fills.append(snapshot(key, "filled", raw, phase, **detail))

    for i in range(begin - 1, len(bars)):
        bar = bars[i]
        carried = units > 0
        if i == begin - 1:
            snapshot(i, "funded", bar.close, "Close")
        else:
            payment = bounded(even(F(units * bar.dividend, 10**13)))
            cash, dividends = bounded(cash + payment), bounded(dividends + payment)
            snapshot(
                i,
                "dividend",
                bars[i - 1].close,
                "PriorClose",
                payment_cents=payment,
                prior_units_raw=units,
                dividend_per_unit_raw=bar.dividend,
            )
            if pending is not None:
                fill(i, pending, bar.opening, "Open")
                pending = None
        row = snapshot(
            i,
            "close",
            bar.close,
            "Close",
            funding=i == begin - 1,
            close_invested=units > 0,
            legacy_any_session_exposure=carried or units > 0,
        )
        equity.append(row)
        decision = decisions[i]
        buying = decision["action"] == "activate" or (
            profile == "legacy" and i == begin - 1 and decision["target"]
        )
        selling = not decision["target"] and units > 0
        if decision["may_submit"] and ((buying and units == 0) or selling):
            pending = dict(
                order_id=next_id,
                side="Buy" if buying else "Sell",
                decision_key=i,
                signal_date=bar.date,
                eligible_key=i + 1,
                decision_reason=decision["action"],
                budget_cents=cash * 95 // 100 if buying else 0,
                decision_cash_cents=cash,
                synthetic=False,
            )
            next_id += 1
            snapshot(i, "submitted", bar.close, "Close", **pending)
    final = len(bars) - 1
    if pending is not None:
        snapshot(final, "expired", bars[-1].close, "Close", **pending, reason="end_of_scope")
    if terminal == "synthetic" and units:
        # No decision key or fabricated eligible open for a reporting-only sale.
        fill(
            final,
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
            bars[-1].close,
            "Terminal",
        )
    last = snapshot(
        final, "final", bars[-1].close, "Terminal" if terminal == "synthetic" else "Close"
    )
    # Preserve ordinary close equity separately, replace only the reporting point.
    equity[-1]["ordinary_equity_cents"] = equity[-1]["equity_cents"]
    equity[-1].update(
        {
            k: last[k]
            for k in (
                "cash_cents",
                "units_raw",
                "basis_cents",
                "realized_cents",
                "fee_cents",
                "equity_cents",
            )
        }
    )
    peak, max_dd = initial, F(0)
    for row in equity:
        peak = max(peak, row["equity_cents"])
        row["drawdown"] = F(peak - row["equity_cents"], peak)
        max_dd = max(max_dd, row["drawdown"])
    summary = dict(
        profile=profile,
        terminal=terminal,
        commission_bps=commission,
        slippage_bps=slippage,
        initial_cash_cents=initial,
        lot_step_raw=lot,
        numerical_origin=bars[0].date,
        funding=bars[begin - 1].date,
        evaluation_begin=bars[begin].date,
        evaluation_end=bars[-1].date,
        evaluation_sessions=len(bars) - begin,
        equity_rows=len(equity),
        completed_positions=sum(f["side"] == "Sell" for f in fills),
        final_equity_cents=last["equity_cents"],
        final_units_raw=units,
        realized_cents=realized,
        dividend_cents=dividends,
        fee_cents=fees,
        rejected_entries=rejected,
        parity_eligible=profile != "legacy" or rejected == 0,
        max_drawdown=max_dd,
        native_verified=False,
    )
    return dict(summary=summary, decisions=decisions, events=events, fills=fills, equity=equity)


def decision_audit(bars, originals, begin, profile):
    """Distinguish decimal normalization from binary-float threshold effects."""
    raw, floating = [], []
    for i, row in enumerate(originals):
        if i < 4:
            raw.append(None)
            floating.append(None)
            continue
        window = originals[i - 4 : i + 1]
        low, high = min(F(r["Low"]) for r in window), max(F(r["High"]) for r in window)
        raw.append(F(F(row["Close"]) - low, high - low) if high != low else F(1, 2))
        lo, hi = min(float(r["Low"]) for r in window), max(float(r["High"]) for r in window)
        # Preserve the different expression order in each retained source.
        k100 = (
            50.0
            if hi == lo
            else 100 * ((float(row["Close"]) - lo) / (hi - lo))
            if profile == "legacy"
            else 100 * (float(row["Close"]) - lo) / (hi - lo)
        )
        floating.append(F.from_float(k100) / 100)
    normalized = stochastic(bars)
    start = 0 if profile == "legacy" else begin
    states = [episodes(v, start) for v in (raw, normalized, floating)]
    changes = []
    for i in range(4, len(bars)):
        predicates = [(k[i] < F(1, 5), k[i] > F(4, 5)) for k in (raw, normalized, floating)]
        state = [(s[i]["action"], s[i]["episode_start"]) for s in states]
        if predicates[0] != predicates[1] or predicates[1] != predicates[2] or len(set(state)) != 1:
            changes.append(
                dict(
                    date=bars[i].date,
                    key=i,
                    raw_ratio=raw[i],
                    normalized_ratio=normalized[i],
                    float_ratio=floating[i],
                    predicates=predicates,
                    states=state,
                )
            )
    return changes


def compare_archive(result, originals, fills_path, equity_path):
    """Exact, telescoping per-row reconciliation against retained float text.

    Unit differences include budget/cash propagation and the settled-cost lot
    lattice; the latter can admit MORE than floor(continuous units). Decimal CSV
    export/binary arithmetic residuals are separately named, never zeroed by eps.
    """
    saved_fills, saved_equity = csv_rows(fills_path), csv_rows(equity_path)
    raw_by_date = {r["Date"]: r for r in originals}
    old_equity_by_date = {r["date"]: r for r in saved_equity}
    assert len(saved_fills) == len(result["fills"]), "trade-count decision discrepancy"
    fill_diff = []
    rate = F(result["summary"]["commission_bps"], 10000)
    slip = result["summary"]["slippage_bps"]
    for actual, old in zip(result["fills"], saved_fills):
        stage = "Close" if actual["synthetic"] else "Open"
        assert (actual["date"], actual["signal_date"], stage, actual["side"]) == (
            old["date"],
            old["signal_date"],
            old["stage"],
            old["side"],
        ), (actual, old)
        assert old["reason"] == ("YearEnd" if actual["synthetic"] else "Signal")
        buying = actual["side"] == "Buy"
        original = raw_by_date[actual["date"]]
        source = F(original["Close" if actual["synthetic"] else "Open"])
        normalized = F(actual["raw_price"], P)
        factor = F(10000 + slip if buying else 10000 - slip, 10000)
        price, quantity = F(actual["price_raw"], P), F(actual["quantity_raw"], Q)
        old_price, old_quantity = F(old["fillprice"]), F(old["units"])
        gross, fee = F(actual["gross_cents"], M), F(actual["commission_cents"], M)
        components = dict(
            price_normalization=(normalized - source) * factor,
            adverse_price_rounding=price - normalized * factor,
            archived_price_float_export_residual=source * factor - old_price,
            gross_price_effect=(price - old_price) * old_quantity,
            gross_quantity_effect=price * (quantity - old_quantity),
            gross_settlement_rounding=gross - price * quantity,
            fee_price_quantity_effect=(price * quantity - old_price * old_quantity) * rate,
            fee_ceiling=fee - price * quantity * rate,
            archived_fee_float_export_residual=old_price * old_quantity * rate - F(old["fee"]),
        )
        assert (
            sum(
                components[k]
                for k in (
                    "price_normalization",
                    "adverse_price_rounding",
                    "archived_price_float_export_residual",
                )
            )
            == price - old_price
        )
        assert (
            sum(
                components[k]
                for k in (
                    "gross_price_effect",
                    "gross_quantity_effect",
                    "gross_settlement_rounding",
                )
            )
            == gross - old_price * old_quantity
        )
        assert sum(
            components[k]
            for k in (
                "fee_price_quantity_effect",
                "fee_ceiling",
                "archived_fee_float_export_residual",
            )
        ) == fee - F(old["fee"])
        row = dict(
            date=actual["date"],
            side=actual["side"],
            signal_date=actual["signal_date"],
            price_delta=price - old_price,
            units_delta=quantity - old_quantity,
            gross_delta=gross - old_price * old_quantity,
            fee_delta=fee - F(old["fee"]),
            cash_after_delta=F(actual["cash_cents"], M) - F(old["cash_after"]),
            **components,
        )
        if buying:
            continuous = F(actual["budget_cents"], M) / (price * (1 + rate))
            lot = result["summary"]["lot_step_raw"]
            lattice_floor = (continuous * Q // lot) * lot
            old_cash_at_decision = F(old_equity_by_date[actual["signal_date"]]["cash"])
            old_budget = old_cash_at_decision * F(95, 100)
            native_unrounded_budget = F(actual["decision_cash_cents"], M) * F(95, 100)
            budget = F(actual["budget_cents"], M)
            units_parts = dict(
                units_propagated_cash_effect=(native_unrounded_budget - old_budget)
                / (price * (1 + rate)),
                units_budget_floor_effect=(budget - native_unrounded_budget) / (price * (1 + rate)),
                units_price_effect=old_budget / (price * (1 + rate))
                - old_budget / (old_price * (1 + rate)),
                units_continuous_lot_floor=F(lattice_floor, Q) - continuous,
                units_settled_lattice_effect=quantity - F(lattice_floor, Q),
                archived_units_float_export_residual=old_budget / (old_price * (1 + rate))
                - old_quantity,
            )
            assert sum(units_parts.values()) == quantity - old_quantity
            row.update(
                budget_cents=actual["budget_cents"],
                continuous_units=continuous,
                continuous_floor_units_raw=lattice_floor,
                settled_lattice_adjustment_raw=actual["quantity_raw"] - lattice_floor,
                **units_parts,
            )
        fill_diff.append(row)
    actual_equity = (
        result["equity"] if result["summary"]["profile"] == "legacy" else result["equity"][1:]
    )
    assert len(actual_equity) == len(saved_equity)
    equity_diff = []
    old_cash, old_units, native_units = F(result["summary"]["initial_cash_cents"], M), F(0), 0
    dividend_rows = {r["date"]: r for r in result["events"] if r["kind"] == "dividend"}
    fills_by_date = {}
    for new, old in zip(result["fills"], saved_fills):
        fills_by_date.setdefault(new["date"], []).append((new, old))
    previous_cash_delta = F(0)
    for actual, old in zip(actual_equity, saved_equity):
        assert actual["date"] == old["date"], "equity row/date mismatch"
        source = raw_by_date[actual["date"]]
        close = F(actual["mark_raw"], P)
        raw_close = F(source["Close"])
        quantity = F(actual["units_raw"], Q)
        old_q, old_c = F(old["units"]), F(old["cash"])
        cash_delta = F(actual["cash_cents"], M) - old_c
        value = F(actual["equity_cents"] - actual["cash_cents"], M)
        parts = dict(
            cash_effect=cash_delta,
            position_quantity_effect=(quantity - old_q) * close,
            position_price_normalization=old_q * (close - raw_close),
            mark_settlement_rounding=value - quantity * close,
            archived_mark_float_export_residual=old_c + old_q * raw_close - F(old["equity"]),
        )
        equity_delta = F(actual["equity_cents"], M) - F(old["equity"])
        assert sum(parts.values()) == equity_delta
        div = dividend_rows.get(actual["date"])
        dividend = F(div["payment_cents"], M) if div else F(0)
        raw_dividend = old_units * F(source["Dividend"])
        div_quantity = (F(native_units, Q) - old_units) * F(source["Dividend"])
        div_normalization = (
            F(native_units, Q) * (F(div["dividend_per_unit_raw"], P) - F(source["Dividend"]))
            if div
            else F(0)
        )
        div_rounding = (
            dividend - F(native_units, Q) * F(div["dividend_per_unit_raw"], P) if div else F(0)
        )
        assert div_quantity + div_normalization + div_rounding == dividend - raw_dividend
        modeled_old_cash = old_cash + raw_dividend
        cash_fill_delta = F(0)
        for new_fill, old_fill in fills_by_date.get(actual["date"], []):
            sign = -1 if new_fill["side"] == "Buy" else 1
            old_gross = F(old_fill["fillprice"]) * F(old_fill["units"])
            modeled_old_cash += sign * old_gross - F(old_fill["fee"])
            cash_fill_delta += sign * (F(new_fill["gross_cents"], M) - old_gross) - (
                F(new_fill["commission_cents"], M) - F(old_fill["fee"])
            )
        cash_residual = modeled_old_cash - old_c
        assert (
            previous_cash_delta + dividend - raw_dividend + cash_fill_delta + cash_residual
            == cash_delta
        )
        equity_diff.append(
            dict(
                date=actual["date"],
                equity_delta=equity_delta,
                **parts,
                dividend_delta=dividend - raw_dividend,
                dividend_quantity_effect=div_quantity,
                dividend_normalization=div_normalization,
                dividend_payment_rounding=div_rounding,
                previous_cash_delta=previous_cash_delta,
                settled_fill_cash_effect=cash_fill_delta,
                archived_cash_float_export_residual=cash_residual,
                drawdown_percentage_point_delta=actual["drawdown"] * 100 - F(old["drawdown_pct"]),
            )
        )
        old_cash, old_units, native_units = old_c, old_q, actual["units_raw"]
        previous_cash_delta = cash_delta
    return dict(fills=fill_diff, equity=equity_diff)


def compare_exact(expected, actual, tables=("decisions", "events", "fills", "equity")):
    """Future native adapters supply records in this oracle schema, with no eps.

    This helper only compares caller-supplied data; calling simulate by itself
    never establishes native agreement. Fractions must be decoded by the adapter.
    """
    for table in tables:
        assert len(expected[table]) == len(actual[table]), (table, "row count")
        for i, (want, got) in enumerate(zip(expected[table], actual[table])):
            assert want == got, (table, i, want, got)


def verify_archive(root):
    manifest = root / "package_manifest.json"
    assert hashlib.sha256(manifest.read_bytes()).hexdigest() == MANIFEST_SHA
    entries = json.loads(manifest.read_text())
    assert len(entries) == 372
    for name, digest in entries.items():
        path = root / name
        assert path.resolve().is_relative_to(root.resolve()) and not path.is_symlink()
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
    assert entries["data/SPY.csv"] == SPY_SHA
    for cost, digest in FILL_SHA.items():
        assert entries[f"results/legacy_c{cost}_fills.csv"] == digest
    return entries


def self_test():
    """Synthetic boundaries absent from (or hidden by) the legacy fixture."""

    def bars(closes, dividends=None):
        return [
            Bar(
                (date(2000, 1, 1) + timedelta(days=i)).isoformat(),
                100 * P,
                150 * P,
                50 * P,
                int(c * P),
                (dividends or {}).get(i, 0),
            )
            for i, c in enumerate(closes)
        ]

    assert even(F(10000000005, 10)) == 1000000000
    assert even(F(10000000015, 10)) == 1000000002
    flat = [Bar(str(i), P, P, P, P) for i in range(5)]
    assert stochastic(flat) == [None, None, None, None, F(1, 2)]
    k = stochastic(bars([100] * 4 + [70, 69, 130, 131]))
    trace = episodes(k, 0)
    assert trace[4]["action"] == "inactive" and k[4] == F(1, 5)
    assert trace[5]["action"] == "activate"
    assert trace[6]["action"] == "active" and k[6] == F(4, 5)
    assert trace[7]["action"] == "threshold_exit"
    fixture = bars([100] * 4 + [60] * 15)
    fresh = simulate(fixture, 4, commission=0, slippage=0)
    assert [(f["key"], f["side"]) for f in fresh["fills"]][:3] == [
        (5, "Buy"),
        (15, "Sell"),
        (16, "Buy"),
    ]
    assert fresh["decisions"][14]["action"] == "timer_exit"
    assert fresh["decisions"][15]["action"] == "activate"  # no same-close reentry at 14
    mid = simulate(fixture, 9, "legacy", commission=0, slippage=0)
    reset = simulate(fixture, 9, "fresh", commission=0, slippage=0)
    assert mid["fills"][0]["key"] == 9 and mid["fills"][1]["key"] == 15
    assert mid["decisions"][8]["episode_start"] == 4
    assert reset["fills"][0]["key"] == 10 and reset["decisions"][9]["episode_start"] == 9
    rejected = simulate(fixture, 4, initial=1)
    assert [r["key"] for r in rejected["events"] if r["kind"] == "rejected"] == [5, 16]
    assert rejected["decisions"][14]["action"] == "timer_exit" and not rejected["fills"]
    failed_legacy = simulate(fixture, 9, "legacy", initial=1)
    assert not failed_legacy["summary"]["parity_eligible"]
    short = bars([100] * 4 + [60, 100])
    terminal = simulate(short, 4, terminal="synthetic")
    marked = simulate(short, 4)
    assert [(f["key"], f["phase"]) for f in terminal["fills"]] == [(5, "Open"), (5, "Terminal")]
    assert (
        terminal["fills"][-1]["eligible_key"] is None
        and terminal["summary"]["final_units_raw"] == 0
    )
    assert marked["summary"]["final_units_raw"] > 0
    assert (
        terminal["equity"][-1]["ordinary_equity_cents"] == marked["summary"]["final_equity_cents"]
    )
    assert terminal["equity"][-1]["close_invested"]
    expiring = simulate(bars([100] * 4 + [60]), 4)
    assert not expiring["fills"] and any(e["kind"] == "expired" for e in expiring["events"])
    dividend = simulate(bars([100] * 4 + [60, 131, 100], {5: 1889000000, 6: 1889000000}), 4)
    payments = [r for r in dividend["events"] if r["kind"] == "dividend"]
    assert payments[1]["payment_cents"] == 0  # ex-date buyer not entitled
    assert (
        payments[2]["payment_cents"]
        == even(F(payments[2]["prior_units_raw"] * 1889000000, 10**13))
        > 0
    )
    assert dividend["fills"][-1]["side"] == "Sell" and dividend["fills"][-1]["key"] == 6
    assert affordable(100 * P, 950000, 1, 0) == 95000050  # settled lattice, not continuous floor
    # Causality: changing current/future close cannot change an already eligible open.
    changed = short[:5] + [Bar(short[5].date, 100 * P, 150 * P, 50 * P, 149 * P)]
    assert simulate(changed, 4)["fills"][0] == marked["fills"][0]
    assert stochastic(changed)[:5] == stochastic(short)[:5]
    # A deliberately sub-Price9 close is below 20 before normalization, exactly
    # 20 afterwards. Detection is required even though the pinned SPY has no flips.
    boundary_bars = [Bar(str(i), P + 1, P + 5, P, P + 1) for i in range(5)]
    boundary_raw = [dict(Close="1.0000000009", High="1.000000005", Low="1") for _ in range(5)]
    boundary = decision_audit(boundary_bars, boundary_raw, 4, "fresh")
    assert len(boundary) == 1 and boundary[0]["raw_ratio"] == F(9, 50)
    assert boundary[0]["normalized_ratio"] == F(1, 5)
    # Reconciliation must detect changes, rather than hiding them behind a tolerance.
    try:
        compare_exact({"events": [{"cash": 1}]}, {"events": [{"cash": 2}]}, ("events",))
    except AssertionError:
        pass
    else:
        raise AssertionError("exact comparator accepted changed cash")
    return [
        "ties-even normalization",
        "five-bar readiness/flat half",
        "strict K20/K80",
        "a+10 close/a+11 open/no same-close reentry",
        "funding-mid-episode/fresh reset",
        "fresh rejection consumed/legacy parity rejected",
        "final morning synthetic sale/Mark",
        "final intent expiry",
        "ex-date buyer/seller",
        "largest settled-affordable lot",
        "opening causality/future-prefix invariance",
        "quantization-caused threshold change detected",
        "exact comparator detects one-cent change",
    ]


def json_default(value):
    if isinstance(value, F):
        return str(value)
    raise TypeError(type(value).__name__)


def write_json(path, value):
    with path.open("x") as output:
        json.dump(value, output, indent=2, default=json_default)
        output.write("\n")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--research-bundle", type=Path)
    parser.add_argument("--out", type=Path, help="create-new directory outside retained archive")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    checks = self_test()
    print(f"S3_REFERENCE_SYNTHETIC_PASS groups={len(checks)} native_verified=false")
    if args.self_test and args.research_bundle is None:
        return
    if args.research_bundle is None or args.out is None:
        parser.error("provide --research-bundle and --out, or --self-test")
    root, out = args.research_bundle.resolve(), args.out.resolve()
    assert not out.is_relative_to(root), "output must not be inside retained archive"
    assert not out.exists() and out.parent.is_dir(), "output must be new with an existing parent"
    entries = verify_archive(root)
    bars, originals, normalizations = load_prices(root / "data/SPY.csv")
    assert len(bars) == 5511 and len(normalizations) == 799
    assert max(abs(r["error"]) for r in normalizations) == F(1, 2 * P)
    out.mkdir()
    write_json(out / "normalizations.json", dict(policy=NORMALIZATION, changes=normalizations))
    summaries = []
    blockers = []
    for profile, costs, begin_date in (
        ("legacy", (5, 10), "2026-08-17"),
        ("fresh", (5, 10, 20), "2010-01-04"),
    ):
        begin = next(i for i, b in enumerate(bars) if b.date == begin_date)
        audit = decision_audit(bars, originals, begin, profile)
        if audit:
            blockers.append(dict(profile=profile, decision_changes=audit))
        for cost in costs:
            result = simulate(bars, begin, profile, cost, cost)
            if profile == "legacy":
                assert result["summary"]["funding"] == "2026-08-14"
                assert result["summary"]["evaluation_sessions"] == 43
                assert result["summary"]["parity_eligible"]
                pairs = [
                    (a["date"], b["date"])
                    for a, b in zip(result["fills"][::2], result["fills"][1::2])
                ]
                assert pairs == EXPECTED_PAIRS
                assert all(f["decision_reason"] == "threshold_exit" for f in result["fills"][1::2])
                prefix = f"legacy_c{cost}"
            else:
                assert result["summary"]["evaluation_sessions"] == 4222
                prefix = f"SPY_B0_c{cost}"
            comparison = compare_archive(
                result,
                originals,
                root / f"results/{prefix}_fills.csv",
                root / f"results/{prefix}_equity.csv",
            )
            result["summary"].update(
                fills=len(result["fills"]),
                events=len(result["events"]),
                decision_changes=len(audit),
                original_final_equity=csv_rows(root / f"results/{prefix}_equity.csv")[-1]["equity"],
                final_equity_delta=comparison["equity"][-1]["equity_delta"],
            )
            write_json(out / f"{profile}_c{cost}_expected.json", result)
            write_json(out / f"{profile}_c{cost}_reconciliation.json", comparison)
            summaries.append(result["summary"])
            print(
                f"S3_REFERENCE_ARCHIVE_COMPARED profile={profile} cost_each={cost} "
                f"sessions={len(bars) - begin} fills={len(result['fills'])} "
                f"equity_cents={result['summary']['final_equity_cents']} decision_changes={len(audit)}"
            )
    write_json(
        out / "summary.json",
        dict(
            schema="s3-reference-preparation-v1",
            native_verified=False,
            manifest_sha256=MANIFEST_SHA,
            verified_archive_files=len(entries),
            spy_sha256=SPY_SHA,
            normalization=NORMALIZATION,
            synthetic_checks=checks,
            runs=summaries,
            blockers=blockers,
            fraction_encoding="exact numerator/denominator strings; integer coefficients remain JSON integers",
            scope="Independent policy reference and retained float reconciliation; native verification is separate.",
        ),
    )
    # Confirm read-only evidence remained byte-identical throughout the run.
    verify_archive(root)
    assert not blockers, "threshold/state changes block parity; see summary.json"
    print(f"S3_REFERENCE_PREPARATION_PASS evidence={out} native_verified=false")


if __name__ == "__main__":
    main()
