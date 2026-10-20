"""Pure preparatory reporting math for implementation-plan.md section 7.

This module consumes a declared, already validated financial session scope. It
does not establish source completeness, run acceptance, matched controls or risk
actions. There are no external cash flows after funding. Dates are datetime.date,
money is integer cents, and units are integer Quantity6 coefficients (10**6/unit).
Rows are ordinary closes only, strictly after funding; never pass the funding row
or synthetic terminal event as another session. AccountTotals and completed trade
P&Ls describe the endpoint *after* any terminal sale. Trade P&Ls include fees and
exclude dividends. Exposure always uses the ordinary closes before that sale.
Period/rolling returns use continuous-account endpoint ratios, equivalent to
compounding when every intervening daily ratio is defined. An interior zero does
not invalidate a period with a positive starting baseline; a zero baseline does.

calculate_metrics returns a dictionary containing Metric objects. Exact ratios
use Fraction; volatility, Sharpe and CAGR are approximate floats. Undefined values
carry a reason, never NaN/infinity. Ratios are fractions of one unless explicitly
named percentage. render_json preserves Fractions as numerator/denominator pairs;
render_html displays the same JSON without writing files or acceptance markers.
This independent helper verifies reporting math separately from native integration.
"""

from dataclasses import dataclass, fields, is_dataclass
from datetime import date
from fractions import Fraction
from html import escape
import json
import math
from typing import Sequence


@dataclass(frozen=True)
class CloseEquity:
    date: date
    equity_cents: int
    invested_units_raw: int
    invested_value_cents: int
    # True iff units were carried into this session or acquired at its open.
    # A sale at the open can therefore be True even with zero close units.
    legacy_any_session_invested: bool | None = None


@dataclass(frozen=True)
class AccountTotals:
    final_cash_cents: int
    unrealized_pnl_cents: int = 0
    dividends_cents: int = 0
    fees_cents: int = 0
    # Informational, already embedded in equity; never deduct again.
    slippage_estimate_cents: int | None = None
    cumulative_gross_cents: int | None = None


@dataclass(frozen=True)
class Metric:
    value: Fraction | float | None
    undefined_reason: str | None = None


def _undefined(reason):
    return Metric(None, reason)


def _integer(value, name, minimum=None):
    if type(value) is not int:
        raise ValueError(f"{name} must be integer coefficients, not bool/float")
    if minimum is not None and value < minimum:
        raise ValueError(f"{name} must be >= {minimum}")


def _validate(rows, funded_equity_cents, funded_date, totals, trades, terminal):
    _integer(funded_equity_cents, "funded_equity_cents", 1)
    if type(funded_date) is not date:
        raise ValueError("funded_date must be datetime.date")
    if not rows:
        raise ValueError("at least one ordinary close is required")
    previous = funded_date
    for row in rows:
        if type(row.date) is not date or row.date <= previous:
            raise ValueError("close dates must strictly increase after funding")
        previous = row.date
        for name in ("equity_cents", "invested_units_raw", "invested_value_cents"):
            _integer(getattr(row, name), name, 0)
        if row.invested_value_cents > row.equity_cents:
            raise ValueError("invested value exceeds equity (negative cash)")
        if row.invested_units_raw == 0 and row.invested_value_cents != 0:
            raise ValueError("zero units must have zero invested value")
        legacy = row.legacy_any_session_invested
        if legacy is not None and type(legacy) is not bool:
            raise ValueError("legacy_any_session_invested must be bool or None")
        if legacy is False and row.invested_units_raw > 0:
            raise ValueError("close holdings require any-session exposure")
    for field in fields(totals):
        value = getattr(totals, field.name)
        if value is None and field.name in ("slippage_estimate_cents", "cumulative_gross_cents"):
            continue
        _integer(value, field.name, None if field.name == "unrealized_pnl_cents" else 0)
    for pnl in trades:
        _integer(pnl, "closed trade tradingPnl")
    if terminal is not None:
        _integer(terminal, "terminal_cash_cents", 0)
        if terminal > rows[-1].equity_cents:
            raise ValueError("costed terminal cash cannot exceed ordinary close equity")
        if totals.unrealized_pnl_cents != 0:
            raise ValueError("liquidated endpoint cannot retain unrealized P&L")
        expected_cash = terminal
    else:
        expected_cash = rows[-1].equity_cents - rows[-1].invested_value_cents
    if totals.final_cash_cents != expected_cash:
        raise ValueError("final cash does not reconcile to endpoint valuation")
    if (terminal is not None or rows[-1].invested_units_raw == 0) and totals.unrealized_pnl_cents:
        raise ValueError("flat endpoint cannot retain unrealized P&L")
    final = rows[-1].equity_cents if terminal is None else terminal
    if (
        final - funded_equity_cents
        != sum(trades) + totals.unrealized_pnl_cents + totals.dividends_cents
    ):
        raise ValueError("net P&L must equal realized trading + unrealized + dividends")


