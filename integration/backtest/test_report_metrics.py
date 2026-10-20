"""Hand-calculated reporting cases; no native runner or provider dependency."""

from dataclasses import replace
from datetime import date, timedelta
from fractions import Fraction
from html import unescape
import csv
import copy
import io
import json
import math
import os
from pathlib import Path
import subprocess
import unittest

try:
    from .report_metrics import (
        AccountTotals,
        CloseEquity,
        calculate_metrics,
        render_html,
        render_json,
    )
except ImportError:  # Also support direct execution and unittest discovery -s.
    from report_metrics import (
        AccountTotals,
        CloseEquity,
        calculate_metrics,
        render_html,
        render_json,
    )


class ReportMetricsTests(unittest.TestCase):
    def report(self, equities, *, initial=10000, funding=date(2025, 1, 1), dates=None):
        if dates is None:
            dates = [funding + timedelta(days=i + 1) for i in range(len(equities))]
        rows = [CloseEquity(day, equity, 0, 0) for day, equity in zip(dates, equities)]
        return calculate_metrics(
            rows,
            funded_equity_cents=initial,
            funded_date=funding,
            totals=AccountTotals(equities[-1]),
            closed_trade_pnls_cents=[equities[-1] - initial],
        )

    def test_hand_ledger_pnl_and_dividends_not_double_subtracted(self):
        report = calculate_metrics(
            [
                CloseEquity(date(2025, 1, 2), 1000000, 95000000, 950000, True),
                CloseEquity(date(2025, 1, 3), 1104500, 0, 0, True),
            ],
            funded_equity_cents=1000000,
            funded_date=date(2025, 1, 1),
            totals=AccountTotals(
                1104500,
                dividends_cents=9500,
                fees_cents=100,
                slippage_estimate_cents=25,
                cumulative_gross_cents=1995000,
            ),
            closed_trade_pnls_cents=[95000],
        )
        self.assertEqual(report["net_pnl_cents"], 104500)
        self.assertEqual(report["total_return"].value, Fraction(209, 2000))
        self.assertEqual(report["realized_trading_pnl_cents"], 95000)
        self.assertEqual(report["dividends_cents"], 9500)
        self.assertEqual(report["gross_turnover"].value, Fraction(399, 200))
        self.assertEqual(report["fees_cents"], 100)
        self.assertEqual(report["close_invested_fraction"].value, Fraction(1, 2))
        self.assertEqual(report["mean_close_capital_fraction"].value, Fraction(19, 40))
        self.assertEqual(report["legacy_any_session_exposure"].value, 1)

    def test_exact_large_integer_returns(self):
        initial = 2**60
        report = self.report([initial + 1], initial=initial)
        self.assertEqual(report["net_pnl_cents"], 1)
        self.assertEqual(report["total_return"].value, Fraction(1, initial))
        self.assertEqual(report["daily"][0]["daily_return"].value, Fraction(1, initial))
        payload = json.loads(render_json(report))
        self.assertEqual(payload["total_return"]["value"], dict(numerator=1, denominator=initial))

    def test_flat_equity_explicit_undefined_metrics(self):
        report = self.report([10000, 10000, 10000])
        self.assertEqual(report["total_return"].value, 0)
        self.assertEqual(report["drawdown"]["maximum"].value, 0)
        self.assertIsNone(report["drawdown"]["maximum_episode"])
        self.assertFalse(report["drawdown"]["still_underwater"])
        for key in ("annualized_volatility", "sharpe_zero_risk_free"):
            self.assertIsNone(report[key].value)
            self.assertEqual(report[key].undefined_reason, "zero_return_variance")
        self.assertEqual(report["profit_factor"].undefined_reason, "no_losing_trade_pnl")
        self.assertEqual(
            report["legacy_any_session_exposure"].undefined_reason,
            "missing_legacy_session_exposure",
        )
        self.assertEqual(report["rolling_43"]["median"].undefined_reason, "fewer_than_43_sessions")
        self.assertEqual(report["cagr"].undefined_reason, "fewer_than_365_calendar_days")
        self.assertIsNone(json.loads(render_json(report))["sharpe_zero_risk_free"]["value"])

    def test_one_return_has_insufficient_sample(self):
        report = self.report([11000])
        self.assertEqual(report["sessions"], 1)
        self.assertEqual(report["annualized_volatility"].undefined_reason, "fewer_than_two_returns")
        self.assertEqual(report["sharpe_zero_risk_free"].undefined_reason, "fewer_than_two_returns")

    def test_sample_volatility_sharpe_252_zero_risk_free(self):
        # +10%, +20%; mean .15, sample variance .005, not population .0025.
        report = self.report([11000, 13200])
        self.assertAlmostEqual(report["annualized_volatility"].value, math.sqrt(1.26))
        self.assertAlmostEqual(
            report["sharpe_zero_risk_free"].value, 0.15 / math.sqrt(0.005) * math.sqrt(252)
        )
        self.assertEqual(report["worst_daily_return"].value, Fraction(1, 10))
        symmetric = self.report([11000, 9900])
        self.assertAlmostEqual(symmetric["annualized_volatility"].value, math.sqrt(5.04))
        self.assertEqual(symmetric["sharpe_zero_risk_free"].value, 0)

    def test_nonzero_constant_returns_have_zero_variance(self):
        report = self.report([11000, 12100, 13310])
        self.assertEqual(report["annualized_volatility"].undefined_reason, "zero_return_variance")
        self.assertEqual(report["sharpe_zero_risk_free"].undefined_reason, "zero_return_variance")

    def test_terminal_replaces_last_session_and_preserves_exposure(self):
        rows = [
            CloseEquity(date(2025, 1, 2), 11000, 1000000, 10000, True),
            CloseEquity(date(2025, 1, 3), 10500, 1000000, 9500, True),
        ]
        ordinary = calculate_metrics(
            rows,
            funded_equity_cents=10000,
            funded_date=date(2025, 1, 1),
            totals=AccountTotals(1000, unrealized_pnl_cents=500),
            closed_trade_pnls_cents=[],
        )
        terminal = calculate_metrics(
            rows,
            funded_equity_cents=10000,
            funded_date=date(2025, 1, 1),
            totals=AccountTotals(9900, fees_cents=600),
            closed_trade_pnls_cents=[-100],
            terminal_cash_cents=9900,
        )
        self.assertEqual(ordinary["drawdown"]["maximum"].value, Fraction(1, 22))
        self.assertEqual(terminal["drawdown"]["maximum"].value, Fraction(1, 10))
        self.assertEqual(terminal["daily"][-1]["daily_return"].value, Fraction(-1, 10))
        self.assertEqual(terminal["net_pnl_cents"], -100)
        self.assertEqual(terminal["total_return"].value, Fraction(-1, 100))
        self.assertEqual(terminal["sessions"], 2)
        self.assertEqual(len(terminal["daily"]), 2)
        self.assertEqual(terminal["monthly_returns"][0]["total_return"].value, Fraction(-1, 100))
        self.assertEqual(terminal["close_invested_fraction"].value, 1)
        self.assertEqual(terminal["mean_close_capital_fraction"].value, Fraction(419, 462))
        self.assertEqual(
            terminal["mean_close_capital_fraction"], ordinary["mean_close_capital_fraction"]
        )
        self.assertTrue(ordinary["open_position"])
        self.assertFalse(terminal["open_position"])
        self.assertEqual(terminal["final_units_raw"], 0)
        self.assertEqual(rows[-1].equity_cents, 10500)
        self.assertEqual(terminal["daily"][-1]["ordinary_equity_cents"], 10500)
        # This API produces measurements only, never retroactive risk actions.
        self.assertNotIn("risk_actions", terminal)

    def test_first_close_terminal_fees_draw_down_from_funding(self):
        report = calculate_metrics(
            [CloseEquity(date(2025, 1, 2), 10000, 1000000, 9500, True)],
            funded_equity_cents=10000,
            funded_date=date(2025, 1, 1),
            totals=AccountTotals(9900, fees_cents=100),
            closed_trade_pnls_cents=[-100],
            terminal_cash_cents=9900,
        )
        self.assertEqual(report["drawdown"]["maximum"].value, Fraction(1, 100))
        self.assertEqual(report["drawdown"]["maximum_episode"]["duration_sessions"], 1)
        self.assertEqual(report["close_invested_fraction"].value, 1)
        self.assertEqual(report["mean_close_capital_fraction"].value, Fraction(19, 20))

    def test_recovery_durations_include_weekends_and_equality(self):
        dates = [date(2025, 1, 3), date(2025, 1, 6), date(2025, 1, 7), date(2025, 1, 8)]
        report = self.report([12000, 9600, 10800, 12000], dates=dates)
        episode = report["drawdown"]["maximum_episode"]
        self.assertEqual(report["drawdown"]["maximum"].value, Fraction(1, 5))
        self.assertEqual(episode["peak_date"], dates[0])
        self.assertEqual(episode["trough_date"], dates[1])
        self.assertEqual(episode["recovery_date"], dates[3])
        self.assertEqual(episode["duration_sessions"], 3)
        self.assertEqual(episode["duration_calendar_days"], 5)
        self.assertFalse(episode["still_underwater"])

    def test_maximum_episode_distinct_from_current_underwater(self):
        report = self.report([12000, 9600, 12000, 11900])
        self.assertFalse(report["drawdown"]["maximum_episode"]["still_underwater"])
        self.assertTrue(report["drawdown"]["still_underwater"])
        current = report["drawdown"]["episodes"][-1]
        self.assertEqual(current["duration_sessions"], 1)
        self.assertIsNone(current["recovery_date"])
        deeper = self.report([12000, 9600, 12000, 9000, 9100])
        maximum = deeper["drawdown"]["maximum_episode"]
        self.assertEqual(maximum["depth"].value, Fraction(1, 4))
        self.assertTrue(maximum["still_underwater"])
        self.assertEqual(maximum["duration_sessions"], 2)

    def test_peak_touches_and_equal_maxima_have_declared_ties(self):
        report = self.report([12000, 12000, 9600, 9600, 12000, 9600])
        episode = report["drawdown"]["maximum_episode"]
        self.assertEqual(episode["peak_date"], date(2025, 1, 3))
        self.assertEqual(episode["trough_date"], date(2025, 1, 4))
        self.assertEqual(episode["recovery_date"], date(2025, 1, 6))
        self.assertEqual(episode["duration_sessions"], 3)

    def test_cagr_threshold_and_actual_calendar_basis(self):
        funding = date(2024, 1, 1)
        short = self.report([12100], funding=funding, dates=[funding + timedelta(days=364)])
        self.assertIsNone(short["cagr"].value)
        for days in (365, 366, 731):
            with self.subTest(days=days):
                report = self.report(
                    [12100], funding=funding, dates=[funding + timedelta(days=days)]
                )
                self.assertAlmostEqual(report["cagr"].value, 1.21 ** (365.25 / days) - 1)
        bankrupt = self.report([0], funding=funding, dates=[funding + timedelta(days=365)])
        self.assertEqual(bankrupt["cagr"].value, -1)

    def test_monthly_yearly_compound_continuous_account(self):
        dates = [
            date(2024, 12, 31),
            date(2025, 1, 2),
            date(2025, 1, 31),
            date(2025, 2, 3),
            date(2026, 1, 2),
        ]
        report = self.report(
            [12000, 18000, 9000, 9900, 10890], funding=date(2024, 12, 30), dates=dates
        )
        monthly = {row["period"]: row["total_return"].value for row in report["monthly_returns"]}
        yearly = {row["period"]: row["total_return"].value for row in report["yearly_returns"]}
        self.assertEqual(
            monthly,
            {
                "2024-12": Fraction(1, 5),
                "2025-01": Fraction(-1, 4),
                "2025-02": Fraction(1, 10),
                "2026-01": Fraction(1, 10),
            },
        )
        self.assertEqual(
            yearly, {"2024": Fraction(1, 5), "2025": Fraction(-7, 40), "2026": Fraction(1, 10)}
        )
        growth = math.prod(1 + value for value in yearly.values())
        self.assertEqual(growth - 1, report["total_return"].value)
        self.assertEqual(report["sessions"], 5)  # No fabricated missing dates.

    def test_rolling_43_crosses_year_without_reset(self):
        report = self.report([10000 + 100 * i for i in range(1, 45)], funding=date(2024, 12, 1))
        rolling = report["rolling_43"]
        self.assertEqual(rolling["count"], 2)
        self.assertEqual(rolling["windows"][0]["total_return"].value, Fraction(43, 100))
        self.assertEqual(rolling["windows"][1]["total_return"].value, Fraction(43, 101))
        self.assertEqual(rolling["median"].value, (Fraction(43, 100) + Fraction(43, 101)) / 2)
        self.assertEqual(rolling["worst"].value, Fraction(43, 101))
        self.assertEqual(rolling["percentage_positive"].value, 100)
        self.assertEqual(rolling["percentage_at_least_2pct"].value, 100)

    def test_rolling_threshold_inclusive_and_positive_strict(self):
        rolling = self.report([10000] * 43 + [10200])["rolling_43"]
        self.assertEqual(rolling["median"].value, Fraction(1, 100))
        self.assertEqual(rolling["worst"].value, 0)
        self.assertEqual(rolling["percentage_positive"].value, 50)
        self.assertEqual(rolling["percentage_at_least_2pct"].value, 50)
        one = self.report([10000] * 43)["rolling_43"]
        self.assertEqual(one["count"], 1)
        self.assertEqual(one["median"].value, 0)

    def test_closed_trade_statistics_exclude_dividends_and_open_pnl(self):
        report = calculate_metrics(
            [CloseEquity(date(2025, 1, 2), 10900, 1000000, 1000)],
            funded_equity_cents=10000,
            funded_date=date(2025, 1, 1),
            totals=AccountTotals(9900, unrealized_pnl_cents=-400, dividends_cents=1000),
            closed_trade_pnls_cents=[500, -200, 0],
        )
        self.assertEqual(report["completed_round_trips"], 3)
        self.assertEqual((report["wins"], report["losses"], report["breakeven_trades"]), (1, 1, 1))
        self.assertEqual(report["profit_factor"].value, Fraction(5, 2))
        self.assertEqual(report["realized_trading_pnl_cents"], 300)
        self.assertEqual(report["net_pnl_cents"], 900)
        losses_only = self.report([9000])
        self.assertEqual(losses_only["profit_factor"].value, 0)

    def test_zero_equity_denominators_propagate_not_silently_dropped(self):
        report = self.report([0] * 44)
        self.assertEqual(report["total_return"].value, -1)
        self.assertEqual(report["daily"][0]["daily_return"].value, -1)
        self.assertEqual(
            report["daily"][1]["daily_return"].undefined_reason, "zero_previous_equity"
        )
        self.assertEqual(
            report["mean_close_capital_fraction"].undefined_reason, "zero_close_equity"
        )
        self.assertEqual(report["annualized_volatility"].undefined_reason, "undefined_daily_return")
        self.assertIsNone(report["worst_daily_return"].value)
        self.assertEqual(report["monthly_returns"][0]["total_return"].value, -1)
        self.assertEqual(
            report["monthly_returns"][1]["total_return"].undefined_reason, "zero_previous_equity"
        )
        self.assertEqual(report["rolling_43"]["windows"][0]["total_return"].value, -1)
        self.assertEqual(
            report["rolling_43"]["median"].undefined_reason, "undefined_rolling_window"
        )
        self.assertEqual(report["drawdown"]["maximum"].value, 1)
        self.assertEqual(report["drawdown"]["maximum_episode"]["duration_sessions"], 44)

    def test_inputs_reject_inconsistency_instead_of_repair(self):
        row = CloseEquity(date(2025, 1, 2), 10000, 0, 0)
        kwargs = dict(
            funded_equity_cents=10000,
            funded_date=date(2025, 1, 1),
            totals=AccountTotals(10000),
            closed_trade_pnls_cents=[],
        )
        invalid_rows = [
            [],
            [row, row],
            [replace(row, date=date(2025, 1, 1))],
            [replace(row, date="2025-01-02")],
            [replace(row, equity_cents=10000.0)],
            [replace(row, invested_units_raw=True)],
            [replace(row, equity_cents=-1)],
            [replace(row, invested_value_cents=1)],
            [replace(row, invested_units_raw=1, invested_value_cents=10001)],
            [replace(row, invested_units_raw=1, legacy_any_session_invested=False)],
        ]
        for rows in invalid_rows:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                calculate_metrics(rows, **kwargs)
        changes = [
            dict(funded_equity_cents=0),
            dict(closed_trade_pnls_cents=[1]),
            dict(totals=AccountTotals(9999)),
            dict(totals=AccountTotals(10000, fees_cents=-1)),
            dict(terminal_cash_cents=10001),
            dict(closed_trade_pnls_cents=[0.0]),
            dict(totals=AccountTotals(10000, unrealized_pnl_cents=1)),
        ]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                calculate_metrics([row], **(kwargs | change))

    def test_rendering_is_lossless_deterministic_escaped_and_same_payload(self):
        report = self.report([11000, 9900])
        text = render_json(report)
        self.assertEqual(text, render_json(report))
        payload = json.loads(text)
        self.assertEqual(payload["funded_date"], "2025-01-01")
        self.assertEqual(payload["total_return"]["value"], dict(numerator=-1, denominator=100))
        self.assertNotIn("NaN", text)
        self.assertNotIn("Infinity", text)
        html = render_html(report, title='<script>alert("x")</script>')
        self.assertNotIn("<script>", html)
        self.assertIn("&lt;script&gt;", html)
        embedded = html.split("<pre>", 1)[1].split("</pre>", 1)[0]
        self.assertEqual(json.loads(unescape(embedded)), payload)

    @unittest.skipUnless(
        os.environ.get("BACKTEST_REPORT_TEST_BINARY"), "native report test binary not supplied"
    )
    def test_native_metrics_and_all_three_renderings_against_python(self):
        """Native fixture export is test-only; the report runtime never spawns Python."""
        binary = os.environ["BACKTEST_REPORT_TEST_BINARY"]
        actual = json.loads(subprocess.check_output([binary, "--oracle-json"], text=True))
        expected = {
            "flat": self.report([10000, 10000, 10000]),
            "zero": self.report([0] * 44),
            "continuous": self.report(
                [10000 + 100 * i for i in range(1, 45)], funding=date(2024, 12, 1)
            ),
            "calendar": self.report(
                [12000, 18000, 9000, 9900, 10890],
                funding=date(2024, 12, 30),
                dates=[
                    date(2024, 12, 31),
                    date(2025, 1, 2),
                    date(2025, 1, 31),
                    date(2025, 2, 3),
                    date(2026, 1, 2),
                ],
            ),
            "hand": calculate_metrics(
                [
                    CloseEquity(date(2025, 1, 2), 1000000, 95000000, 950000, True),
                    CloseEquity(date(2025, 1, 3), 1104500, 0, 0, True),
                ],
                funded_equity_cents=1000000,
                funded_date=date(2025, 1, 1),
                totals=AccountTotals(
                    1104500,
                    dividends_cents=9500,
                    fees_cents=100,
                    slippage_estimate_cents=25,
                    cumulative_gross_cents=1995000,
                ),
                closed_trade_pnls_cents=[95000],
            ),
            "terminal": calculate_metrics(
                [
                    CloseEquity(date(2025, 1, 2), 11000, 1000000, 10000, True),
                    CloseEquity(date(2025, 1, 3), 10500, 1000000, 9500, True),
                ],
                funded_equity_cents=10000,
                funded_date=date(2025, 1, 1),
                totals=AccountTotals(9900, fees_cents=600),
                closed_trade_pnls_cents=[-100],
                terminal_cash_cents=9900,
            ),
        }

        def compare(native, reference, path=""):
            if isinstance(reference, dict):
                # Native averages/median may be approximate; all endpoint ratios
                # and monetary coefficients must remain exactly equal.
                if set(reference) == {"numerator", "denominator"} and not isinstance(native, dict):
                    self.assertTrue(
                        path.endswith("mean_close_capital_fraction/value")
                        or path.endswith("rolling_43/median/value"),
                        path,
                    )
                    self.assertTrue(
                        math.isclose(
                            native,
                            reference["numerator"] / reference["denominator"],
                            rel_tol=1e-12,
                            abs_tol=1e-14,
                        ),
                        path,
                    )
                    return
                for key, value in reference.items():
                    if key != "schema":
                        compare(native[key], value, path + "/" + key)
            elif isinstance(reference, list):
                self.assertEqual(len(native), len(reference), path)
                for index, (a, b) in enumerate(zip(native, reference)):
                    compare(a, b, path + "/" + str(index))
            elif isinstance(reference, float):
                self.assertTrue(math.isclose(native, reference, rel_tol=1e-12, abs_tol=1e-14), path)
            else:
                self.assertEqual(native, reference, path)

        self.assertEqual(actual.keys(), expected.keys())
        for name, report in expected.items():
            with self.subTest(case=name):
                compare(actual[name], json.loads(render_json(report)), name)
        # Asset gaps are independent of account returns, with explicit coverage.
        self.assertEqual(
            actual["hand"]["worst_overnight_asset_gap"]["value"],
            {"numerator": -1, "denominator": 20},
        )
        self.assertEqual(actual["hand"]["observed_overnight_gap_sessions"], 2)
        self.assertEqual(actual["hand"]["rejected_orders"], 3)
        self.assertEqual(actual["hand"]["expired_orders"], 1)

        # Verify CSV contains every JSON leaf exactly, including nulls, empty
        # containers and the reason with quotes/comma/newline/HTML metacharacters.
        def leaves(value, path=""):
            if isinstance(value, dict) and value:
                for key, child in value.items():
                    yield from leaves(child, path + "/" + key.replace("~", "~0").replace("/", "~1"))
            elif isinstance(value, list) and value:
                for index, child in enumerate(value):
                    yield from leaves(child, path + "/" + str(index))
            else:
                yield path, value

        csv_text = subprocess.check_output([binary, "--oracle-csv"], text=True)
        csv_rows = list(csv.DictReader(io.StringIO(csv_text)))
        csv_values = {
            row["path"]: row["value"] if row["type"] == "string" else json.loads(row["value"])
            for row in csv_rows
        }
        self.assertEqual(len(csv_rows), len(csv_values))
        self.assertEqual(csv_values, dict(leaves(actual["hand"])))
        html = subprocess.check_output([binary, "--oracle-html"], text=True)
        self.assertNotIn("<reason>", html)
        embedded = html.split("<pre>", 1)[1].split("</pre>", 1)[0]
        self.assertEqual(json.loads(unescape(embedded)), actual["hand"])


