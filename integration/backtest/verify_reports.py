#!/usr/bin/env python3
# ruff: noqa: E402 -- bytecode suppression intentionally precedes verification imports
"""Read-only independent reconciliation of native per-trial report metrics.

Inputs come from effective-config.xml, the financial.txt v1 configuration and
21-field events, and hash-checked prepared prices/session dates. No report field
is used to build the oracle inputs. This verifies reporting and exact accounting
transitions; it does not rerun strategy signals or create an acceptance marker.
The required 2025-12-31 historical section is rebuilt from a validated ordinary-
close prefix of that SAME account, with original funding and preterminal holdings.
Its JSON metrics and visible HTML table are checked independently of the full view.

Only native floating statistics have a tolerance: relative 2e-12, absolute 2e-14
for volatility, Sharpe, CAGR, mean close capital fraction, and the arithmetic mean
of the two central rolling returns (even-sample median). All other ratios, cents,
counts, dates, undefined reasons, and drawdown durations must match exactly.

Usage:
  python3 -B integration/backtest/verify_reports.py --dataset DIR --run DIR
  python3 -B integration/backtest/verify_reports.py --dataset DIR --suite DIR
"""

import sys

sys.dont_write_bytecode = True

import argparse
from collections import Counter
import csv
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from fractions import Fraction
import hashlib
from html.parser import HTMLParser
import io
import math
from pathlib import Path
import re
import shlex
import time

try:
    from .report_metrics import AccountTotals, CloseEquity, Metric, calculate_metrics
    from .verify_suite import (
        Bundle,
        COSTS,
        PROFILES,
        parse_json,
        projections,
        require,
        xml_document,
    )
except ImportError:
    from report_metrics import AccountTotals, CloseEquity, Metric, calculate_metrics
    from verify_suite import Bundle, COSTS, PROFILES, parse_json, projections, require, xml_document

REL_TOL, ABS_TOL = 2e-12, 2e-14
MONEY_PRODUCT_SCALE = 10**13  # Price9 * Quantity6 -> cents.
REJECTIONS = (
    "BelowMinimumLot",
    "InsufficientCash",
    "AllocationCap",
    "PositionAlreadyOpen",
    "NoPosition",
    "CashChanged",
    "NotEligible",
    "NonpositiveProtectiveThreshold",
)
STATE_FIELDS = ("cash", "units", "basis", "realized", "dividends", "fees")
EVENT_FIELDS = (
    "trial",
    "key",
    "id",
    "kind",
    "reason",
    *STATE_FIELDS,
    "equity",
    "phase",
    "price",
    "quantity",
    "gross",
    "fee",
    "decision_key",
    "eligible_key",
    "quantity_policy",
    "budget",
)
TEXT_FIELDS = {"kind", "reason", "quantity_policy"}
OPTIONAL_FIELDS = {"equity", "phase", "decision_key", "eligible_key"}
FLOAT_METRICS = {
    "annualized_volatility",
    "sharpe_zero_risk_free",
    "cagr",
    "mean_close_capital_fraction",
}
ADJUSTMENT_POLICY = "split_adjusted_ohlc_dividends_same_share_basis_v1"
ADJUSTMENT_INTERPRETATION = "Split-adjusted OHLC; separate dividends on the same share basis; AdjClose is retained but not traded; no additional split postings."
RISK_GAP_INTERPRETATION = "Difference of separately ties-even settled holding values: actual raw open minus trigger close, using trigger-held units; loss=max(0,-gap PnL); execution drag=raw open value-executed gross+fee; dividends excluded; diagnostics already included in equity, never subtracted again."
HISTORICAL_CUTOFF = date(2025, 12, 31)
HISTORICAL_PROFILE = "continuous_account_through_2025_12_31_v1"
HISTORICAL_REASONS = {
    "legacy_profile": "Not applicable: legacy initialization profile; no fresh continuous-account historical section.",
    "financial_scope_starts_after_cutoff": "Not applicable: the financial evaluation starts after 2025-12-31.",
    "financial_scope_ends_before_cutoff": "Not applicable: the financial evaluation ends before 2025-12-31.",
    "partial_financial_run": "Historical section unavailable: the financial run is partial; no cutoff metrics are asserted.",
}


def integer(text):
    value = int(text)
    require(
        str(value) == text and -(2**63) <= value < 2**63,
        "noncanonical/out-of-range integer: " + text,
    )
    return value


def even(value):
    value = Fraction(value)
    whole, remainder = divmod(value.numerator, value.denominator)
    return whole + int(
        2 * remainder > value.denominator or (2 * remainder == value.denominator and whole % 2)
    )


