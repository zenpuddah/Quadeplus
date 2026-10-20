#!/usr/bin/env python3
"""Fail-closed independent inspection of the bounded native no-cache suite.

Usage: python3 verify_suite.py SUITE_DIRECTORY
       python3 verify_suite.py --self-test
No dependency, subprocess, workspace mutation or second acceptance artifact.
"""

import csv
import copy
import datetime as dt
from decimal import Decimal
from fractions import Fraction
import hashlib
from html.parser import HTMLParser
from html import escape as html_escape
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import xml.etree.ElementTree as ET

PROFILES = ("sma", "keltner", "stochastic-fresh", "stochastic-legacy")
COSTS = (5, 10, 20)
SHA = re.compile(r"[0-9a-f]{64}\Z")
MAX_FILE = 512 * 1024 * 1024
MAX_METADATA = 4 * 1024 * 1024
MAX_REPORT = 64 * 1024 * 1024
csv.field_size_limit(MAX_REPORT)  # Historical metrics metadata is one bounded CSV string field.
STRATEGIES = {
    "sma": ["sma20_50_long_cash_v1"],
    "keltner": ["keltner_3_2", "keltner_2_1", "keltner_5_2", "keltner_2_1_duplicate"],
    "stochastic-fresh": ["spy_stochastic5_v1"],
    "stochastic-legacy": ["spy_stochastic5_v1"],
}


def require(value, message):
    if not value:
        raise ValueError(message)


def integer(value):
    require(type(value) is int and -(2**63) <= value < 2**63, "expected signed64 JSON integer")
    return value


def sha(value):
    require(isinstance(value, str) and SHA.fullmatch(value), "invalid SHA256")
    return value


def unique_object(pairs):
    out = {}
    for key, value in pairs:
        require(key not in out, "duplicate JSON key: " + key)
        out[key] = value
    return out


def parse_json(text):
    def invalid(value):
        raise ValueError("nonfinite JSON constant: " + value)

    return json.loads(
        text, object_pairs_hook=unique_object, parse_float=Decimal, parse_constant=invalid
    )


def xml_document(data):
    require(b"<!" not in data, "XML DTD/comments are not in the strict suite grammar")
    without_declaration = re.sub(rb'^\s*<\?xml version="1\.0"\?>', b"", data, count=1)
    require(b"<?" not in without_declaration, "unsupported XML declaration/processing instruction")
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True, insert_pis=True))
    root = ET.fromstring(data, parser=parser)
    for node in root.iter():
        require(
            isinstance(node.tag, str)
            and not (node.text or "").strip()
            and not (node.tail or "").strip(),
            "unexpected XML text/node",
        )
    return root


def relative_name(name):
    require(isinstance(name, str) and 0 < len(name) <= 4096, "invalid inventory pathname")
    require(
        "\\" not in name and all(ord(c) >= 32 and ord(c) != 127 for c in name),
        "unsafe inventory pathname",
    )
    parts = name.split("/")
    require(
        all(p not in ("", ".", "..") for p in parts) and not PurePosixPath(name).is_absolute(),
        "inventory path escape",
    )
    return name


class Bundle:
    def __init__(self, root):
        self.root = Path(os.path.abspath(root))
        current = self.root
        while True:
            require(not current.is_symlink(), "symlink in suite root path")
            if current == current.parent:
                break
            current = current.parent
        require(self.root.is_dir(), "suite root is not a directory")

    def path(self, name):
        relative_name(name)
        path = self.root
        for part in name.split("/"):
            path /= part
            require(not path.is_symlink(), "symlink in evidence path: " + name)
        require(path.is_file(), "missing/nonregular evidence: " + name)
        return path

    def read(self, name, maximum=MAX_METADATA):
        path = self.path(name)
        with path.open("rb") as source:
            before = os.fstat(source.fileno())
            require(
                stat.S_ISREG(before.st_mode) and before.st_size <= maximum,
                "file exceeds bounded read: " + name,
            )
            data = source.read(maximum + 1)
            after = os.fstat(source.fileno())
        require(
            len(data) == before.st_size
            and before.st_size == after.st_size
            and before.st_mtime_ns == after.st_mtime_ns,
            "file changed during read: " + name,
        )
        return data

    def json(self, name, maximum=MAX_METADATA):
        return parse_json(self.read(name, maximum).decode("utf-8"))

    def inventory(self):
        files, entries = set(), 0
        for parent, dirs, names in os.walk(self.root, followlinks=False):
            for name in dirs + names:
                entries += 1
                require(entries <= 544, "suite directory inventory exceeds bound")
                path = Path(parent) / name
                require(not path.is_symlink(), "symlink in inventory")
                mode = path.stat().st_mode
                require(stat.S_ISDIR(mode) or stat.S_ISREG(mode), "nonregular inventory entry")
                if stat.S_ISREG(mode):
                    files.add(path.relative_to(self.root).as_posix())
        return files

    def verify_file(self, entry):
        require(
            set(entry) == {"name", "format", "path", "bytes", "sha256"},
            "unsupported file inventory schema",
        )
        require(
            isinstance(entry["name"], str)
            and entry["name"]
            and isinstance(entry["format"], str)
            and entry["format"],
            "missing evidence name/format",
        )
        count = integer(entry["bytes"])
        require(0 <= count <= MAX_FILE, "invalid evidence length")
        expected = sha(entry["sha256"])
        path = self.path(entry["path"])
        digest, seen = hashlib.sha256(), 0
        with path.open("rb") as source:
            before = os.fstat(source.fileno())
            require(before.st_size == count, "evidence size mismatch: " + entry["path"])
            while True:
                block = source.read(1024 * 1024)
                if not block:
                    break
                seen += len(block)
                require(seen <= count, "evidence grew while hashing")
                digest.update(block)
            after = os.fstat(source.fileno())
        require(
            seen == count
            and digest.hexdigest() == expected
            and before.st_mtime_ns == after.st_mtime_ns
            and before.st_size == after.st_size,
            "evidence hash/readback mismatch: " + entry["path"],
        )