class HistoricalSectionTests(unittest.TestCase):
    def setUp(self):
        try:
            from . import verify_reports as oracle
        except ImportError:
            import verify_reports as oracle
        self.v = oracle

    def fixture(self, terminal_at_cutoff=False):
        v = self.v
        dates = [
            date(2025, 12, 24),
            date(2025, 12, 26),
            date(2025, 12, 29),
            date(2025, 12, 30),
            date(2025, 12, 31),
            date(2026, 1, 2),
        ]
        openings, closes = [10, 10, 10, 14, 20, 25], [10, 10, 12, 14, 22, 25]
        dividends = [0, 0, 0, 100000000, 0, 500000000]
        rows = [
            v.PriceRow(day, opening * 10**9, close * 10**9, dividend)
            for day, opening, close, dividend in zip(dates, openings, closes, dividends)
        ]
        if terminal_at_cutoff:
            rows = rows[:5]
        dataset = v.Dataset(rows, {}, "authored-history-fixture")
        trial = dict(
            initial=10000,
            lot=1,
            commission=5,
            slippage=0,
            terminal=int(terminal_at_cutoff),
            currency="USD",
            drawdown=0,
            daily_loss=0,
        )
        header = dict(funding=0, begin=1, end=len(rows), legacy=False)
        state = dict(cash=10000, units=0, basis=0, realized=0, dividends=0, fees=0)
        events, pending = [], None

        def emit(kind, key, *, phase=None, price=0, order=None, quantity=0, settled=0, fee=0):
            mark = (
                rows[key - 1].close
                if phase == 0
                else rows[key].opening
                if phase == 1
                else rows[key].close
            )
            event = dict(
                trial=1,
                key=key,
                id=order["id"] if order else 0,
                kind=kind,
                reason=order["reason"] if order else "ordinary",
                **state,
                equity=state["cash"] + v.gross(mark, state["units"]) if phase is not None else None,
                phase=phase,
                price=price,
                quantity=quantity,
                gross=settled,
                fee=fee,
                decision_key=order["decision_key"] if order else None,
                eligible_key=order["eligible_key"] if order else None,
                quantity_policy=order["quantity_policy"] if order else "none",
                budget=0,
            )
            events.append(event)
            return event

        def fill(key, order, terminal=False):
            buying = order is not None and order["quantity_policy"] == "fixed_quantity"
            quantity = order["quantity"] if buying else state["units"]
            price = rows[key].close if terminal else rows[key].opening
            settled = v.gross(price, quantity)
            fee = v.ceil(Fraction(price * quantity * 5, 10**17))
            if not terminal:
                emit(
                    "accepted",
                    key,
                    phase=1,
                    price=price,
                    order=order,
                    quantity=quantity,
                    settled=settled,
                    fee=fee,
                )
            if buying:
                state["basis"] = settled + fee
                state["cash"] -= state["basis"]
                state["units"] = quantity
            else:
                state["cash"] += settled - fee
                state["realized"] += settled - fee - state["basis"]
                state["basis"] = state["units"] = 0
            state["fees"] += fee
            event = emit(
                "terminal" if terminal else "filled",
                key,
                phase=2 if terminal else 1,
                price=price,
                order=order,
                quantity=quantity,
                settled=settled,
                fee=fee,
            )
            if terminal:
                event.update(id=5, reason="synthetic_terminal_liquidation")

        emit("funded", 0, phase=2, price=rows[0].close)
        emit("close", 0, phase=2, price=rows[0].close)
        actions = {
            1: ("fixed_quantity", 5000000),
            2: ("sell_all", 0),
            3: ("fixed_quantity", 4000000),
            4: ("sell_all", 0),
        }
        for key in range(1, len(rows)):
            payment = v.gross(rows[key].dividend, state["units"])
            state["cash"] += payment
            state["dividends"] += payment
            emit("dividend", key, phase=0, price=rows[key - 1].close)
            if pending:
                fill(key, pending)
                pending = None
            emit("close", key, phase=2, price=rows[key].close)
            if key in actions:
                policy, quantity = actions[key]
                pending = dict(
                    id=key,
                    reason="authored_intent",
                    decision_key=key,
                    eligible_key=key + 1,
                    quantity_policy=policy,
                    quantity=quantity,
                )
                emit("submitted", key, order=pending, quantity=quantity)
        if pending:
            emit(
                "expired",
                len(rows) - 1,
                order=dict(pending, reason="end_of_scope"),
                quantity=pending["quantity"],
            )
        if terminal_at_cutoff:
            fill(len(rows) - 1, None, terminal=True)
        emit("final", len(rows) - 1, phase=2, price=rows[-1].close)
        return dataset, trial, header, events

    def test_continuous_prefix_preserves_capital_positions_and_cumulative_totals(self):
        v = self.v
        dataset, trial, header, events = self.fixture()
        scope = v.historical_scope(dataset, header, "fresh_research_v1")
        full_inputs, prefix = v._reconstruct(events, trial, header, dataset, scope[0])
        full = v.expected_metrics(dataset, trial, header, full_inputs)
        historical, summary = v.historical_expected(dataset, trial, header, scope, prefix)
        self.assertEqual(historical["funded_equity_cents"], 10000)
        self.assertEqual(historical["funded_date"], date(2025, 12, 24))
        self.assertEqual(historical["final_date"], date(2025, 12, 31))
        self.assertEqual(historical["sessions"], 4)
        self.assertEqual(historical["final_equity_cents"], 12839)
        self.assertEqual(historical["final_cash_cents"], 4039)
        self.assertEqual(historical["final_units_raw"], 4000000)
        self.assertEqual(historical["realized_trading_pnl_cents"], 1993)
        self.assertEqual(historical["unrealized_pnl_cents"], 796)
        self.assertEqual(historical["dividends_cents"], 50)
        self.assertEqual(historical["fees_cents"], 11)
        self.assertEqual(historical["cumulative_gross_cents"], 20000)
        self.assertEqual(historical["completed_round_trips"], 1)
        self.assertEqual(historical["close_invested_fraction"].value, Fraction(1, 2))
        self.assertEqual(historical["legacy_any_session_exposure"].value, Fraction(3, 4))
        self.assertEqual(historical["total_return"].value, Fraction(2839, 10000))
        self.assertEqual(dict(v.historical_table(historical))["Cost basis (cents)"], "8004")
        self.assertEqual(
            (
                full["final_equity_cents"],
                full["realized_trading_pnl_cents"],
                full["dividends_cents"],
                full["fees_cents"],
            ),
            (14234, 3984, 250, 16),
        )
        self.assertEqual(full["completed_round_trips"], 2)
        self.assertEqual(full["yearly_returns"][1]["total_return"].value, Fraction(1395, 12839))
        self.assertIn("no capital reset", summary)

    def test_cutoff_precedes_same_date_synthetic_sale_and_expiry(self):
        v = self.v
        dataset, trial, header, events = self.fixture(terminal_at_cutoff=True)
        scope = v.historical_scope(dataset, header, "fresh_research_v1")
        full_inputs, prefix = v._reconstruct(events, trial, header, dataset, scope[0])
        full = v.expected_metrics(dataset, trial, header, full_inputs)
        historical, _ = v.historical_expected(dataset, trial, header, scope, prefix)
        self.assertEqual(historical["final_equity_cents"], 12839)
        self.assertEqual(full["final_equity_cents"], 12834)
        self.assertEqual(
            (historical["completed_round_trips"], full["completed_round_trips"]), (1, 2)
        )
        self.assertEqual((historical["expired_orders"], full["expired_orders"]), (0, 1))
        self.assertTrue(historical["open_position"])
        self.assertFalse(full["open_position"])
        self.assertIsNone(historical["terminal_cash_cents"])
        self.assertEqual(historical["terminal_policy"], "mark")

    def test_missing_declared_cutoff_rejects_instead_of_using_prior_close(self):
        dataset, _, header, _ = self.fixture()
        dataset.rows[4] = replace(dataset.rows[4], day=date(2025, 12, 30))
        with self.assertRaises(ValueError):
            self.v.historical_scope(dataset, header, "fresh_research_v1")

    def test_nonapplicable_legacy_and_partial_never_claim_cutoff_metrics(self):
        v = self.v
        dataset, trial, header, events = self.fixture()
        legacy = v.historical_scope(dataset, header, "legacy_spy_2026_v1")
        result, _ = v.historical_expected(dataset, trial, header, legacy, None)
        self.assertEqual(
            result,
            dict(
                status="not_applicable",
                reason="legacy_profile",
                cutoff_date="2025-12-31",
                metrics=None,
            ),
        )
        scope = v.historical_scope(dataset, header, "fresh_research_v1")
        _, prefix = v._reconstruct(events, trial, header, dataset, scope[0])
        result, _ = v.historical_expected(dataset, trial, header, scope, prefix, complete=False)
        self.assertEqual(result["status"], "unavailable")
        self.assertEqual(result["reason"], "partial_financial_run")
        self.assertIsNone(result["metrics"])
        self.assertEqual(v.historical_table(result), [])

    def test_historical_metadata_and_visible_table_fail_on_full_window_leak(self):
        v = self.v
        dataset, trial, header, events = self.fixture()
        scope = v.historical_scope(dataset, header, "fresh_research_v1")
        _, prefix = v._reconstruct(events, trial, header, dataset, scope[0])
        expected = v.historical_expected(dataset, trial, header, scope, prefix)
        encoded = json.loads(render_json(expected[0]))
        # Native mean-capital reporting is a floating statistic, unlike the
        # Fraction retained by the independent oracle.
        encoded["mean_close_capital_fraction"]["value"] = float(
            expected[0]["mean_close_capital_fraction"].value
        )
        metadata = dict(
            historical_section_profile=v.HISTORICAL_PROFILE,
            historical_section_json=json.dumps(encoded),
            historical_section_summary=expected[1],
        )
        from html import escape

        rows = "".join(
            f"<tr><th>{escape(k)}</th><td>{escape(value)}</td></tr>"
            for k, value in v.historical_table(expected[0])
        )
        html = (
            f'<section id="historical-section"><p id="historical-summary-text">{expected[1]}</p>'
            f'<table id="historical-summary">{rows}</table></section>'
        )
        self.assertGreater(v.compare_historical_metadata(metadata, expected, html).exact, 0)
        with self.assertRaises(ValueError):
            v.compare_historical_metadata(
                metadata, expected, html.replace("<td>12839</td>", "<td>14234</td>")
            )
        bad = json.loads(metadata["historical_section_json"])
        bad["dividends_cents"] = 250
        metadata["historical_section_json"] = json.dumps(bad)
        with self.assertRaises(ValueError):
            v.compare_historical_metadata(metadata, expected)