def ceil(value):
    return -(-value.numerator // value.denominator)


def gross(price, quantity):
    return even(Fraction(price * quantity, MONEY_PRODUCT_SCALE))


@dataclass(frozen=True)
class PriceRow:
    day: date
    opening: int
    close: int
    dividend: int


@dataclass
class Dataset:
    rows: list
    manifest: dict
    manifest_sha256: str


def load_dataset(directory):
    bundle = Bundle(directory)
    manifest_bytes = bundle.read("manifest.qdaily")
    digest = hashlib.sha256(manifest_bytes).hexdigest()
    require(
        bundle.read("COMPLETE").decode() == f"quadeplus-daily-research-v1 {digest}\n",
        "dataset COMPLETE/manifest mismatch",
    )
    lines = manifest_bytes.decode().splitlines()
    require(lines[0] == "quadeplus-daily-research-v1", "unsupported prepared dataset schema")
    manifest = {}
    for line in lines[1:]:
        fields = shlex.split(line)
        require(len(fields) == 2 and fields[0] not in manifest, "invalid/duplicate manifest field")
        manifest[fields[0]] = fields[1]
    require(
        manifest["normalization"] == "research_decimal_to_price9_even_v1"
        and manifest["adjustment"] == ADJUSTMENT_POLICY
        and manifest["clock"] == "date_utc_midnight_close_plus_second_v1",
        "unsupported dataset price/clock conventions",
    )
    retained = {}
    for name, field in (
        ("prices.csv", "prices_sha256"),
        ("sessions.csv", "sessions_sha256"),
        ("normalization.csv", "normalizations_sha256"),
    ):
        retained[name] = bundle.read(name, 64 * 1024 * 1024)
        require(
            hashlib.sha256(retained[name]).hexdigest() == manifest[field],
            "dataset hash mismatch: " + name,
        )
    reader = csv.DictReader(io.StringIO(retained["prices.csv"].decode(), newline=""))
    require(
        reader.fieldnames
        == ["Date", "Open", "High", "Low", "Close", "Volume", "AdjClose", "Dividend"],
        "unexpected prices header",
    )
    raw_rows = list(reader)
    grid = list(csv.reader(io.StringIO(retained["sessions.csv"].decode(), newline="")))
    require(
        grid == [["Date"]] + [[row["Date"]] for row in raw_rows],
        "retained price/session grid differs",
    )
    require(
        len(raw_rows) == integer(manifest["original_rows"]), "original dataset row count mismatch"
    )
    selected, previous = [], None
    for row in raw_rows:
        day = date.fromisoformat(row["Date"])
        require(
            day.isoformat() == row["Date"] and (previous is None or day > previous),
            "unordered/noncanonical source dates",
        )
        previous = day
        o, h, low, c, dividend = (
            Fraction(row[name]) for name in ("Open", "High", "Low", "Close", "Dividend")
        )
        require(
            0 < low <= min(o, c) <= max(o, c) <= h and dividend >= 0,
            "invalid unrounded source prices/dividend",
        )
        if manifest["numerical_origin"] <= row["Date"] <= manifest["evaluation_end"]:
            selected.append(PriceRow(day, even(o * 10**9), even(c * 10**9), even(dividend * 10**9)))
    require(
        len(selected) == integer(manifest["selected_rows"])
        and selected
        and selected[0].day.isoformat() == manifest["numerical_origin"]
        and selected[-1].day.isoformat() == manifest["evaluation_end"],
        "selected numerical scope mismatch",
    )
    return Dataset(selected, manifest, digest)


class ConfigurationCursor:
    """Read the host's length-framed configuration; semicolons inside text are data."""

    def __init__(self, text):
        self.text, self.pos = text, 0

    def token(self):
        end = self.text.index(";", self.pos)
        value, self.pos = self.text[self.pos : end], end + 1
        return value

    def framed(self):
        colon = self.text.index(":", self.pos)
        size = integer(self.text[self.pos : colon])
        require(size >= 0, "negative framed string length")
        end = colon + 1 + size
        require(end < len(self.text) and self.text[end] == ";", "bad framed configuration string")
        value, self.pos = self.text[colon + 1 : end], end + 1
        return value

    def numbers(self, count):
        values = [integer(part) for part in self.token().split(",")]
        require(len(values) == count, "host configuration numeric field count mismatch")
        return values


def financial_header(lines, config, dataset):
    require(
        lines[0] == "quadeplus_financial_ledger_v1" and lines[2] == "financial_state=completed",
        "financial evidence is unsupported or partial",
    )
    require(
        lines[1].startswith("experiment=") and lines[3].startswith("configuration_bytes="),
        "financial evidence header missing",
    )
    experiment = lines[1].removeprefix("experiment=")
    size, text = lines[3].removeprefix("configuration_bytes=").split(":", 1)
    require(
        integer(size) == len(text.encode()) and text.isascii(),
        "host configuration byte length/encoding mismatch",
    )
    require(
        hashlib.sha256(text.encode()).hexdigest() == experiment,
        "financial experiment digest mismatch",
    )
    cursor = ConfigurationCursor(text)
    fields = [cursor.framed() for _ in range(9)]
    require(
        fields[:4]
        == [
            "quadeplus_financial_host_v1",
            dataset.manifest_sha256,
            "price9_money2_exact_v1",
            "ex_date_cash_compatibility_v1",
        ],
        "financial source/profile identity mismatch",
    )
    funding, begin, end, legacy = cursor.numbers(4)
    limits = cursor.numbers(4)
    dates = [row.day.isoformat() for row in dataset.rows]
    require(
        config["numerical_origin"] == dates[0] and config["evaluation_end"] == dates[-1],
        "configured numerical origin/end differs from dataset",
    )
    require(
        begin == dates.index(config["evaluation_begin"])
        and end == len(dates)
        and funding == begin - 1
        and funding >= 0,
        "financial keys differ from configured date scope",
    )
    require(
        dataset.manifest["evaluation_begin"] <= dates[begin] <= dataset.manifest["evaluation_end"],
        "financial begin outside permitted range",
    )
    require(
        config["profile"] in ("fresh_research_v1", "legacy_spy_2026_v1")
        and legacy == int(config["profile"] == "legacy_spy_2026_v1"),
        "initialization profile mismatch",
    )
    require(
        config["terminal"] in ("mark", "synthetic_close")
        and config["currency"] == dataset.manifest["currency"] == "USD"
        and config["symbol"] == dataset.manifest["symbol"]
        and config["controls"] == "cash,buy_hold"
        and integer(config["allocation_bps"]) == 9500,
        "unsupported account/control convention",
    )
    trials = {}
    while cursor.pos < len(text):
        trial = integer(cursor.token())
        require(trial not in trials, "duplicate configured trial ID")
        strategy, strategy_config, currency = (cursor.framed() for _ in range(3))
        initial, lot, commission, slippage, terminal, drawdown, daily_loss = cursor.numbers(7)
        require(
            [initial, lot, commission, slippage]
            == [
                integer(config[key])
                for key in ("initial_cash_cents", "lot_step_raw", "commission_bps", "slippage_bps")
            ],
            "trial costs/capital differ from effective XML",
        )
        require(
            initial > 0
            and lot > 0
            and 0 <= commission < 10000
            and 0 <= slippage < 10000
            and currency == config["currency"]
            and terminal == int(config["terminal"] == "synthetic_close"),
            "invalid trial financial configuration",
        )
        observations = []
        while cursor.pos < len(text):
            prefix = re.match(r"[0-9]+([:;])", text[cursor.pos :])
            require(prefix is not None, "invalid observation/trial framing")
            if prefix[1] == ";":
                break
            name = cursor.framed()
            observations.append((name, cursor.numbers(3)))
        trials[trial] = dict(
            strategy=strategy,
            strategy_configuration=strategy_config,
            currency=currency,
            initial=initial,
            lot=lot,
            commission=commission,
            slippage=slippage,
            terminal=terminal,
            drawdown=drawdown,
            daily_loss=daily_loss,
            observations=observations,
        )
    require(0 < len(trials) <= limits[0], "host trial inventory exceeds its bound")
    count = 6 if config["strategy"] == "keltner_atr_risk_v1" else 3
    require(
        config["strategy"] in ("sma20_50_long_cash_v1", "keltner_atr_risk_v1", "spy_stochastic5_v1")
        and len(trials) == count
        and list(trials) == list(range(1, count + 1)),
        "unexpected application trial inventory",
    )
    require(
        [t["strategy"] for t in trials.values()]
        == [config["strategy"]] * (count - 2)
        + ["cash_usd_no_interest_v1", "buy_hold95_next_open_v1"],
        "strategy/control IDs differ from configuration",
    )
    return dict(
        experiment=experiment,
        funding=funding,
        begin=begin,
        end=end,
        legacy=bool(legacy),
        trials=trials,
        limits=limits,
    )


def parse_events(lines, header):
    grouped = {trial: [] for trial in header["trials"]}
    require(len(lines) - 4 <= header["limits"][2], "financial trace exceeds admitted record bound")
    for line_number, line in enumerate(lines[4:], 5):
        parts = shlex.split(line)
        require(len(parts) == 21, f"financial.txt:{line_number}: expected 21 event fields")
        event = {
            key: token
            if key in TEXT_FIELDS
            else None
            if key in OPTIONAL_FIELDS and token == "none"
            else integer(token)
            for key, token in zip(EVENT_FIELDS, parts)
        }
        require(event["trial"] in grouped, "undeclared financial trial")
        grouped[event["trial"]].append(event)
    return grouped


def _reconstruct(events, trial, header, dataset, historical_key=None):
    """Rebuild account postings before using any ordinary close as a metric input."""
    funding, begin, end = (header[key] for key in ("funding", "begin", "end"))
    state = dict(cash=trial["initial"], units=0, basis=0, realized=0, dividends=0, fees=0)
    rows, trades = [], []
    pending, accepted = None, None
    next_close, last_key = begin, funding
    funded = funding_close = finalized = session_dividend = any_exposure = False
    dividends_seen = terminal_count = gross_total = slippage_total = expired = 0
    rejected, ids = Counter(), set()
    historical_inputs = None

    def balances(event):
        require(
            all(event[key] == state[key] for key in STATE_FIELDS),
            f"account transition differs at {event['key']} {event['kind']}",
        )
        require(
            state["cash"] >= 0
            and state["units"] >= 0
            and state["basis"] >= 0
            and state["units"] % trial["lot"] == 0
            and (state["units"] > 0 or state["basis"] == 0),
            "invalid long/cash account",
        )
        require(
            state["cash"] + state["basis"] - trial["initial"]
            == state["realized"] + state["dividends"],
            "cash/basis P&L identity",
        )

    def valuation(event, phase, fill=False):
        price = (
            dataset.rows[event["key"] - 1].close
            if phase == 0
            else (
                dataset.rows[event["key"]].opening
                if phase == 1
                else dataset.rows[event["key"]].close
            )
        )
        position = gross(price, state["units"])
        require(
            event["phase"] == phase
            and event["equity"] == state["cash"] + position
            and (fill or event["price"] == price),
            "phase-specific equity/mark mismatch",
        )
        return position

    def quote(event, terminal=False):
        buying = not terminal and event["quantity_policy"] in ("cash_budget", "fixed_quantity")
        require(
            buying or terminal or event["quantity_policy"] == "sell_all",
            "unknown fill quantity policy",
        )
        raw = dataset.rows[event["key"]].close if terminal else dataset.rows[event["key"]].opening
        slipped = Fraction(
            raw * (10000 + trial["slippage"] if buying else 10000 - trial["slippage"]), 10000
        )
        price = ceil(slipped) if buying else slipped.numerator // slipped.denominator
        quantity = event["quantity"]
        require(
            price > 0 and quantity > 0 and quantity % trial["lot"] == 0,
            "invalid positive fill price/lot",
        )
        settled, fee = (
            gross(price, quantity),
            ceil(Fraction(price * quantity * trial["commission"], 10**17)),
        )
        require(
            (event["price"], event["gross"], event["fee"]) == (price, settled, fee),
            "execution/settlement differs from exact oracle",
        )
        if buying:
            require(
                state["units"] == 0 and settled + fee <= state["cash"] * 95 // 100,
                "buy violates allocation/cash/position constraint",
            )
            if event["quantity_policy"] == "cash_budget":
                require(settled + fee <= event["budget"], "cash-budget fill exceeds frozen budget")
                next_q = quantity + trial["lot"]
                next_cost = gross(price, next_q) + ceil(
                    Fraction(price * next_q * trial["commission"], 10**17)
                )
                require(next_cost > event["budget"], "cash-budget quantity is not maximal")
        else:
            require(
                quantity == state["units"] and quantity > 0, "sale is not a full position close"
            )
        return (
            buying,
            settled,
            fee,
            settled - gross(raw, quantity) if buying else gross(raw, quantity) - settled,
        )

    for event in events:
        key, kind = event["key"], event["kind"]
        require(
            not finalized and funding <= last_key <= key < end,
            "out-of-scope, regressing or post-final event",
        )
        last_key = key
        if kind == "funded":
            require(not funded and key == funding, "duplicate/misplaced funding")
            funded = True
        else:
            require(funded, "event before funding")
        if kind == "dividend":
            require(
                funding_close and key == next_close and not session_dividend and accepted is None,
                "dividend session coverage/order mismatch",
            )
            any_exposure = state["units"] > 0
            payment = gross(dataset.rows[key].dividend, state["units"])
            state["cash"] += payment
            state["dividends"] += payment
            dividends_seen += 1
            session_dividend = True
        elif kind == "submitted":
            require(
                pending is None
                and accepted is None
                and event["id"] > 0
                and event["id"] not in ids
                and event["decision_key"] == key
                and event["eligible_key"] == key + 1
                and (
                    (key == funding and header["legacy"] and funding_close)
                    or (key >= begin and key + 1 == next_close)
                ),
                "invalid submitted intent",
            )
            require(
                event["quantity_policy"] in ("cash_budget", "fixed_quantity", "sell_all"),
                "unsupported submitted policy",
            )
            require(
                event["budget"]
                == (state["cash"] * 95 // 100 if event["quantity_policy"] == "cash_budget" else 0),
                "frozen budget mismatch",
            )
            require(
                event["quantity"] >= 0
                and (event["quantity_policy"] == "fixed_quantity" or event["quantity"] == 0),
                "submitted quantity mismatch",
            )
            ids.add(event["id"])
            pending = event
        elif kind == "accepted":
            require(
                session_dividend
                and key == next_close
                and pending is not None
                and accepted is None
                and event["id"] == pending["id"],
                "acceptance without pending open intent",
            )
            for name in ("decision_key", "eligible_key", "quantity_policy", "budget", "reason"):
                require(event[name] == pending[name], "accepted intent drift: " + name)
            if event["quantity_policy"] == "fixed_quantity":
                require(event["quantity"] == pending["quantity"], "fixed quantity resized")
            quote(event)
            accepted = event
        elif kind in ("filled", "terminal"):
            terminal = kind == "terminal"
            if terminal:
                require(
                    trial["terminal"]
                    and key == end - 1
                    and next_close == end
                    and pending is None
                    and accepted is None
                    and terminal_count == 0
                    and event["id"] > 0
                    and event["id"] not in ids
                    and event["decision_key"] is None
                    and event["eligible_key"] is None
                    and event["quantity_policy"] == "none",
                    "invalid synthetic terminal fill",
                )
                ids.add(event["id"])
                terminal_count += 1
            else:
                require(
                    session_dividend and key == next_close and accepted is not None,
                    "fill without preceding acceptance",
                )
                require(
                    all(
                        event[name] == accepted[name]
                        for name in EVENT_FIELDS
                        if name not in {*STATE_FIELDS, "kind", "equity"}
                    ),
                    "accepted/fill quote drift",
                )
            buying, settled, fee, impact = quote(event, terminal)
            previous_realized = state["realized"]
            if buying:
                state["basis"] = settled + fee
                state["cash"] -= state["basis"]
                state["units"] = event["quantity"]
                any_exposure = True
            else:
                state["cash"] += settled - fee
                state["realized"] += settled - fee - state["basis"]
                state["units"] = state["basis"] = 0
                require(event["realized"] == state["realized"], "full-sale realized delta mismatch")
                trades.append(event["realized"] - previous_realized)
            require(impact >= 0, "negative adverse slippage diagnostic")
            gross_total += settled
            slippage_total += impact
            state["fees"] += fee
            pending = accepted = None
        elif kind in ("rejected", "expired"):
            require(
                pending is not None and accepted is None and event["id"] == pending["id"],
                "rejection/expiration without pending intent",
            )
            for name in ("quantity_policy", "budget", "quantity", "decision_key", "eligible_key"):
                require(event[name] == pending[name], "unfilled intent drift: " + name)
            if kind == "rejected":
                require(
                    session_dividend
                    and key == next_close
                    and event["reason"] in tuple(map(str, range(len(REJECTIONS)))),
                    "invalid rejection timing/reason",
                )
                rejected[REJECTIONS[int(event["reason"])]] += 1
            else:
                require(
                    key == end - 1 and next_close == end and event["reason"] == "end_of_scope",
                    "invalid expiration",
                )
                expired += 1
            pending = None
        elif kind == "close":
            require(event["reason"] == "ordinary" and accepted is None, "invalid ordinary close")
            if key == funding:
                require(not funding_close, "duplicate funding close")
                funding_close = True
            else:
                require(key == next_close and session_dividend, "missing/duplicate ordinary close")
                position = gross(dataset.rows[key].close, state["units"])
                rows.append(
                    CloseEquity(
                        dataset.rows[key].day,
                        state["cash"] + position,
                        state["units"],
                        position,
                        any_exposure,
                    )
                )
                next_close += 1
                session_dividend = False
        elif kind == "final":
            require(
                key == end - 1 and next_close == end and pending is None and accepted is None,
                "final before complete account scope",
            )
            finalized = True
        elif kind in ("risk", "suppressed"):
            require(
                key >= begin
                and key + 1 == next_close
                and (kind != "suppressed" or event["reason"] == "risk_latched"),
                "risk diagnostic outside completed close",
            )
        else:
            require(kind == "funded", "unknown host event kind: " + kind)
        balances(event)
        if kind == "dividend":
            valuation(event, 0)
        elif kind in ("accepted", "filled"):
            valuation(event, 1, fill=True)
        elif kind in ("funded", "close", "risk", "final", "terminal"):
            valuation(event, 2, fill=kind == "terminal")
        else:
            require(
                event["equity"] is None
                and event["phase"] is None
                and event["price"] == event["gross"] == event["fee"] == 0,
                "unmarked intent contains settlement/valuation",
            )
        if kind in ("funded", "close", "risk", "final", "dividend"):
            require(
                event["id"]
                == event["quantity"]
                == event["gross"]
                == event["fee"]
                == event["budget"]
                == 0
                and event["decision_key"] is None
                and event["eligible_key"] is None
                and event["quantity_policy"] == "none",
                "nonfill event contains order amounts",
            )
        if kind == "close" and key == historical_key:
            require(
                historical_inputs is None and key >= begin,
                "historical snapshot is duplicate or outside evaluation",
            )
            require(
                sum(trades) == state["realized"],
                "historical realized trade prefix does not reconcile",
            )
            historical_inputs = (
                tuple(rows),
                tuple(trades),
                AccountTotals(
                    state["cash"],
                    gross(dataset.rows[key].close, state["units"]) - state["basis"],
                    state["dividends"],
                    state["fees"],
                    slippage_total,
                    gross_total,
                ),
                None,
                rejected.copy(),
                expired,
            )
    require(
        funded and funding_close and finalized and len(rows) == dividends_seen == end - begin,
        "incomplete financial session/funding coverage",
    )
    require(
        terminal_count == (int(rows[-1].invested_units_raw > 0) if trial["terminal"] else 0)
        and (not trial["terminal"] or state["units"] == 0),
        "terminal liquidation presence disagrees with preterminal holdings",
    )
    require(sum(trades) == state["realized"], "trade realized P&L aggregate mismatch")
    require(
        (historical_inputs is not None) == (historical_key is not None),
        "validated trace lacks required historical snapshot",
    )
    totals = AccountTotals(
        state["cash"],
        gross(dataset.rows[end - 1].close, state["units"]) - state["basis"],
        state["dividends"],
        state["fees"],
        slippage_total,
        gross_total,
    )
    return (
        rows,
        trades,
        totals,
        state["cash"] if trial["terminal"] else None,
        rejected,
        expired,
    ), historical_inputs


def reconstruct(events, trial, header, dataset):
    return _reconstruct(events, trial, header, dataset)[0]


def expected_metrics(dataset, trial, header, inputs):
    rows, trades, totals, terminal, rejected, expired = inputs
    expected = calculate_metrics(
        rows,
        funded_equity_cents=trial["initial"],
        funded_date=dataset.rows[header["funding"]].day,
        totals=totals,
        closed_trade_pnls_cents=trades,
        terminal_cash_cents=terminal,
    )
    expected["schema"] = "quadeplus-native-report-preparatory-v1"
    expected["currency"] = trial["currency"]
    expected["rejected_orders"] = sum(rejected.values())
    expected["expired_orders"] = expired
    expected["rejection_reasons"] = [
        dict(reason=reason, count=count) for reason, count in sorted(rejected.items())
    ]
    gaps = [
        Fraction(
            dataset.rows[key].opening - dataset.rows[key - 1].close, dataset.rows[key - 1].close
        )
        for key in range(header["begin"], header["end"])
    ]
    for row, gap in zip(expected["daily"], gaps):
        row["overnight_asset_gap"] = Metric(gap)
    expected["worst_overnight_asset_gap"] = Metric(min(gaps))
    expected["observed_overnight_gap_sessions"] = len(gaps)
    return expected


class Comparison:
    def __init__(self, rolling_count):
        self.exact = self.approximate = 0
        self.rolling_count = rolling_count

    def compare(self, actual, expected, path=""):
        def mismatch(message):
            raise ValueError(f"{path or '/'}: {message}")

        if isinstance(expected, Metric):
            if type(actual) is not dict or set(actual) != {"value", "undefined_reason"}:
                mismatch("metric encoding differs")
            self.compare(
                actual["undefined_reason"], expected.undefined_reason, path + "/undefined_reason"
            )
            value = actual["value"]
            if expected.value is None:
                self.compare(value, None, path + "/value")
            elif path.lstrip("/") in FLOAT_METRICS or (
                path == "/rolling_43/median" and self.rolling_count % 2 == 0
            ):
                if (
                    type(value) not in (int, Decimal)
                    or not math.isfinite(float(value))
                    or not math.isclose(
                        float(value), float(expected.value), rel_tol=REL_TOL, abs_tol=ABS_TOL
                    )
                ):
                    mismatch(
                        f"floating statistic mismatch: native={value}, oracle={float(expected.value):.17g}"
                    )
                self.approximate += 1
            else:
                if type(value) is not dict or set(value) != {"numerator", "denominator"}:
                    mismatch("exact ratio replaced by a non-ratio value")
                n, d = value["numerator"], value["denominator"]
                if (
                    type(n) is not int
                    or type(d) is not int
                    or d <= 0
                    or math.gcd(n, d) != 1
                    or Fraction(n, d) != expected.value
                ):
                    mismatch("exact ratio differs or is not reduced")
                self.exact += 1
        elif isinstance(expected, dict):
            if type(actual) is not dict or set(actual) != set(expected):
                mismatch(
                    "object field inventory differs: "
                    + str(set(actual) ^ set(expected) if isinstance(actual, dict) else type(actual))
                )
            for key, value in expected.items():
                self.compare(actual[key], value, path + "/" + key)
        elif isinstance(expected, list):
            if type(actual) is not list or len(actual) != len(expected):
                mismatch("sequence length differs")
            for index, (a, b) in enumerate(zip(actual, expected)):
                self.compare(a, b, path + "/" + str(index))
        else:
            if isinstance(expected, date):
                expected = expected.isoformat()
            if isinstance(expected, float):  # Exact declared constant 365.25, not a statistic.
                valid = type(actual) in (int, Decimal) and Fraction(actual) == Fraction.from_float(
                    expected
                )
            else:
                valid = type(actual) is type(expected) and actual == expected
            if not valid:
                mismatch(f"exact value differs: native={actual!r}, oracle={expected!r}")
            self.exact += 1


def project_risk(events, trial, header, dataset, *, complete=True):
    """Independent trigger/order projection, including observed partial prefixes.

    Uses only ordinary close snapshots to validate the first latched threshold.
    Gap diagnostics exist only for a recorded eligible opening fill. A synthetic
    terminal event remains separate; unrecorded execution is never inferred.
    """
    previous_key = header["funding"]
    high = previous_equity = trial["initial"]
    expected = None
    closes, triggers = {}, []
    for event in events:
        key = event["key"]
        require(
            previous_key <= key < header["end"] and event["units"] >= 0,
            "risk trace scope/order/units mismatch",
        )
        previous_key = key
        if event["kind"] == "close" and key >= header["begin"]:
            price = dataset.rows[key].close
            require(
                event["phase"] == 2
                and event["price"] == price
                and event["cash"] >= 0
                and event["equity"] is not None
                and event["equity"] >= 0
                and event["equity"] == event["cash"] + gross(price, event["units"]),
                "risk ordinary-close valuation mismatch",
            )
            equity = event["equity"]
            high = max(high, equity)
            if expected is None:
                for code, name, baseline, bps in (
                    ("1", "Drawdown", high, trial["drawdown"]),
                    ("2", "DailyLoss", previous_equity, trial["daily_loss"]),
                ):
                    if bps:
                        require(baseline > 0, "risk threshold has a nonpositive baseline")
                        if max(0, baseline - equity) * 10000 >= baseline * bps:
                            expected = dict(
                                key=key, code=code, reason=name, baseline=baseline, bps=bps
                            )
                            break  # Drawdown takes precedence if both qualify.
            previous_equity = equity
            closes[key] = event
        elif event["kind"] == "risk":
            require(
                not triggers
                and expected is not None
                and key == expected["key"]
                and event["reason"] == expected["code"]
                and key in closes,
                "missing/duplicate/inconsistent latched risk trigger",
            )
            require(
                all(
                    event[name] == closes[key][name]
                    for name in (*STATE_FIELDS, "equity", "phase", "price", "key")
                ),
                "risk trigger differs from preceding ordinary close",
            )
            triggers.append(event)
    if complete:
        require(
            bool(triggers) == (expected is not None),
            "completed report omitted required risk trigger",
        )
    if not triggers:
        require(
            not any(e["kind"] == "submitted" and e["reason"] == "risk_exit" for e in events),
            "orphan risk exit",
        )
        return (
            [],
            "No latched risk trigger occurred."
            if complete
            else "Partial trace: no latched risk trigger recorded.",
        )
    trigger = triggers[0]
    trigger_value = gross(dataset.rows[trigger["key"]].close, trigger["units"])
    orders, synthetic = [], None

    def location(event):
        return dict(date=dataset.rows[event["key"]].day.isoformat(), key=event["key"])

    def sale(event, terminal=False):
        require(
            event["quantity"] == trigger["units"] > 0 and event["units"] == 0,
            "risk sale does not close trigger holdings",
        )
        raw = dataset.rows[event["key"]].close if terminal else dataset.rows[event["key"]].opening
        execution = raw * (10000 - trial["slippage"]) // 10000
        fee = ceil(Fraction(execution * event["quantity"] * trial["commission"], 10**17))
        require(
            event["phase"] == (2 if terminal else 1)
            and event["price"] == execution > 0
            and event["gross"] == gross(execution, event["quantity"])
            and event["fee"] == fee,
            "risk execution/settlement mismatch",
        )
        return raw

    active = False
    last_close = None
    for event in events:
        if event["kind"] == "close":
            last_close = event
        if event is trigger:
            active = True
            continue
        if not active:
            require(
                not (event["kind"] == "submitted" and event["reason"] == "risk_exit"),
                "risk order precedes trigger",
            )
            continue
        kind, key = event["kind"], event["key"]
        if kind == "submitted":
            require(
                event["reason"] == "risk_exit"
                and trigger["units"] > 0
                and event["units"] == trigger["units"]
                and key >= trigger["key"]
                and event["id"] > 0
                and event["quantity_policy"] == "sell_all"
                and event["decision_key"] == key
                and event["eligible_key"] == key + 1
                and last_close is not None
                and last_close["key"] == key
                and (not orders or orders[-1]["status"] == "rejected"),
                "invalid risk exit submission",
            )
            require(event["id"] not in [order["id"] for order in orders], "duplicate risk order ID")
            orders.append(
                dict(
                    id=event["id"],
                    queued_date=dataset.rows[key].day.isoformat(),
                    queued_key=key,
                    eligible_key=key + 1,
                    eligible_date=dataset.rows[key + 1].day.isoformat()
                    if key + 1 < header["end"]
                    else None,
                    status="queued",
                    resolution=None,
                    fill=None,
                )
            )
        elif kind == "terminal":
            require(
                synthetic is None
                and trial["terminal"]
                and key == header["end"] - 1
                and event["decision_key"] is None
                and event["eligible_key"] is None
                and event["quantity_policy"] == "none"
                and (not orders or orders[-1]["status"] == "expired"),
                "synthetic endpoint mislabelled as executable risk exit",
            )
            sale(event, True)
            synthetic = dict(
                **location(event),
                id=event["id"],
                reason=event["reason"],
                quantity_raw=event["quantity"],
                executed_price_raw=event["price"],
                gross_cents=event["gross"],
                fee_cents=event["fee"],
            )
        elif kind in ("accepted", "filled", "expired", "rejected"):
            found = [order for order in orders if order["id"] == event["id"]]
            require(len(found) == 1, "lifecycle event after trigger has no risk order")
            order = found[0]
            require(
                event["decision_key"] == order["queued_key"]
                and event["eligible_key"] == order["eligible_key"]
                and event["quantity_policy"] == "sell_all",
                "risk order policy/eligibility drift",
            )
            if kind == "accepted":
                require(
                    order["status"] == "queued"
                    and key >= order["eligible_key"]
                    and event["reason"] == "risk_exit",
                    "invalid risk acceptance",
                )
                order["status"] = "accepted"
                continue
            require(order["resolution"] is None, "risk order resolved twice")
            if kind == "filled":
                require(
                    order["status"] == "accepted"
                    and key >= order["eligible_key"]
                    and event["reason"] == "risk_exit",
                    "risk fill is not an eligible opening execution",
                )
                raw = sale(event)
                raw_value = gross(raw, trigger["units"])
                gap, slippage = raw_value - trigger_value, raw_value - event["gross"]
                require(slippage >= 0 and event["fee"] >= 0, "negative risk execution drag")
                drag = slippage + event["fee"]
                order["fill"] = dict(
                    **location(event),
                    quantity_raw=event["quantity"],
                    raw_open_price_raw=raw,
                    executed_price_raw=event["price"],
                    raw_open_value_cents=raw_value,
                    executed_gross_cents=event["gross"],
                    fee_cents=event["fee"],
                    raw_open_gap_pnl_cents=gap,
                    raw_open_gap_loss_cents=max(0, -gap),
                    slippage_drag_cents=slippage,
                    execution_drag_cents=drag,
                    net_exit_vs_trigger_close_cents=gap - drag,
                )
            elif kind == "expired":
                require(
                    order["status"] == "queued"
                    and key == header["end"] - 1
                    and event["reason"] == "end_of_scope",
                    "invalid risk expiry",
                )
            else:
                require(
                    order["status"] == "queued"
                    and key >= order["eligible_key"]
                    and event["reason"] in tuple(map(str, range(len(REJECTIONS)))),
                    "invalid risk rejection",
                )
            order["status"] = kind
            order["resolution"] = dict(**location(event), kind=kind, reason=event["reason"])
    last = events[-1]
    filled = any(order["fill"] is not None for order in orders)
    pending = bool(orders and orders[-1]["status"] in ("queued", "accepted"))
    if complete:
        require(
            last["kind"] == "final" and not pending and (trigger["units"] == 0 or orders),
            "completed risk outcome unresolved",
        )
        require(
            not (filled or trigger["units"] == 0) or (last["units"] == 0 and synthetic is None),
            "holdings acquired after risk latch exit/flat trigger",
        )
    outcome = (
        "flat_trigger_no_order"
        if trigger["units"] == 0
        else "risk_exit_filled"
        if filled
        else "trigger_recorded_order_not_recorded"
        if not orders
        else "risk_exit_pending"
        if pending
        else (
            "risk_exit_expired_synthetic_endpoint" if synthetic else "risk_exit_expired_still_held"
        )
        if orders[-1]["status"] == "expired"
        else "risk_exit_rejected"
    )
    explanations = {
        "flat_trigger_no_order": "Risk latched while flat; no liquidation order was required.",
        "risk_exit_filled": "Risk exit filled at an eligible opening; gap and execution drag are diagnostics already included in equity.",
        "trigger_recorded_order_not_recorded": "Risk triggered while held, but no queued risk order is recorded in this partial trace.",
        "risk_exit_pending": "Risk exit is queued or accepted without a recorded fill in this partial trace; no execution is inferred.",
        "risk_exit_expired_synthetic_endpoint": "Risk order expired unexecuted; a separate synthetic endpoint sale flattened holdings and is not an executable risk exit.",
        "risk_exit_expired_still_held": "Risk order expired unexecuted; holdings remain at the observed endpoint.",
        "risk_exit_rejected": "Risk exit was rejected; no fill or gap execution is inferred.",
    }
    payload = dict(
        trigger=dict(
            **location(trigger),
            reason=expected["reason"],
            threshold_bps=expected["bps"],
            baseline_equity_cents=expected["baseline"],
            equity_cents=trigger["equity"],
            close_price_raw=trigger["price"],
            held_units_raw=trigger["units"],
            held_close_value_cents=trigger_value,
        ),
        orders=orders,
        synthetic_terminal=synthetic,
        endpoint=dict(
            financial_state="completed" if complete else "partial",
            observed_date=dataset.rows[last["key"]].day.isoformat(),
            observed_key=last["key"],
            observed_units_raw=last["units"],
            still_held=last["units"] > 0,
            risk_exit_executed=filled,
            pending_order_id=orders[-1]["id"] if pending else None,
        ),
        outcome=outcome,
    )
    return [payload], ("" if complete else "Partial trace: ") + explanations[outcome]


def compare_risk_metadata(metadata, expected):
    payload, outcome = expected
    require(
        metadata["risk_report_profile"] == "trigger_order_gap_v1"
        and metadata["risk_gap_interpretation"] == RISK_GAP_INTERPRETATION,
        "unknown risk reporting convention",
    )
    require(isinstance(metadata["risk_events_json"], str), "risk_events_json must be a JSON string")
    comparison = Comparison(0)
    comparison.compare(parse_json(metadata["risk_events_json"]), payload, "/risk_events")
    require(metadata["risk_outcome"] == outcome, "human risk outcome differs from observed events")
    return comparison.exact


def historical_scope(dataset, header, profile):
    """The financial evaluation must actually contain the fixed cutoff session."""
    if profile != "fresh_research_v1":
        require(profile == "legacy_spy_2026_v1", "unknown historical-view initialization profile")
        return None, "legacy_profile"
    rows = dataset.rows
    if rows[header["begin"]].day > HISTORICAL_CUTOFF:
        return None, "financial_scope_starts_after_cutoff"
    if rows[header["end"] - 1].day < HISTORICAL_CUTOFF:
        return None, "financial_scope_ends_before_cutoff"
    matches = [
        key for key in range(header["begin"], header["end"]) if rows[key].day == HISTORICAL_CUTOFF
    ]
    require(
        len(matches) == 1,
        "fresh scope spans 2025-12-31 but the declared cutoff session is missing/duplicated",
    )
    return matches[0], None


def historical_expected(dataset, trial, header, scope, snapshot, *, complete=True):
    key, reason = scope
    if not complete:
        reason = "partial_financial_run"
    if reason is not None:
        return dict(
            status="unavailable" if not complete else "not_applicable",
            reason=reason,
            cutoff_date=HISTORICAL_CUTOFF.isoformat(),
            metrics=None,
        ), HISTORICAL_REASONS[reason]
    require(
        snapshot is not None
        and len(snapshot[0]) == key - header["begin"] + 1
        and snapshot[3] is None,
        "historical view lacks validated ordinary-close prefix",
    )
    require(snapshot[0][-1].date == HISTORICAL_CUTOFF, "historical view ends on the wrong session")
    # Only reporting math sees the prefix. No fake final event, expired intent,
    # new capital, account restart or strategy rerun is introduced.
    metrics = expected_metrics(
        dataset, dict(trial, terminal=0), dict(header, end=key + 1), snapshot
    )
    summary = (
        f"Continuous account from funding {dataset.rows[header['funding']].day.isoformat()} through 2025-12-31; "
        f"{metrics['sessions']} ordinary closes; marked holdings; no capital reset or synthetic sale."
    )
    return metrics, summary


def historical_table(metrics):
    if "status" in metrics:
        return []

    def metric_ratio(value):
        return (
            value.value
            if isinstance(value, Metric)
            else Fraction(value["value"]["numerator"], value["value"]["denominator"])
        )

    drawdown = metric_ratio(metrics["drawdown"]["maximum"])
    return [
        (name, str(value))
        for name, value in (
            ("Funding date", metrics["funded_date"]),
            ("Cutoff date", HISTORICAL_CUTOFF),
            ("Sessions", metrics["sessions"]),
            ("Currency", metrics["currency"]),
            ("Initial equity (cents)", metrics["funded_equity_cents"]),
            ("Equity (cents)", metrics["final_equity_cents"]),
            ("Net P&L (cents)", metrics["net_pnl_cents"]),
            ("Cash (cents)", metrics["final_cash_cents"]),
            ("Units (Quantity6 raw)", metrics["final_units_raw"]),
            (
                "Cost basis (cents)",
                metrics["daily"][-1]["invested_value_cents"] - metrics["unrealized_pnl_cents"],
            ),
            ("Realized trading P&L (cents)", metrics["realized_trading_pnl_cents"]),
            ("Unrealized P&L (cents)", metrics["unrealized_pnl_cents"]),
            ("Dividends (cents)", metrics["dividends_cents"]),
            ("Fees (cents)", metrics["fees_cents"]),
            ("Closed trades", metrics["completed_round_trips"]),
            ("Maximum drawdown (fraction)", f"{drawdown.numerator}/{drawdown.denominator}"),
            ("Endpoint", "Mark (no synthetic sale)"),
        )
    ]


class HistoricalHTML(HTMLParser):
    """Check the visible historical table separately from metadata projections."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.sections = self.tables = 0
        self.in_section = self.in_table = False
        self.active, self.parts, self.cells, self.rows, self.summaries = None, [], [], [], []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == "section" and attributes.get("id") == "historical-section":
            require(not self.in_section, "nested historical HTML section")
            self.sections += 1
            self.in_section = True
        elif self.in_section and tag == "table" and attributes.get("id") == "historical-summary":
            self.tables += 1
            self.in_table = True
        elif self.in_table and tag == "tr":
            require(not self.cells, "incomplete historical HTML row")
        elif (self.in_table and tag in ("th", "td")) or (
            self.in_section and tag == "p" and attributes.get("id") == "historical-summary-text"
        ):
            require(
                self.active is None and "hidden" not in attributes,
                "nested/hidden historical table text",
            )
            self.active, self.parts = tag, []

    def handle_data(self, text):
        if self.active:
            self.parts.append(text)

    def handle_endtag(self, tag):
        if self.active == tag:
            text = "".join(self.parts)
            if tag == "p":
                self.summaries.append(text)
            else:
                self.cells.append(text)
            self.active, self.parts = None, []
        elif tag == "tr" and self.in_table:
            require(len(self.cells) == 2, "historical table must have label/value pairs")
            self.rows.append(tuple(self.cells))
            self.cells = []
        elif tag == "table" and self.in_table:
            self.in_table = False
        elif tag == "section" and self.in_section:
            self.in_section = False


def compare_historical_metadata(metadata, expected, html=None):
    metrics, summary = expected
    require(
        metadata["historical_section_profile"] == HISTORICAL_PROFILE
        and isinstance(metadata["historical_section_json"], str),
        "unknown historical report convention",
    )
    comparison = Comparison(metrics["rolling_43"]["count"] if "rolling_43" in metrics else 0)
    comparison.compare(parse_json(metadata["historical_section_json"]), metrics, "")
    require(
        metadata["historical_section_summary"] == summary,
        "historical human summary differs from the continuous prefix",
    )
    if html is not None:
        parser = HistoricalHTML()
        parser.feed(html)
        parser.close()
        require(
            parser.sections == 1
            and not parser.in_section
            and not parser.in_table
            and parser.active is None
            and parser.summaries == [summary],
            "missing/ambiguous historical HTML section",
        )
        require(
            parser.rows == historical_table(metrics)
            and parser.tables == int("status" not in metrics),
            "visible historical HTML table differs from independently reconstructed prefix",
        )
    return comparison


def identity_checks(document, dataset, config, trial_id, trial, header, risk):
    require(
        set(document) == {"schema", "financial_state", "metadata", "metrics"}
        and document["schema"] == "quadeplus-report-evidence-v1"
        and document["financial_state"] == "completed",
        "report is partial or unsupported",
    )
    d, rows = dataset.manifest, dataset.rows
    expected = dict(
        profile=config["profile"],
        trial_id=str(trial_id),
        strategy_id=trial["strategy"],
        strategy_configuration=trial["strategy_configuration"],
        experiment_sha256=header["experiment"],
        dataset_schema="quadeplus-daily-research-v1",
        dataset_id=d["version"],
        dataset_provenance=d["provenance"],
        adjustment_policy=d["adjustment"],
        adjustment_interpretation=ADJUSTMENT_INTERPRETATION,
        dataset_manifest_sha256=dataset.manifest_sha256,
        prices_sha256=d["prices_sha256"],
        sessions_sha256=d["sessions_sha256"],
        symbol=d["symbol"],
        currency=d["currency"],
        instrument_id=d["instrument"],
        venue_id=d["venue"],
        source_id=d["source"],
        numerical_origin=rows[0].day.isoformat(),
        numerical_end=rows[-1].day.isoformat(),
        numerical_source_rows=str(len(rows)),
        manifest_permitted_evaluation_begin=d["evaluation_begin"],
        manifest_permitted_evaluation_end=d["evaluation_end"],
        funding_date=rows[header["funding"]].day.isoformat(),
        funding_key=str(header["funding"]),
        evaluation_begin=config["evaluation_begin"],
        evaluation_end=config["evaluation_end"],
        evaluation_begin_key=str(header["begin"]),
        evaluation_end_key_exclusive=str(header["end"]),
        declared_evaluation_sessions=str(header["end"] - header["begin"]),
        execution_profile="price9_money2_exact_v1",
        dividend_profile="ex_date_cash_compatibility_v1",
        commission_bps=str(trial["commission"]),
        slippage_bps=str(trial["slippage"]),
        terminal_policy=config["terminal"],
        initial_cash_cents=str(trial["initial"]),
        lot_step_raw=str(trial["lot"]),
        drawdown_breaker_bps=str(trial["drawdown"]) if trial["drawdown"] else "disabled",
        daily_loss_breaker_bps=str(trial["daily_loss"]) if trial["daily_loss"] else "disabled",
        slippage_diagnostic="settled_gross_difference_v1;buy_executed_minus_raw;sell_raw_minus_executed;settle_each_gross_ties_even;already_embedded_in_equity",
        control_timing="funding_close_intent_may_fill_first_evaluation_open"
        if header["legacy"]
        else "first_evaluation_close_intent_may_fill_next_open",
        acceptance_authority="engine_required_evidence_and_existing_marker;report_is_not_acceptance",
    )
    metadata = document["metadata"]
    require(d["adjustment"] == ADJUSTMENT_POLICY, "unsupported report adjustment policy")
    require(
        set(metadata)
        == set(expected)
        | {
            "run_label",
            "window_status",
            "provenance",
            "risk_report_profile",
            "risk_events_json",
            "risk_outcome",
            "risk_gap_interpretation",
            "historical_section_profile",
            "historical_section_json",
            "historical_section_summary",
        },
        "metadata inventory differs",
    )
    for key, value in expected.items():
        require(metadata[key] == value, "report identity mismatch: " + key)
    for key in ("run_label", "window_status", "provenance"):
        require(isinstance(metadata[key], str) and metadata[key], "missing report label: " + key)
    return compare_risk_metadata(metadata, risk)


def verify_run(directory, dataset, expected_profile=None, expected_cost=None):
    started = time.monotonic()
    bundle = Bundle(directory)
    config = xml_document(bundle.read("effective-config.xml", 65536))
    require(
        config.tag == "backtest" and len(config) == 0 and config.get("version") == "1",
        "unsupported effective configuration",
    )
    config = dict(config.attrib)
    if expected_profile is not None:
        family = (
            "keltner"
            if config["strategy"] == "keltner_atr_risk_v1"
            else "sma"
            if config["strategy"] == "sma20_50_long_cash_v1"
            else (
                "stochastic-legacy"
                if config["profile"] == "legacy_spy_2026_v1"
                else "stochastic-fresh"
            )
        )
        require(
            family == expected_profile
            and config["commission_bps"] == config["slippage_bps"] == str(expected_cost),
            "suite child profile/cost mismatch",
        )
    lines = bundle.read("financial.txt", 128 * 1024 * 1024).decode().splitlines()
    require(len(lines) >= 5, "empty financial evidence")
    header = financial_header(lines, config, dataset)
    historical = historical_scope(dataset, header, config["profile"])
    events = parse_events(lines, header)
    require(
        {p.name for p in bundle.root.glob("report-*.json")}
        == {f"report-{trial}.json" for trial in header["trials"]},
        "missing/extra native trial reports",
    )
    result = dict(
        run=str(bundle.root),
        trials=0,
        financial_events=len(lines) - 4,
        close_observations=0,
        exact_checks=0,
        floating_checks=0,
        projection_files=0,
        risk_triggers=0,
        historical_sections=0,
    )
    for trial_id, trial in header["trials"].items():
        prefix = f"report-{trial_id}"
        try:
            inputs, historical_inputs = _reconstruct(
                events[trial_id], trial, header, dataset, historical[0]
            )
            document = bundle.json(prefix + ".json", 64 * 1024 * 1024)
            risk = project_risk(events[trial_id], trial, header, dataset)
            result["exact_checks"] += identity_checks(
                document, dataset, config, trial_id, trial, header, risk
            )
            result["risk_triggers"] += len(risk[0])
            expected = expected_metrics(dataset, trial, header, inputs)
            comparison = Comparison(expected["rolling_43"]["count"])
            comparison.compare(document["metrics"], expected)
            prefix_expected = historical_expected(
                dataset, trial, header, historical, historical_inputs
            )
            prefix_comparison = compare_historical_metadata(
                document["metadata"],
                prefix_expected,
                bundle.read(prefix + ".html", 64 * 1024 * 1024).decode(),
            )
            result["historical_sections"] += int(historical[0] is not None)
            result["exact_checks"] += prefix_comparison.exact
            result["floating_checks"] += prefix_comparison.approximate
            projections(bundle, prefix, document)
            result["trials"] += 1
            result["close_observations"] += len(inputs[0])
            result["exact_checks"] += comparison.exact
            result["floating_checks"] += comparison.approximate
            result["projection_files"] += 2
        except (ValueError, KeyError, TypeError, IndexError, ArithmeticError) as exc:
            raise ValueError(f"{bundle.root.name}/{prefix}: {exc}") from exc
    result["elapsed_seconds"] = round(time.monotonic() - started, 3)
    print(
        f"PASS {bundle.root.name}: {result['trials']} trials, {result['financial_events']} events, "
        f"{result['close_observations']} closes, {result['elapsed_seconds']:.3f}s",
        flush=True,
    )
    return result


def verify_suite(directory, dataset):
    suite = Bundle(directory)
    manifest = suite.json("manifest.json")
    require(
        manifest["schema"] == "quadeplus-suite-manifest"
        and manifest["version"] == 1
        and manifest["input_manifest_sha256"] == dataset.manifest_sha256,
        "suite dataset/schema mismatch",
    )
    children = manifest["children"]
    require(
        [(child["profile"], child["cost_bps"]) for child in children]
        == [(p, c) for p in PROFILES for c in COSTS],
        "suite must contain the complete 12-run matrix",
    )
    results = []
    for child in children:
        profile, cost = child["profile"], child["cost_bps"]
        require(child["path"] == f"runs/{profile}-{cost}", "noncanonical suite child path")
        require(
            suite.read(child["path"] + "/effective-config.xml", 65536)
            == suite.read(child["config_path"], 65536),
            "suite/effective child config mismatch",
        )
        results.append(verify_run(suite.root / child["path"], dataset, profile, cost))
    return results


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--dataset", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--run", type=Path)
    mode.add_argument("--suite", type=Path)
    args = parser.parse_args()
    try:
        dataset = load_dataset(args.dataset)
        results = (
            verify_suite(args.suite, dataset) if args.suite else [verify_run(args.run, dataset)]
        )
        totals = {
            key: sum(result[key] for result in results)
            for key in (
                "trials",
                "financial_events",
                "close_observations",
                "exact_checks",
                "floating_checks",
                "projection_files",
                "risk_triggers",
                "historical_sections",
            )
        }
        print(
            "VERIFIED reporting math: "
            + ", ".join(
                [f"runs={len(results)}"] + [f"{key}={value}" for key, value in totals.items()]
            )
        )
        print(
            f"Floating-only tolerance: relative={REL_TOL:g}, absolute={ABS_TOL:g}; exact metrics have no tolerance."
        )
        print(
            "Read-only numerical/report reconciliation; native acceptance and strategy-signal gates are separate."
        )
    except (ValueError, KeyError, TypeError, IndexError, ArithmeticError, OSError) as exc:
        print(f"REPORT VERIFICATION FAILED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