def _endpoint_return(ending, baseline):
    return (
        Metric(Fraction(ending - baseline, baseline))
        if baseline
        else _undefined("zero_previous_equity")
    )


def _period_returns(rows, equities, funded_equity_cents, yearly=False):
    groups = {}
    for index, row in enumerate(rows):
        key = row.date.isoformat()[: 4 if yearly else 7]
        groups.setdefault(key, []).append(index)
    return [
        dict(
            period=key,
            sessions=len(indices),
            total_return=_endpoint_return(
                equities[indices[-1]],
                equities[indices[0] - 1] if indices[0] else funded_equity_cents,
            ),
        )
        for key, indices in groups.items()
    ]


def _rolling_returns(rows, equities, funded_equity_cents):
    windows = [
        dict(
            start_date=rows[end - 42].date,
            end_date=rows[end].date,
            total_return=_endpoint_return(
                equities[end], equities[end - 43] if end > 42 else funded_equity_cents
            ),
        )
        for end in range(42, len(rows))
    ]
    if not windows:
        missing = _undefined("fewer_than_43_sessions")
    elif any(window["total_return"].value is None for window in windows):
        # Do not silently select only defined windows for the headline statistics.
        missing = _undefined("undefined_rolling_window")
    else:
        values = sorted(window["total_return"].value for window in windows)
        count = len(values)
        middle = count // 2
        median = values[middle] if count % 2 else (values[middle - 1] + values[middle]) / 2
        return dict(
            window_sessions=43,
            count=count,
            windows=windows,
            median=Metric(median),
            worst=Metric(values[0]),
            percentage_positive=Metric(Fraction(100 * sum(v > 0 for v in values), count)),
            percentage_at_least_2pct=Metric(
                Fraction(100 * sum(v >= Fraction(1, 50) for v in values), count)
            ),
        )
    return dict(
        window_sessions=43,
        count=len(windows),
        windows=windows,
        median=missing,
        worst=missing,
        percentage_positive=missing,
        percentage_at_least_2pct=missing,
    )


def _daily_statistics(returns):
    if any(result.value is None for result in returns):
        missing = _undefined("undefined_daily_return")
    elif len(returns) < 2:
        missing = _undefined("fewer_than_two_returns")
    else:
        values = [result.value for result in returns]
        mean = sum(values, Fraction(0)) / len(values)
        variance = sum(((value - mean) ** 2 for value in values), Fraction(0)) / (len(values) - 1)
        if variance == 0:
            missing = _undefined("zero_return_variance")
        else:
            try:
                std = math.sqrt(variance)
                volatility = std * math.sqrt(252)
                sharpe = float(mean) / std * math.sqrt(252)
                if math.isfinite(volatility) and math.isfinite(sharpe):
                    return Metric(volatility), Metric(sharpe)
            except (OverflowError, ZeroDivisionError):
                pass
            missing = _undefined("floating_point_range")
    return missing, missing