def inventory_map(entries):
    require(isinstance(entries, list) and 0 < len(entries) <= 512, "invalid inventory count")
    out = {}
    total = 0
    for entry in entries:
        name = relative_name(entry["path"])
        require(name not in out, "duplicate inventory pathname")
        out[name] = entry
        total += integer(entry["bytes"])
    require(total <= 16 * 1024**3, "suite inventory byte bound exceeded")
    return out


def native_build(value):
    require(
        set(value)
        == {
            "binary_sha256",
            "dependencies_sha256",
            "source_build_sha256",
            "sdk_build_contract_id",
            "platform",
        },
        "native build identity schema",
    )
    for name in ("binary_sha256", "dependencies_sha256", "source_build_sha256"):
        require(sha(value[name]) != "0" * 64, "empty native build digest")
    for name in ("sdk_build_contract_id", "platform"):
        require(
            isinstance(value[name], str) and 0 < len(value[name]) <= 4096,
            "native build string identity missing",
        )


def metric(value):
    require(
        isinstance(value, dict) and set(value) == {"value", "undefined_reason"},
        "metric must explicitly label undefined values",
    )
    number, reason = value["value"], value["undefined_reason"]
    if number is None:
        require(isinstance(reason, str) and reason, "undefined metric lacks reason")
        return None
    require(reason is None, "defined metric has an undefined reason")
    if isinstance(number, dict):
        require(set(number) == {"numerator", "denominator"}, "unknown exact-ratio encoding")
        n, d = integer(number["numerator"]), integer(number["denominator"])
        require(d > 0 and math.gcd(n, d) == 1, "ratio must be positive-denominator and reduced")
        return Fraction(n, d)
    require(
        type(number) in (int, Decimal) and (not isinstance(number, Decimal) or number.is_finite()),
        "metric is not finite",
    )
    return number


def all_metrics(value):
    if isinstance(value, dict):
        if "value" in value or "undefined_reason" in value:
            metric(value)
        else:
            for child in value.values():
                all_metrics(child)
    elif isinstance(value, list):
        for child in value:
            all_metrics(child)


def leaves(value, path=""):
    if isinstance(value, dict) and value:
        for key, child in value.items():
            yield from leaves(child, path + "/" + key.replace("~", "~0").replace("/", "~1"))
    elif isinstance(value, list) and value:
        for i, child in enumerate(value):
            yield from leaves(child, path + "/" + str(i))
    else:
        kind = (
            "null"
            if value is None
            else "boolean"
            if type(value) is bool
            else "string"
            if isinstance(value, str)
            else "array"
            if isinstance(value, list)
            else "object"
            if isinstance(value, dict)
            else "number"
        )
        yield path, (kind, value)