class RiskEvidenceProjectionTests(unittest.TestCase):
    """Authored close/exit snapshots: reporting diagnostics, not strategy fixtures."""

    def setUp(self):
        try:
            from . import verify_reports as oracle
        except ImportError:
            import verify_reports as oracle
        self.v = oracle
        self.dataset = oracle.Dataset(
            [
                oracle.PriceRow(date(2025, 1, 1), 100 * 10**9, 100 * 10**9, 0),
                oracle.PriceRow(date(2025, 1, 2), 100 * 10**9, 100 * 10**9, 0),
                oracle.PriceRow(date(2025, 1, 3), 100 * 10**9, 75 * 10**9, 0),
                oracle.PriceRow(date(2025, 1, 6), 70 * 10**9, 70 * 10**9, 10**9),
            ],
            {},
            "synthetic",
        )
        self.trial = dict(
            initial=100000, drawdown=2000, daily_loss=0, terminal=0, commission=100, slippage=100
        )
        self.header = dict(funding=0, begin=1, end=4)

    def event(self, kind, key, **changes):
        units, cash = changes.get("units", 9000000), changes.get("cash", 10000)
        price = self.dataset.rows[key].close
        marked = kind in ("close", "risk", "final", "terminal")
        result = dict(
            trial=1,
            kind=kind,
            key=key,
            id=0,
            reason="ordinary",
            cash=cash,
            units=units,
            basis=90000,
            realized=0,
            dividends=0,
            fees=0,
            equity=cash + self.v.gross(price, units) if marked else None,
            phase=2 if marked else None,
            price=price if marked else 0,
            quantity=0,
            gross=0,
            fee=0,
            decision_key=None,
            eligible_key=None,
            quantity_policy="none",
            budget=0,
        )
        result.update(changes)
        return result

    def prefix(self):
        return [self.event("close", 1), self.event("close", 2), self.event("risk", 2, reason="1")]

    def order(self, kind, key, **changes):
        result = self.event(
            kind,
            key,
            id=7,
            reason="risk_exit",
            decision_key=2,
            eligible_key=3,
            quantity_policy="sell_all",
        )
        result.update(changes)
        return result

    def filled_trace(self):
        events = self.prefix() + [self.order("submitted", 2), self.order("accepted", 3)]
        price = self.dataset.rows[3].opening * 99 // 100
        settled = self.v.gross(price, 9000000)
        fee = self.v.ceil(Fraction(price * 9000000 * 100, 10**17))
        # The ex-date dividend adds 900 cents of cash; it is NOT gap P&L.
        cash = 10900 + settled - fee
        events += [
            self.order(
                "filled",
                3,
                units=0,
                cash=cash,
                basis=0,
                quantity=9000000,
                phase=1,
                price=price,
                gross=settled,
                fee=fee,
                equity=cash,
            ),
            self.event("close", 3, units=0, cash=cash, basis=0),
            self.event("final", 3, units=0, cash=cash, basis=0),
        ]
        return events

    def project(self, events, complete=True):
        return self.v.project_risk(events, self.trial, self.header, self.dataset, complete=complete)

    def test_signed_gap_and_execution_drag_exclude_dividends(self):
        payload, text = self.project(self.filled_trace())
        self.assertEqual(len(payload), 1)
        risk = payload[0]
        self.assertEqual(risk["trigger"]["baseline_equity_cents"], 100000)
        self.assertEqual(risk["trigger"]["equity_cents"], 77500)
        self.assertEqual(risk["trigger"]["held_close_value_cents"], 67500)
        fill = risk["orders"][0]["fill"]
        self.assertEqual(fill["raw_open_gap_pnl_cents"], -4500)
        self.assertEqual(fill["raw_open_gap_loss_cents"], 4500)
        self.assertEqual(fill["slippage_drag_cents"], 630)
        self.assertEqual(fill["fee_cents"], 624)
        self.assertEqual(fill["execution_drag_cents"], 1254)
        self.assertEqual(fill["net_exit_vs_trigger_close_cents"], -5754)
        self.assertEqual(risk["endpoint"]["observed_units_raw"], 0)
        self.assertTrue(risk["endpoint"]["risk_exit_executed"])
        self.assertIn("already included", text)

    def test_favorable_open_gap_has_zero_loss(self):
        self.dataset.rows[3] = self.v.PriceRow(date(2025, 1, 6), 80 * 10**9, 80 * 10**9, 10**9)
        fill = self.project(self.filled_trace())[0][0]["orders"][0]["fill"]
        self.assertEqual(fill["raw_open_gap_pnl_cents"], 4500)
        self.assertEqual(fill["raw_open_gap_loss_cents"], 0)

    def test_final_mark_expiry_remains_held_without_fabricated_fill(self):
        self.header["end"] = 3
        events = self.prefix() + [
            self.order("submitted", 2),
            self.order("expired", 2, reason="end_of_scope"),
            self.event("final", 2),
        ]
        risk = self.project(events)[0][0]
        self.assertEqual(risk["outcome"], "risk_exit_expired_still_held")
        self.assertIsNone(risk["orders"][0]["eligible_date"])
        self.assertIsNone(risk["orders"][0]["fill"])
        self.assertTrue(risk["endpoint"]["still_held"])
        self.assertFalse(risk["endpoint"]["risk_exit_executed"])

    def test_synthetic_endpoint_is_not_executable_risk_liquidation(self):
        self.header["end"] = 3
        self.trial["terminal"] = 1
        price = 74250000000
        terminal = self.event(
            "terminal",
            2,
            id=8,
            units=0,
            cash=76156,
            basis=0,
            reason="synthetic_terminal_liquidation",
            quantity=9000000,
            price=price,
            gross=66825,
            fee=669,
        )
        events = self.prefix() + [
            self.order("submitted", 2),
            self.order("expired", 2, reason="end_of_scope"),
            terminal,
            self.event("final", 2, units=0, cash=76156, basis=0),
        ]
        risk = self.project(events)[0][0]
        self.assertEqual(risk["outcome"], "risk_exit_expired_synthetic_endpoint")
        self.assertIsNone(risk["orders"][0]["fill"])
        self.assertEqual(risk["synthetic_terminal"]["fee_cents"], 669)
        self.assertFalse(risk["endpoint"]["risk_exit_executed"])
        self.assertFalse(risk["endpoint"]["still_held"])
        broken = copy.deepcopy(events)
        broken[-2].update(
            kind="filled",
            id=7,
            reason="risk_exit",
            decision_key=2,
            eligible_key=3,
            quantity_policy="sell_all",
        )
        with self.assertRaises(ValueError):
            self.project(broken)

    def test_flat_trigger_and_daily_loss_precedence(self):
        self.header["end"] = 3
        self.trial["daily_loss"] = 500
        events = [
            self.event("close", 1, units=0, cash=100000),
            self.event("close", 2, units=0, cash=80000),
            self.event("risk", 2, units=0, cash=80000, reason="1"),
            self.event("final", 2, units=0, cash=80000),
        ]
        risk = self.project(events)[0][0]
        self.assertEqual(risk["trigger"]["reason"], "Drawdown")
        self.assertEqual(risk["outcome"], "flat_trigger_no_order")
        self.assertEqual(risk["orders"], [])
        self.trial["drawdown"] = 0
        events[1] = self.event("close", 2, units=0, cash=95000)
        events[2] = self.event("risk", 2, units=0, cash=95000, reason="2")
        events[3] = self.event("final", 2, units=0, cash=95000)
        trigger = self.project(events)[0][0]["trigger"]
        self.assertEqual((trigger["reason"], trigger["threshold_bps"]), ("DailyLoss", 500))

    def test_partial_pending_and_unrecorded_order_state(self):
        for accepted in (False, True):
            events = self.prefix() + [self.order("submitted", 2)]
            if accepted:
                events.append(self.order("accepted", 3))
            risk, text = self.project(events, complete=False)
            self.assertEqual(risk[0]["endpoint"]["financial_state"], "partial")
            self.assertEqual(risk[0]["endpoint"]["pending_order_id"], 7)
            self.assertTrue(risk[0]["endpoint"]["still_held"])
            self.assertIsNone(risk[0]["orders"][0]["fill"])
            self.assertEqual(risk[0]["orders"][0]["status"], "accepted" if accepted else "queued")
            self.assertTrue(text.startswith("Partial trace:"))
            with self.assertRaises(ValueError):
                self.project(events)
        risk = self.project(self.prefix(), complete=False)[0][0]
        self.assertEqual(risk["outcome"], "trigger_recorded_order_not_recorded")
        self.assertIsNone(risk["endpoint"]["pending_order_id"])
        filled_prefix = self.filled_trace()[:-2]
        risk = self.project(filled_prefix, complete=False)[0][0]
        self.assertTrue(risk["endpoint"]["risk_exit_executed"])
        self.assertEqual(risk["endpoint"]["financial_state"], "partial")

    def test_duplicate_missing_or_changed_risk_evidence_rejects(self):
        original = self.filled_trace()
        duplicate = original[:3] + [copy.deepcopy(original[2])] + original[3:]
        missing = [event for event in original if event["kind"] != "risk"]
        wrong = copy.deepcopy(original)
        wrong[2]["reason"] = "2"
        for events in (duplicate, missing, wrong):
            with self.subTest(events=events), self.assertRaises(ValueError):
                self.project(events)
        expected = self.project(original)
        metadata = dict(
            risk_report_profile="trigger_order_gap_v1",
            risk_gap_interpretation=self.v.RISK_GAP_INTERPRETATION,
            risk_events_json=json.dumps(expected[0]),
            risk_outcome=expected[1],
        )
        self.assertGreater(self.v.compare_risk_metadata(metadata, expected), 0)
        changed = copy.deepcopy(expected[0])
        changed[0]["orders"][0]["fill"]["raw_open_gap_pnl_cents"] += 1
        metadata["risk_events_json"] = json.dumps(changed)
        with self.assertRaises(ValueError):
            self.v.compare_risk_metadata(metadata, expected)

    @unittest.skipUnless(
        os.environ.get("BACKTEST_RISK_REPORT_RUN")
        and os.environ.get("BACKTEST_RISK_REPORT_DATASET"),
        "rebuilt 160-row, 5+5bps Keltner crash report fixture not supplied",
    )
    def test_rebuilt_native_keltner_crash_reports(self):
        dataset = self.v.load_dataset(Path(os.environ["BACKTEST_RISK_REPORT_DATASET"]))
        run = Path(os.environ["BACKTEST_RISK_REPORT_RUN"])
        result = self.v.verify_run(run, dataset)  # Full independent metrics and all projections.
        self.assertEqual((result["trials"], result["risk_triggers"]), (6, 4))
        bundle = self.v.Bundle(run)
        for trial in range(1, 7):
            metadata = bundle.json(f"report-{trial}.json")["metadata"]
            self.assertEqual(metadata["adjustment_policy"], self.v.ADJUSTMENT_POLICY)
            risks = self.v.parse_json(metadata["risk_events_json"])
            if trial > 4:
                self.assertEqual(risks, [])
                continue
            self.assertEqual(len(risks), 1)
            risk = risks[0]
            self.assertEqual((risk["trigger"]["key"], risk["trigger"]["reason"]), (45, "Drawdown"))
            fill = risk["orders"][0]["fill"]
            self.assertEqual(fill["key"], 46)
            # The fixture crashes at the triggering close. Its NEXT raw open is
            # unchanged, so the executed gap is zero, not the entire trade loss.
            self.assertEqual(fill["raw_open_gap_pnl_cents"], 0)
            self.assertEqual(fill["execution_drag_cents"], 21 if trial == 1 else 5)
            self.assertEqual(fill["net_exit_vs_trigger_close_cents"], -21 if trial == 1 else -5)
            self.assertFalse(risk["endpoint"]["still_held"])


if __name__ == "__main__":
    unittest.main()