def _drawdowns(rows, equities, funded_equity_cents, funded_date):
    """Positive loss fractions; durations run peak -> recovery or last close.

    Duration sessions are index differences, funding index zero. Calendar days
    are actual date differences. Equal peaks reset the next episode's origin to
    the latest touch; equal deepest troughs/maxima retain the first occurrence.
    A recovered episode includes the recovery session in its elapsed duration.
    Maximum-depth episode and currently-underwater status are reported separately.
    """
    peak, peak_index, peak_date = funded_equity_cents, 0, funded_date
    episodes, series, active, maximum = [], [], None, None
    for index, (row, equity) in enumerate(zip(rows, equities), 1):
        drawdown = Fraction(max(0, peak - equity), peak)
        series.append(Metric(drawdown))
        if equity >= peak:
            if active is not None:
                active.update(
                    recovery_date=row.date,
                    still_underwater=False,
                    duration_sessions=index - active.pop("_peak_index"),
                    duration_calendar_days=(row.date - active["peak_date"]).days,
                )
                active = None
            peak, peak_index, peak_date = equity, index, row.date
        else:
            if active is None:
                active = dict(
                    peak_date=peak_date,
                    peak_equity_cents=peak,
                    trough_date=row.date,
                    trough_equity_cents=equity,
                    depth=Metric(drawdown),
                    recovery_date=None,
                    still_underwater=True,
                    _peak_index=peak_index,
                )
                episodes.append(active)
            if drawdown > active["depth"].value:
                active.update(
                    trough_date=row.date, trough_equity_cents=equity, depth=Metric(drawdown)
                )
            if maximum is None or drawdown > maximum["depth"].value:
                maximum = active
    if active is not None:
        active.update(
            duration_sessions=len(rows) - active.pop("_peak_index"),
            duration_calendar_days=(rows[-1].date - active["peak_date"]).days,
        )
    return dict(
        maximum=Metric(Fraction(0) if maximum is None else maximum["depth"].value),
        maximum_episode=maximum,
        episodes=episodes,
        still_underwater=active is not None,
    ), series