class ReportHTML(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.active, self.parts, self.pre, self.metadata, self.key = None, [], [], {}, None

    def handle_starttag(self, tag, attrs):
        if tag in ("pre", "dt", "dd"):
            require(self.active is None, "nested report HTML payload")
            self.active, self.parts = tag, []

    def handle_data(self, text):
        if self.active:
            self.parts.append(text)

    def handle_endtag(self, tag):
        if tag != self.active:
            return
        text = "".join(self.parts)
        if tag == "pre":
            self.pre.append(text)
        elif tag == "dt":
            require(self.key is None, "unpaired HTML metadata key")
            self.key = text
        else:
            require(
                self.key is not None and self.key not in self.metadata,
                "duplicate/unpaired HTML metadata",
            )
            self.metadata[self.key], self.key = text, None
        self.active = None


def projections(bundle, prefix, document):
    reader = csv.reader(
        io.StringIO(bundle.read(prefix + ".csv", MAX_REPORT).decode("utf-8"), newline=""),
        strict=True,
    )
    require(next(reader) == ["path", "type", "value"], "report CSV header mismatch")
    actual = {}
    for row in reader:
        require(len(row) == 3 and row[0] not in actual, "duplicate/malformed CSV leaf")
        path, kind, text = row
        value = text if kind == "string" else parse_json(text)
        require(
            kind in ("string", "number", "boolean", "null", "array", "object"),
            "unknown CSV leaf type",
        )
        require(
            kind == "string"
            or kind == "number"
            and type(value) in (int, Decimal)
            or kind == "boolean"
            and type(value) is bool
            or kind == "null"
            and value is None
            or kind == "array"
            and type(value) is list
            and not value
            or kind == "object"
            and type(value) is dict
            and not value,
            "CSV value contradicts declared leaf type",
        )
        actual[path] = (kind, value)
    require(actual == dict(leaves(document)), "CSV is not the complete report JSON projection")
    parser = ReportHTML()
    parser.feed(bundle.read(prefix + ".html", MAX_REPORT).decode("utf-8"))
    parser.close()
    require(
        parser.active is None and parser.key is None and len(parser.pre) == 1,
        "missing/ambiguous HTML payload",
    )
    require(
        dict(leaves(parse_json(parser.pre[0]))) == dict(leaves(document["metrics"]))
        and parser.metadata == document["metadata"],
        "HTML metrics/identity differ from JSON",
    )


def close_enough(actual, expected, name):
    require(
        actual is not None
        and math.isclose(float(actual), float(expected), rel_tol=2e-12, abs_tol=2e-14),
        name + " statistic mismatch",
    )


def validate_metrics(m, summary, legacy):
    require(
        m["schema"] == "quadeplus-native-report-preparatory-v1" and m["currency"] == "USD",
        "unknown report metric schema/currency",
    )
    all_metrics(m)
    field_map = {
        "final_cash_cents": "cash_cents",
        "final_units_raw": "units_raw",
        "final_equity_cents": "equity_cents",
        "net_pnl_cents": "pnl_cents",
        "realized_trading_pnl_cents": "realized_cents",
        "unrealized_pnl_cents": "unrealized_cents",
        "dividends_cents": "dividend_cents",
        "fees_cents": "fee_cents",
    }
    for name, source in field_map.items():
        require(integer(m[name]) == integer(summary[source]), "report/summary mismatch: " + name)
    require(integer(m["funded_equity_cents"]) == 1000000, "funding baseline differs from USD 10000")
    require(
        m["net_pnl_cents"]
        == m["final_equity_cents"] - m["funded_equity_cents"]
        == m["realized_trading_pnl_cents"] + m["unrealized_pnl_cents"] + m["dividends_cents"],
        "P&L decomposition failed",
    )
    require(
        m["final_cash_cents"] >= 0
        and m["final_units_raw"] >= 0
        and m["dividends_cents"] >= 0
        and m["fees_cents"] >= 0,
        "negative long/cash account quantity",
    )
    require(m["open_position"] is (m["final_units_raw"] > 0), "open-position flag mismatch")
    require(
        metric(m["total_return"]) == Fraction(m["net_pnl_cents"], 1000000),
        "total return baseline mismatch",
    )
    funding = "2026-08-14" if legacy else "2009-12-31"
    begin = "2026-08-17" if legacy else "2010-01-04"
    count = 43 if legacy else 4222
    require(
        m["funded_date"] == funding and m["final_date"] == "2026-10-15" and m["sessions"] == count,
        "profile funding/date/session endpoint mismatch",
    )
    require(
        m["terminal_policy"] == ("synthetic_cash_replacement" if legacy else "mark"),
        "wrong report terminal policy",
    )
    if legacy:
        require(
            m["terminal_cash_cents"] == m["final_cash_cents"] == m["final_equity_cents"]
            and m["final_units_raw"] == 0
            and m["unrealized_pnl_cents"] == 0,
            "synthetic endpoint not settled flat cash",
        )
    else:
        require(
            m["terminal_cash_cents"] is None
            and m["ordinary_final_equity_cents"] == m["final_equity_cents"],
            "mark endpoint was liquidated/replaced",
        )
    daily = m["daily"]
    require(
        isinstance(daily, list)
        and len(daily) == count
        and daily[0]["date"] == begin
        and daily[-1]["date"] == m["final_date"],
        "funding row included/missing evaluation rows",
    )
    previous, peak, maximum, invested, capital, returns = (
        1000000,
        1000000,
        Fraction(0),
        0,
        Fraction(0),
        [],
    )
    previous_date = dt.date.fromisoformat(funding)
    for i, row in enumerate(daily):
        date = dt.date.fromisoformat(row["date"])
        require(date > previous_date, "report dates are not strictly increasing")
        ordinary, equity = (
            integer(row["ordinary_equity_cents"]),
            integer(row["reporting_equity_cents"]),
        )
        require(
            ordinary >= 0 and equity >= 0 and (i == count - 1 and legacy or ordinary == equity),
            "unexpected equity replacement",
        )
        expected_return = Fraction(equity - previous, previous) if previous else None
        require(metric(row["daily_return"]) == expected_return, "daily return baseline mismatch")
        if expected_return is None:
            require(
                row["daily_return"]["undefined_reason"] == "zero_previous_equity",
                "wrong zero-equity undefined reason",
            )
        returns.append(expected_return)
        peak = max(peak, equity)
        drawdown = Fraction(peak - equity, peak)
        require(metric(row["drawdown"]) == drawdown, "daily drawdown mismatch")
        maximum = max(maximum, drawdown)
        units, value = integer(row["invested_units_raw"]), integer(row["invested_value_cents"])
        require(
            units >= 0 and 0 <= value <= ordinary and (units > 0 or value == 0),
            "invalid preterminal exposure",
        )
        fraction = Fraction(value, ordinary) if ordinary else None
        require(
            metric(row["close_capital_fraction"]) == fraction, "close exposure fraction mismatch"
        )
        invested += units > 0
        capital = capital + fraction if fraction is not None and capital is not None else None
        previous, previous_date = equity, date
    require(
        previous == m["final_equity_cents"]
        and daily[-1]["ordinary_equity_cents"] == m["ordinary_final_equity_cents"],
        "daily/report endpoint mismatch",
    )
    if not legacy:
        require(
            m["final_units_raw"] == daily[-1]["invested_units_raw"]
            and m["final_cash_cents"] + daily[-1]["invested_value_cents"]
            == m["final_equity_cents"],
            "cash plus marked position invariant failed",
        )
    require(metric(m["drawdown"]["maximum"]) == maximum, "maximum drawdown mismatch")
    require(
        metric(m["close_invested_fraction"]) == Fraction(invested, count),
        "funding/terminal row polluted exposure denominator",
    )
    if capital is not None:
        close_enough(metric(m["mean_close_capital_fraction"]), capital / count, "mean exposure")
    else:
        require(
            metric(m["mean_close_capital_fraction"]) is None
            and m["mean_close_capital_fraction"]["undefined_reason"] == "zero_close_equity",
            "zero-equity exposure must be undefined",
        )
    require(
        all(type(row["legacy_any_session_invested"]) is bool for row in daily),
        "missing any-session exposure evidence",
    )
    require(
        metric(m["legacy_any_session_exposure"])
        == Fraction(sum(row["legacy_any_session_invested"] for row in daily), count),
        "legacy exposure denominator mismatch",
    )
    gaps = [metric(row["overnight_asset_gap"]) for row in daily]
    require(
        all(gap is not None for gap in gaps)
        and m["observed_overnight_gap_sessions"] == count
        and metric(m["worst_overnight_asset_gap"]) == min(gaps),
        "overnight asset-gap coverage mismatch",
    )
    if all(r is not None for r in returns):
        mean = sum(returns, Fraction(0)) / count
        variance = sum(((r - mean) ** 2 for r in returns), Fraction(0)) / (count - 1)
        if variance:
            std = math.sqrt(float(variance))
            close_enough(metric(m["annualized_volatility"]), std * math.sqrt(252), "volatility")
            close_enough(
                metric(m["sharpe_zero_risk_free"]), float(mean) / std * math.sqrt(252), "Sharpe"
            )
        else:
            for name in ("annualized_volatility", "sharpe_zero_risk_free"):
                require(
                    metric(m[name]) is None
                    and m[name]["undefined_reason"] == "zero_return_variance",
                    "zero variance must be explicitly undefined",
                )
        require(metric(m["worst_daily_return"]) == min(returns), "worst daily return mismatch")
    else:
        for name in ("annualized_volatility", "sharpe_zero_risk_free", "worst_daily_return"):
            require(
                metric(m[name]) is None and m[name]["undefined_reason"] == "undefined_daily_return",
                "missing undefined-return propagation",
            )
    for name in (
        "completed_round_trips",
        "wins",
        "losses",
        "breakeven_trades",
        "rejected_orders",
        "expired_orders",
    ):
        require(integer(m[name]) >= 0, "negative event/trade count")
    require(
        len({r["reason"] for r in m["rejection_reasons"]}) == len(m["rejection_reasons"])
        and all(
            isinstance(r["reason"], str) and r["reason"] and integer(r["count"]) > 0
            for r in m["rejection_reasons"]
        )
        and sum(r["count"] for r in m["rejection_reasons"]) == m["rejected_orders"],
        "rejection counts do not reconcile",
    )
    require(
        integer(m["cumulative_gross_cents"]) >= 0
        and integer(m["slippage_estimate_cents"]) >= 0
        and metric(m["gross_turnover"]) == Fraction(m["cumulative_gross_cents"], 1000000),
        "cost/turnover totals mismatch",
    )
    require(
        m["completed_round_trips"] == m["wins"] + m["losses"] + m["breakeven_trades"],
        "trade counts do not reconcile",
    )
    if m["losses"] == 0:
        require(
            metric(m["profit_factor"]) is None
            and m["profit_factor"]["undefined_reason"] == "no_losing_trade_pnl",
            "profit factor with no losses must be undefined",
        )
    else:
        require(
            metric(m["profit_factor"]) is not None and metric(m["profit_factor"]) >= 0,
            "invalid defined profit factor",
        )
    days = (dt.date.fromisoformat(m["final_date"]) - dt.date.fromisoformat(funding)).days
    require(
        m["elapsed_calendar_days"] == days
        and m["annualization_sessions"] == 252
        and m["cagr_year_days"] == Decimal("365.25"),
        "annualization calendar mismatch",
    )
    if days < 365:
        require(
            metric(m["cagr"]) is None
            and m["cagr"]["undefined_reason"] == "fewer_than_365_calendar_days",
            "short cohort CAGR must be undefined",
        )
    else:
        close_enough(
            metric(m["cagr"]), (m["final_equity_cents"] / 1000000) ** (365.25 / days) - 1, "CAGR"
        )
    for field, size in (("monthly_returns", 7), ("yearly_returns", 4)):
        groups = {}
        for row in daily:
            groups.setdefault(row["date"][:size], []).append(row)
        require(len(m[field]) == len(groups), "period inventory mismatch")
        baseline = 1000000
        for reported, (period, rows) in zip(m[field], groups.items()):
            end = rows[-1]["reporting_equity_cents"]
            require(
                reported["period"] == period
                and reported["sessions"] == len(rows)
                and metric(reported["total_return"])
                == (Fraction(end - baseline, baseline) if baseline else None),
                "period returns reset/misalign account",
            )
            baseline = end
    rolling = m["rolling_43"]
    require(
        rolling["window_sessions"] == 43
        and rolling["count"] == count - 42
        and len(rolling["windows"]) == count - 42,
        "rolling-window count mismatch",
    )
    for i, window in enumerate(rolling["windows"]):
        baseline = 1000000 if i == 0 else daily[i - 1]["reporting_equity_cents"]
        end = daily[i + 42]["reporting_equity_cents"]
        require(
            window["start_date"] == daily[i]["date"]
            and window["end_date"] == daily[i + 42]["date"]
            and metric(window["total_return"])
            == (Fraction(end - baseline, baseline) if baseline else None),
            "rolling 43 window is not continuous-account return",
        )
    values = [metric(w["total_return"]) for w in rolling["windows"]]
    if all(value is not None for value in values):
        values.sort()
        n = len(values)
        median = values[n // 2] if n % 2 else (values[n // 2 - 1] + values[n // 2]) / 2
        close_enough(metric(rolling["median"]), median, "rolling median")
        require(
            metric(rolling["worst"]) == values[0]
            and metric(rolling["percentage_positive"])
            == Fraction(100 * sum(v > 0 for v in values), n)
            and metric(rolling["percentage_at_least_2pct"])
            == Fraction(100 * sum(v >= Fraction(1, 50) for v in values), n),
            "rolling summary mismatch",
        )
    else:
        for name in ("median", "worst", "percentage_positive", "percentage_at_least_2pct"):
            require(
                metric(rolling[name]) is None
                and rolling[name]["undefined_reason"] == "undefined_rolling_window",
                "undefined rolling summary mismatch",
            )


def verify_report(bundle, manifest, child, summary):
    prefix = child["path"] + "/report-" + str(summary["trial"])
    document = bundle.json(prefix + ".json", MAX_REPORT)
    require(
        set(document) == {"schema", "financial_state", "metadata", "metrics"}
        and document["schema"] == "quadeplus-report-evidence-v1"
        and document["financial_state"] == "completed",
        "partial/unsupported report evidence",
    )
    md = document["metadata"]
    legacy = child["profile"] == "stochastic-legacy"
    strategy = (
        "keltner_atr_risk_v1"
        if child["profile"] == "keltner" and summary["trial"] <= 4
        else summary["strategy"]
    )
    expected = {
        "profile": "legacy_spy_2026_v1" if legacy else "fresh_research_v1",
        "trial_id": str(summary["trial"]),
        "strategy_id": strategy,
        "experiment_sha256": child["experiment_sha256"],
        "dataset_manifest_sha256": manifest["input_manifest_sha256"],
        "currency": "USD",
        "symbol": "SPY",
        "dataset_schema": "quadeplus-daily-research-v1",
        "evaluation_end_key_exclusive": "5511",
        "numerical_origin": "2004-11-18",
        "funding_date": "2026-08-14" if legacy else "2009-12-31",
        "evaluation_begin": "2026-08-17" if legacy else "2010-01-04",
        "evaluation_end": "2026-10-15",
        "commission_bps": str(child["cost_bps"]),
        "slippage_bps": str(child["cost_bps"]),
        "initial_cash_cents": "1000000",
        "lot_step_raw": "1",
        "terminal_policy": "synthetic_close" if legacy else "mark",
        "window_status": "previously_inspected_research_not_holdout",
        "execution_profile": "price9_money2_exact_v1",
        "dividend_profile": "ex_date_cash_compatibility_v1",
        "adjustment_policy": "split_adjusted_ohlc_dividends_same_share_basis_v1",
        "adjustment_interpretation": "Split-adjusted OHLC; separate dividends on the same share basis; AdjClose is retained but not traded; no additional split postings.",
        "risk_report_profile": "trigger_order_gap_v1",
        "historical_section_profile": "continuous_account_through_2025_12_31_v1",
        "control_timing": "funding_close_intent_may_fill_first_evaluation_open"
        if legacy
        else "first_evaluation_close_intent_may_fill_next_open",
    }
    require(
        all(md.get(key) == value for key, value in expected.items()),
        "per-trial report identity/profile/cost mismatch",
    )
    if strategy == "keltner_atr_risk_v1":
        width = ("3/2", "2/1", "5/2", "2/1")[summary["trial"] - 1]
        require(
            md["strategy_configuration"] == f"width={width};risk=1/200;stop=2atr;entry_guard=v1"
            and md["drawdown_breaker_bps"] == "2000",
            "Keltner variant/risk identity mismatch",
        )
    else:
        require(md["drawdown_breaker_bps"] == "disabled", "unexpected baseline risk overlay")
    require(md["daily_loss_breaker_bps"] == "disabled", "unexpected daily-loss overlay")
    risk = parse_json(md["risk_events_json"])
    require(
        isinstance(risk, list) and len(risk) <= 1 and isinstance(md["risk_outcome"], str),
        "invalid latched-risk report",
    )
    if strategy != "keltner_atr_risk_v1":
        require(not risk, "baseline/control report contains an unsolicited risk action")
    for field in ("prices_sha256", "sessions_sha256"):
        sha(md[field])
    require(
        md["prices_sha256"] == "28963337b6eba735cad1eb62dc333d497079cff73f02076d17d3439c48e167ae",
        "report source is not pinned retained SPY",
    )
    validate_metrics(document["metrics"], summary, legacy)
    historical = parse_json(md["historical_section_json"])
    if legacy:
        require(
            historical.get("status") == "not_applicable"
            and historical.get("reason") == "legacy_profile"
            and historical.get("metrics") is None,
            "legacy cohort fabricated a pre-2026 historical account",
        )
    else:
        full = document["metrics"]
        require(
            historical["funded_date"] == full["funded_date"]
            and historical["funded_equity_cents"] == full["funded_equity_cents"]
            and historical["final_date"] == "2025-12-31"
            and historical["sessions"] == 4024,
            "historical cutoff reset funding or changed scope",
        )
        require(
            historical["daily"] == [row for row in full["daily"] if row["date"] <= "2025-12-31"],
            "historical section differs from continuous-account prefix",
        )
    projections(bundle, prefix, document)
    if summary["strategy"] == "cash_usd_no_interest_v1":
        require(
            summary["equity_cents"] == summary["cash_cents"] == 1000000
            and all(
                summary[name] == 0
                for name in (
                    "units_raw",
                    "pnl_cents",
                    "realized_cents",
                    "unrealized_cents",
                    "dividend_cents",
                    "fee_cents",
                )
            ),
            "cash control is not no-interest flat cash",
        )
    return document


def verify(root):
    bundle = Bundle(root)
    marker = bundle.json("acceptance.json")
    manifest = bundle.json("manifest.json")
    identity_fields = {"input_manifest_sha256", "suite_config_sha256", "native_build"}
    require(
        set(marker)
        == identity_fields
        | {
            "schema",
            "version",
            "status",
            "cache",
            "new_cache_origins",
            "manifest_sha256",
            "required_files",
        },
        "unknown marker fields",
    )
    require(
        set(manifest) == identity_fields | {"schema", "version", "children", "aggregate_files"},
        "unknown manifest fields",
    )
    require(
        marker["schema"] == "quadeplus-suite-acceptance"
        and type(marker["version"]) is int
        and marker["version"] == 1
        and marker["status"] == "Accepted"
        and marker["cache"] == "disabled"
        and integer(marker["new_cache_origins"]) == 0,
        "missing supported standalone suite acceptance",
    )
    require(
        manifest["schema"] == "quadeplus-suite-manifest"
        and type(manifest["version"]) is int
        and manifest["version"] == 1,
        "unsupported suite manifest",
    )
    for field in ("input_manifest_sha256", "suite_config_sha256", "native_build"):
        require(marker[field] == manifest[field], "marker/manifest identity mismatch")
    sha(manifest["input_manifest_sha256"])
    sha(manifest["suite_config_sha256"])
    native_build(manifest["native_build"])
    inventory = inventory_map(marker["required_files"])
    require(
        "acceptance.json" not in inventory and "manifest.json" in inventory,
        "marker inventory is self-referential or lacks manifest",
    )
    require(
        inventory["manifest.json"]["sha256"] == sha(marker["manifest_sha256"]),
        "manifest hash identity mismatch",
    )
    require(
        bundle.inventory() == set(inventory) | {"acceptance.json"},
        "complete file inventory mismatch",
    )
    for entry in inventory.values():
        bundle.verify_file(entry)
    aggregates = inventory_map(manifest["aggregate_files"])
    needed = {"suite.xml", "registry.csv", "cost-comparison.csv", "index.html"}
    needed |= {"configs/source-" + name + ".xml" for name in PROFILES}
    needed |= {f"configs/{name}-{cost}.xml" for name in PROFILES for cost in COSTS}
    require(set(aggregates) == needed, "aggregate/config inventory mismatch")
    require(
        aggregates["suite.xml"]["sha256"] == manifest["suite_config_sha256"],
        "suite configuration SHA mismatch",
    )
    suite_xml = xml_document(bundle.read("suite.xml", 65536))
    require(
        suite_xml.tag == "suite"
        and suite_xml.attrib == {"version": "1", "costs": "5,10,20", "cache": "disabled"}
        and len(suite_xml) == 4,
        "suite XML grammar mismatch",
    )
    for node, name in zip(suite_xml, PROFILES):
        require(
            node.tag == "profile"
            and len(node) == 0
            and set(node.attrib) == {"name", "config"}
            and node.get("name") == name
            and node.get("config"),
            "suite XML profile grammar mismatch",
        )
    union = dict(aggregates)
    children = manifest["children"]
    require(
        len(children) == 12
        and [(c["profile"], c["cost_bps"]) for c in children]
        == [(p, c) for p in PROFILES for c in COSTS],
        "incomplete or duplicate profile/cost matrix",
    )
    registry_expected, comparison_expected, reports = [], [], {}
    experiments = set()
    index = bundle.read("index.html").decode("utf-8")
    for child in children:
        require(
            set(child)
            == {
                "profile",
                "cost_bps",
                "path",
                "config_path",
                "config_sha256",
                "experiment_sha256",
                "facts",
                "required_files",
                "trials",
            },
            "unknown child manifest fields",
        )
        profile, cost = child["profile"], integer(child["cost_bps"])
        count = 6 if profile == "keltner" else 3
        require(
            child["path"] == f"runs/{profile}-{cost}"
            and child["config_path"] == f"configs/{profile}-{cost}.xml",
            "noncanonical child path",
        )
        require(
            sha(child["experiment_sha256"]) not in experiments,
            "duplicate financial experiment identity",
        )
        experiments.add(child["experiment_sha256"])
        files = inventory_map(child["required_files"])
        require(
            len(files) <= 64 and len({f["name"] for f in files.values()}) == len(files),
            "duplicate/oversized child required inventory",
        )
        facts = {
            "numerical": "Completed",
            "integration": "Completed",
            "acceptance": "Accepted",
            "cache": "Disabled",
            "new_cache_origins": 0,
            "financial_exhausted": True,
            "financial_finalized": True,
            "required_count": len(files),
        }
        require(
            child["facts"] == facts
            and type(child["facts"]["financial_exhausted"]) is bool
            and type(child["facts"]["financial_finalized"]) is bool,
            "child acceptance facts are not complete",
        )
        require(
            integer(child["facts"]["new_cache_origins"]) == 0
            and integer(child["facts"]["required_count"]) == len(files),
            "child count fact type mismatch",
        )
        for path, entry in files.items():
            require(
                path.startswith(child["path"] + "/") and path not in union,
                "child path escaped/collided",
            )
            union[path] = entry
        basics = (
            "financial.txt",
            "effective-config.xml",
            "graph.xml",
            "summary.json",
            "retention.log",
        )
        if profile != "keltner":
            basics += ("input-identity.txt",)
        for name in basics:
            require(child["path"] + "/" + name in files, "missing required basic child evidence")
        graph = xml_document(bundle.read(child["path"] + "/graph.xml"))
        sources, caches = graph.findall("sources/source"), graph.findall("cache")
        require(
            graph.tag == "composition"
            and len(sources) == len(caches) == 1
            and sources[0].get("source_version") == manifest["input_manifest_sha256"]
            and caches[0].get("mode") == "disabled",
            "graph source manifest/cache identity mismatch",
        )
        effective = bundle.read(child["config_path"], 65536)
        require(
            hashlib.sha256(effective).hexdigest() == sha(child["config_sha256"])
            and effective == bundle.read(child["path"] + "/effective-config.xml", 65536),
            "child effective configuration differs from suite",
        )
        config = xml_document(effective)
        require(
            config.tag == "backtest"
            and config.get("cache") == "disabled"
            and config.get("commission_bps") == config.get("slippage_bps") == str(cost),
            "child costs/cache differ from suite",
        )
        source_config = xml_document(bundle.read("configs/source-" + profile + ".xml", 65536))
        expected_config = dict(
            source_config.attrib, commission_bps=str(cost), slippage_bps=str(cost)
        )
        require(
            len(config) == len(source_config) == 0
            and source_config.tag == "backtest"
            and config.attrib == expected_config,
            "suite changed non-cost child configuration",
        )
        require(
            len(child["trials"]) == count
            and [s["trial"] for s in child["trials"]] == list(range(1, count + 1)),
            "trial/control inventory mismatch",
        )
        require(
            [s["strategy"] for s in child["trials"]]
            == STRATEGIES[profile] + ["cash_usd_no_interest_v1", "buy_hold95_next_open_v1"],
            "missing strategy variants or matched controls",
        )
        basic = bundle.json(child["path"] + "/summary.json")
        require(
            basic["financial_state"] == "completed"
            and basic["dataset_sha256"] == manifest["input_manifest_sha256"]
            and len(basic["trials"]) == count,
            "basic child summary identity mismatch",
        )
        for scalar, basic_trial in zip(child["trials"], basic["trials"]):
            require(
                basic_trial["id"] == scalar["trial"]
                and all(basic_trial[k] == v for k, v in scalar.items() if k != "trial"),
                "basic and suite scalar summaries disagree",
            )
        baseline = next(c for c in children if c["profile"] == profile and c["cost_bps"] == 5)
        for i, summary in enumerate(child["trials"]):
            for ext in ("html", "json", "csv"):
                path = child["path"] + f"/report-{summary['trial']}.{ext}"
                require(
                    path in files
                    and files[path]["name"] == f"report-{summary['trial']}-{ext}"
                    and files[path]["format"] == f"quadeplus_report_evidence_{ext}_v1",
                    "report omitted from required closed evidence",
                )
            report = verify_report(bundle, manifest, child, summary)
            # Keep only digests for cross-child comparisons; each full report is
            # released before the next child. Decimal formatting remains exact.
            reports[profile, cost, summary["trial"]] = hashlib.sha256(
                json.dumps(report["metrics"], sort_keys=True, default=str).encode()
            ).hexdigest()
            link = child["path"] + f"/report-{summary['trial']}.html"
            require('href="' + link + '"' in index, "suite index omits required report")
            registry_expected.append(
                [profile, str(cost), str(summary["trial"]), summary["strategy"]]
                + [
                    str(summary[k])
                    for k in (
                        "equity_cents",
                        "pnl_cents",
                        "realized_cents",
                        "unrealized_cents",
                        "dividend_cents",
                        "fee_cents",
                    )
                ]
                + [link]
            )
            comparison_expected.append(
                [
                    profile,
                    str(cost),
                    str(summary["trial"]),
                    summary["strategy"],
                    str(summary["equity_cents"]),
                    str(summary["equity_cents"] - baseline["trials"][i]["equity_cents"]),
                    str(summary["equity_cents"] - child["trials"][-2]["equity_cents"]),
                    str(summary["equity_cents"] - child["trials"][-1]["equity_cents"]),
                ]
            )
    union["manifest.json"] = inventory["manifest.json"]
    require(union == inventory, "marker and manifest complete inventories disagree")
    headers = {
        "registry.csv": "profile,cost_bps,trial,strategy,equity_cents,pnl_cents,realized_cents,unrealized_cents,dividend_cents,fee_cents,report",
        "cost-comparison.csv": "profile,cost_bps,trial,strategy,equity_cents,delta_vs_base_cents,delta_vs_cash_cents,delta_vs_buy_hold_cents",
    }
    for name, expected in (
        ("registry.csv", registry_expected),
        ("cost-comparison.csv", comparison_expected),
    ):
        rows = list(csv.reader(io.StringIO(bundle.read(name).decode("utf-8")), strict=True))
        require(
            rows == [headers[name].split(",")] + expected,
            "aggregate CSV differs from child financial evidence",
        )
    # Same fresh timing/costs/endpoint requires identical financial control outcomes.
    for cost in COSTS:
        for control_offset in (0, 1):
            reference = reports["sma", cost, 2 + control_offset]
            for profile, trial in (
                ("keltner", 5 + control_offset),
                ("stochastic-fresh", 2 + control_offset),
            ):
                require(
                    reports[profile, cost, trial] == reference,
                    "matched fresh controls differ across strategies",
                )
        require(
            reports["keltner", cost, 2] == reports["keltner", cost, 4],
            "duplicate Keltner 2/1 trials are not independent equivalent accounts",
        )
    return {
        "schema": "quadeplus-suite-verification",
        "version": 1,
        "verified": True,
        "suite_directory": str(bundle.root),
        "manifest_sha256": marker["manifest_sha256"],
        "input_manifest_sha256": manifest["input_manifest_sha256"],
        "native_build": manifest["native_build"],
        "children": 12,
        "trials": len(reports),
        "required_files": len(inventory),
        "checks": [
            "complete_inventory",
            "closed_hashes",
            "no_path_escape",
            "cost_matrix",
            "native_identity",
            "required_reports",
            "report_projections",
            "pnl_reconciliation",
            "funding_and_endpoints",
            "undefined_metrics",
            "continuous_returns",
            "controls_and_duplicate_trials",
        ],
        "scope": "Retained-byte and financial-report consistency; strategy/ledger oracle gates remain separate. Legacy20 is diagnostic, not original archive float-ledger parity.",
    }


def financial_self_test():
    # A separately constructed 43-session flat-cash report exercises the public
    # verifier boundary without calling native report calculations or touching disk.
    def ratio(n=0, d=1):
        value = Fraction(n, d)
        return {
            "value": {"numerator": value.numerator, "denominator": value.denominator},
            "undefined_reason": None,
        }

    def undefined(reason):
        return {"value": None, "undefined_reason": reason}

    dates = []
    date = dt.date(2026, 8, 17)
    while date <= dt.date(2026, 10, 15):
        if date.weekday() < 5 and date != dt.date(2026, 9, 7):
            dates.append(date.isoformat())
        date += dt.timedelta(days=1)
    require(len(dates) == 43, "self-test declared session grid")
    summary = {
        "trial": 2,
        "strategy": "cash_usd_no_interest_v1",
        "cash_cents": 1000000,
        "units_raw": 0,
        "equity_cents": 1000000,
        "pnl_cents": 0,
        "realized_cents": 0,
        "unrealized_cents": 0,
        "dividend_cents": 0,
        "fee_cents": 0,
    }
    m = {
        "schema": "quadeplus-native-report-preparatory-v1",
        "currency": "USD",
        "funded_date": "2026-08-14",
        "funded_equity_cents": 1000000,
        "final_date": dates[-1],
        "elapsed_calendar_days": 62,
        "sessions": 43,
        "terminal_policy": "synthetic_cash_replacement",
        "terminal_cash_cents": 1000000,
        "ordinary_final_equity_cents": 1000000,
        "final_equity_cents": 1000000,
        "net_pnl_cents": 0,
        "realized_trading_pnl_cents": 0,
        "unrealized_pnl_cents": 0,
        "dividends_cents": 0,
        "final_cash_cents": 1000000,
        "final_units_raw": 0,
        "open_position": False,
        "fees_cents": 0,
        "slippage_estimate_cents": 0,
        "cumulative_gross_cents": 0,
        "rejected_orders": 0,
        "expired_orders": 0,
        "rejection_reasons": [],
        "completed_round_trips": 0,
        "wins": 0,
        "losses": 0,
        "breakeven_trades": 0,
        "observed_overnight_gap_sessions": 43,
        "annualization_sessions": 252,
        "cagr_year_days": Decimal("365.25"),
        "annualized_volatility": undefined("zero_return_variance"),
        "sharpe_zero_risk_free": undefined("zero_return_variance"),
        "cagr": undefined("fewer_than_365_calendar_days"),
        "profit_factor": undefined("no_losing_trade_pnl"),
        "drawdown": {
            "maximum": ratio(),
            "maximum_episode": None,
            "episodes": [],
            "still_underwater": False,
        },
    }
    for name in (
        "total_return",
        "gross_turnover",
        "worst_daily_return",
        "worst_overnight_asset_gap",
        "close_invested_fraction",
        "mean_close_capital_fraction",
        "legacy_any_session_exposure",
    ):
        m[name] = ratio()
    m["daily"] = [
        {
            "date": date,
            "ordinary_equity_cents": 1000000,
            "reporting_equity_cents": 1000000,
            "invested_units_raw": 0,
            "invested_value_cents": 0,
            "legacy_any_session_invested": False,
            "daily_return": ratio(),
            "drawdown": ratio(),
            "close_capital_fraction": ratio(),
            "overnight_asset_gap": ratio(),
        }
        for date in dates
    ]
    for field, size in (("monthly_returns", 7), ("yearly_returns", 4)):
        periods = sorted({date[:size] for date in dates})
        m[field] = [
            {
                "period": p,
                "sessions": sum(date.startswith(p) for date in dates),
                "total_return": ratio(),
            }
            for p in periods
        ]
    m["rolling_43"] = {
        "window_sessions": 43,
        "count": 1,
        "windows": [{"start_date": dates[0], "end_date": dates[-1], "total_return": ratio()}],
        "median": ratio(),
        "worst": ratio(),
        "percentage_positive": ratio(),
        "percentage_at_least_2pct": ratio(),
    }
    validate_metrics(m, summary, True)
    corruptions = [
        ("net_pnl_cents", 1),
        ("sessions", 44),
        ("funded_equity_cents", 1000001),
        ("terminal_cash_cents", 999999),
        ("funded_date", "2026-08-17"),
        ("profit_factor", ratio()),
        ("sharpe_zero_risk_free", ratio()),
        ("cagr", ratio()),
        ("final_units_raw", 1),
        ("gross_turnover", ratio(1, 10)),
        ("annualization_sessions", 365),
    ]
    for key, value in corruptions:
        changed = copy.deepcopy(m)
        changed[key] = value
        try:
            validate_metrics(changed, summary, True)
        except ValueError:
            pass
        else:
            raise ValueError("financial self-test accepted corruption: " + key)
    changed = copy.deepcopy(m)
    changed["daily"][0]["daily_return"] = ratio(1, 100)
    try:
        validate_metrics(changed, summary, True)
    except ValueError:
        pass
    else:
        raise ValueError("financial self-test accepted a wrong first-return baseline")
    return len(corruptions) + 1


def self_test():
    failures = 0
    for function, value in (
        (parse_json, '{"x":1,"x":2}'),
        (parse_json, '{"x":NaN}'),
        (relative_name, "../escape"),
        (relative_name, "/absolute"),
        (relative_name, "a//b"),
        (relative_name, "a/./b"),
        (relative_name, "a\\b"),
        (integer, True),
        (metric, {"value": None, "undefined_reason": None}),
        (metric, {"value": {"numerator": 2, "denominator": 4}, "undefined_reason": None}),
        (metric, {"value": {"numerator": 1, "denominator": 0}, "undefined_reason": None}),
        (metric, {"value": 1, "undefined_reason": "not undefined"}),
    ):
        try:
            function(value)
        except (ValueError, TypeError):
            failures += 1
        else:
            raise ValueError("negative self-test admitted malformed input")
    require(
        metric({"value": {"numerator": -1, "denominator": 2}, "undefined_reason": None})
        == Fraction(-1, 2),
        "ratio parser self-test",
    )
    require(
        metric({"value": None, "undefined_reason": "zero_return_variance"}) is None,
        "undefined parser self-test",
    )
    require(parse_json('{"x":0.1}')["x"] == Decimal("0.1"), "decimal parser self-test")
    require(
        relative_name("runs/sma-5/report-1.json") == "runs/sma-5/report-1.json",
        "relative path self-test",
    )
    financial_failures = financial_self_test()
    document = {
        "schema": "quadeplus-report-evidence-v1",
        "financial_state": "completed",
        "metadata": {"label": 'A<&"B'},
        "metrics": {"empty": [], "absent": None, "text": "two\nlines", "value_array": [1, True]},
    }
    table = io.StringIO(newline="")
    writer = csv.writer(table)
    writer.writerow(["path", "type", "value"])
    for path, (kind, value) in leaves(document):
        writer.writerow([path, kind, value if kind == "string" else json.dumps(value)])
    page = (
        "<html><dl><dt>label</dt><dd>"
        + html_escape(document["metadata"]["label"])
        + "</dd></dl><pre>"
        + html_escape(json.dumps(document["metrics"]))
        + "</pre></html>"
    )

    class InMemoryReport:
        def __init__(self, csv_text, html_text):
            self.csv_text, self.html_text = csv_text, html_text

        def read(self, name, maximum):
            value = self.csv_text if name.endswith(".csv") else self.html_text
            require(len(value.encode()) <= maximum, "self-test projection bound")
            return value.encode()

    projections(InMemoryReport(table.getvalue(), page), "report", document)
    projection_failures = 0
    for csv_text, html_text in (
        (table.getvalue() + '"/schema","string","duplicate"\r\n', page),
        (table.getvalue(), page.replace("<dt>label</dt>", "<dt>wrong</dt>")),
        (table.getvalue().replace("two\nlines", "changed"), page),
    ):
        try:
            projections(InMemoryReport(csv_text, html_text), "report", document)
        except ValueError:
            projection_failures += 1
        else:
            raise ValueError("projection self-test admitted mismatched evidence")
    return {
        "schema": "quadeplus-suite-verifier-self-test",
        "version": 1,
        "passed": True,
        "negative_cases": failures,
        "financial_negative_cases": financial_failures,
        "projection_negative_cases": projection_failures,
        "scope": "Parser/path/metric, independent flat-cash report and HTML/CSV projection checks; no native suite executed",
    }


def main(argv):
    try:
        require(len(argv) == 1, "usage: verify_suite.py SUITE_DIRECTORY | --self-test")
        result = self_test() if argv[0] == "--self-test" else verify(argv[0])
    except Exception as error:
        print(
            json.dumps(
                {
                    "schema": "quadeplus-suite-verification",
                    "version": 1,
                    "verified": False,
                    "error": type(error).__name__ + ": " + str(error),
                },
                sort_keys=True,
            )
        )
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
