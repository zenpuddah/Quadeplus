#!/usr/bin/env python3
import csv
from fractions import Fraction
import io
import subprocess
import sys
from verify_reference import even

run = subprocess.run([sys.argv[1]], text=True, capture_output=True)
if run.returncode:
    raise RuntimeError(run.stderr + run.stdout)
rows = list(csv.DictReader(io.StringIO(run.stdout)))
assert len(rows) == 160
for name in ("constant", "changing"):
    closes, ranges = [], []
    ema = atr = None
    for row in (r for r in rows if r["case"] == name):
        key, h, low, c = (int(row[k]) for k in ("key", "high", "low", "close"))
        tr = h - low if not closes else max(h - low, abs(h - closes[-1]), abs(low - closes[-1]))
        closes.append(c)
        ranges.append(tr)
        if key == 19:
            ema = even(Fraction(sum(closes), 20))
        elif key > 19:
            ema = even(Fraction(2 * c + 19 * ema, 21))
        if key == 9:
            atr = even(Fraction(sum(ranges), 10))
        elif key > 9:
            atr = even(Fraction(9 * atr + tr, 10))
        upper = None if ema is None else even(Fraction(2 * ema + 3 * atr, 2))
        for field, expected in (("tr", tr), ("ema", ema), ("atr", atr), ("upper", upper)):
            assert row[field] == ("" if expected is None else str(expected)), (
                name,
                key,
                field,
                row[field],
                expected,
            )
print("FEATURE_ORACLE_PASS rows=160 warming_and_parent_checks=native half_tick=2")