def calculate_metrics(
    rows: Sequence[CloseEquity],
    *,
    funded_equity_cents: int,
    funded_date: date,
    totals: AccountTotals,
    closed_trade_pnls_cents: Sequence[int],
    terminal_cash_cents: int | None = None,
):
    """Calculate only; reject inconsistent account inputs, never repair them.

    closed_trade_pnls_cents includes every completed full round trip, including a
    synthetic terminal sale when present. Totals include terminal fees/dividends
    already posted. Source/ledger verification and evidence provenance belong to
    the integrating caller. The supplied sequence defines sessions; no calendar
    inference, date sorting, annual reset or fresh-cohort simulation occurs here.
    """
    rows, trades = tuple(rows), tuple(closed_trade_pnls_cents)
    _validate(rows, funded_equity_cents, funded_date, totals, trades, terminal_cash_cents)
    equities = [row.equity_cents for row in rows]
    if terminal_cash_cents is not None:
        equities[-1] = terminal_cash_cents
    previous, returns = funded_equity_cents, []
    for equity in equities:
        returns.append(
            Metric(Fraction(equity - previous, previous))
            if previous
            else _undefined("zero_previous_equity")
        )
        previous = equity
    volatility, sharpe = _daily_statistics(returns)
    drawdown, drawdown_series = _drawdowns(rows, equities, funded_equity_cents, funded_date)
    elapsed_days = (rows[-1].date - funded_date).days
    if elapsed_days < 365:
        cagr = _undefined("fewer_than_365_calendar_days")
    else:
        try:
            value = (
                float(Fraction(equities[-1], funded_equity_cents)) ** (365.25 / elapsed_days) - 1
            )
            cagr = Metric(value) if math.isfinite(value) else _undefined("floating_point_range")
        except OverflowError:
            cagr = _undefined("floating_point_range")
    profits = sum(pnl for pnl in trades if pnl > 0)
    losses = -sum(pnl for pnl in trades if pnl < 0)
    capital_fractions = [
        Metric(Fraction(row.invested_value_cents, row.equity_cents))
        if row.equity_cents
        else _undefined("zero_close_equity")
        for row in rows
    ]
    capital_mean = (
        _undefined("zero_close_equity")
        if any(value.value is None for value in capital_fractions)
        else Metric(sum((value.value for value in capital_fractions), Fraction(0)) / len(rows))
    )
    legacy_exposure = (
        _undefined("missing_legacy_session_exposure")
        if any(row.legacy_any_session_invested is None for row in rows)
        else Metric(Fraction(sum(row.legacy_any_session_invested for row in rows), len(rows)))
    )
    worst_return = (
        _undefined("undefined_daily_return")
        if any(value.value is None for value in returns)
        else Metric(min(value.value for value in returns))
    )
    return dict(
        schema="quadeplus-report-metrics-preparatory-v1",
        funded_date=funded_date,
        funded_equity_cents=funded_equity_cents,
        final_date=rows[-1].date,
        elapsed_calendar_days=elapsed_days,
        sessions=len(rows),
        terminal_policy="mark" if terminal_cash_cents is None else "synthetic_cash_replacement",
        terminal_cash_cents=terminal_cash_cents,
        ordinary_final_equity_cents=rows[-1].equity_cents,
        final_equity_cents=equities[-1],
        net_pnl_cents=equities[-1] - funded_equity_cents,
        total_return=Metric(Fraction(equities[-1] - funded_equity_cents, funded_equity_cents)),
        realized_trading_pnl_cents=sum(trades),
        unrealized_pnl_cents=totals.unrealized_pnl_cents,
        dividends_cents=totals.dividends_cents,
        final_cash_cents=totals.final_cash_cents,
        final_units_raw=rows[-1].invested_units_raw if terminal_cash_cents is None else 0,
        open_position=terminal_cash_cents is None and rows[-1].invested_units_raw > 0,
        fees_cents=totals.fees_cents,
        slippage_estimate_cents=totals.slippage_estimate_cents,
        cumulative_gross_cents=totals.cumulative_gross_cents,
        gross_turnover=(
            Metric(Fraction(totals.cumulative_gross_cents, funded_equity_cents))
            if totals.cumulative_gross_cents is not None
            else _undefined("gross_not_supplied")
        ),
        completed_round_trips=len(trades),
        wins=sum(pnl > 0 for pnl in trades),
        losses=sum(pnl < 0 for pnl in trades),
        breakeven_trades=sum(pnl == 0 for pnl in trades),
        profit_factor=Metric(Fraction(profits, losses))
        if losses
        else _undefined("no_losing_trade_pnl"),
        drawdown=drawdown,
        worst_daily_return=worst_return,
        annualized_volatility=volatility,
        sharpe_zero_risk_free=sharpe,
        cagr=cagr,
        annualization_sessions=252,
        cagr_year_days=365.25,
        close_invested_fraction=Metric(
            Fraction(sum(row.invested_units_raw > 0 for row in rows), len(rows))
        ),
        mean_close_capital_fraction=capital_mean,
        legacy_any_session_exposure=legacy_exposure,
        daily=[
            dict(
                date=row.date,
                ordinary_equity_cents=row.equity_cents,
                reporting_equity_cents=equity,
                daily_return=result,
                drawdown=dd,
                invested_units_raw=row.invested_units_raw,
                invested_value_cents=row.invested_value_cents,
                close_capital_fraction=capital,
                legacy_any_session_invested=row.legacy_any_session_invested,
            )
            for row, equity, result, dd, capital in zip(
                rows, equities, returns, drawdown_series, capital_fractions
            )
        ],
        monthly_returns=_period_returns(rows, equities, funded_equity_cents),
        yearly_returns=_period_returns(rows, equities, funded_equity_cents, yearly=True),
        rolling_43=_rolling_returns(rows, equities, funded_equity_cents),
    )


def _json_default(value):
    if isinstance(value, Fraction):
        return dict(numerator=value.numerator, denominator=value.denominator)
    if isinstance(value, date):
        return value.isoformat()
    if is_dataclass(value) and not isinstance(value, type):
        return {field.name: getattr(value, field.name) for field in fields(value)}
    raise TypeError(f"unsupported report value: {type(value).__name__}")


def render_json(report):
    """Return deterministic, lossless JSON text. Undefined Metric.value is null."""
    return (
        json.dumps(report, default=_json_default, sort_keys=True, indent=2, allow_nan=False) + "\n"
    )


def render_html(report, title="Backtest reporting math"):
    """Return escaped, self-contained HTML showing the exact same report payload."""
    return (
        '<!doctype html>\n<html lang="en"><head><meta charset="utf-8">'
        f"<title>{escape(title)}</title></head><body><h1>{escape(title)}</h1>"
        "<p>Preparatory metrics only; this report does not establish run acceptance. "
        "Ratios use exact numerator/denominator pairs. Undefined values include reasons.</p>"
        f"<pre>{escape(render_json(report))}</pre></body></html>\n"
    )
