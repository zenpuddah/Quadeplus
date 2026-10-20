"""Run and independently verify three synthetic backtests, then link their reports."""

import argparse
import csv
from html import escape
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def execute(*args):
    result = subprocess.run([str(arg) for arg in args], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"command failed: {args}\n{result.stdout}\n{result.stderr}")
    print(result.stdout, end="")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument(
        "--out", type=Path, required=True, help="new directory under an existing parent"
    )
    args = parser.parse_args()
    if not __debug__:
        parser.error("independent verification requires assertions; do not use Python -O")
    binaries = args.build_dir.resolve() / "bin"
    runner = binaries / "quadeplus_backtest"
    for name in ("quadeplus_backtest", "backtest_sma_tests", "backtest_keltner_tests"):
        if not (binaries / name).is_file():
            parser.error(
                f"missing executable: {binaries / name}; build the backtesting profile first"
            )
    out = args.out.resolve()
    if out.exists() or not out.parent.is_dir():
        parser.error("--out must be new beneath an existing directory")
    out.mkdir()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ROOT / "integration/backtest"))
    import verify_sharing

    reports, runs = [], []
    for family, template, fixture, begin in (
        ("sma", "sma.xml", "backtest_sma_tests", 1),
        ("keltner", "keltner.xml", "backtest_keltner_tests", 60),
        ("stochastic", "stochastic-fresh.xml", "backtest_sma_tests", 1),
    ):
        inputs = out / f"{family}-input"
        execute(binaries / fixture, "--fixture", inputs, "trend", begin)
        dataset = inputs / "dataset"
        with (inputs / "prices.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        tree = ET.parse(ROOT / "examples/backtest" / template)
        tree.getroot().attrib.update(
            symbol="TEST",
            numerical_origin=rows[0]["Date"],
            evaluation_begin=rows[begin]["Date"],
            evaluation_end=rows[-1]["Date"],
        )
        config = out / f"{family}.xml"
        with config.open("xb") as stream:
            tree.write(stream, encoding="utf-8")
        run = out / family
        stdout = execute(runner, "run", "--config", config, "--dataset", dataset, "--out", run)
        if "RUN_ACCEPTED" not in stdout:
            raise RuntimeError(f"{family}: missing accepted run result")
        if family == "keltner":
            execute(
                sys.executable,
                "-B",
                ROOT / "integration/backtest/verify_keltner.py",
                "--dataset",
                dataset,
                "--config",
                config,
                "--output",
                run,
            )
        else:
            manifest, digest, bars, first = verify_sharing.fixture_inputs(dataset)
            verify_sharing.verify_run(dataset, config, run, family, manifest, digest, bars, first)
        execute(
            sys.executable,
            "-B",
            ROOT / "integration/backtest/verify_reports.py",
            "--dataset",
            dataset,
            "--run",
            run,
        )
        reports.extend(sorted(run.glob("report-*.html")))
        runs.append(
            {
                "family": family,
                "input": str(dataset.relative_to(out)),
                "output": family,
                "verified": True,
            }
        )
    links = "".join(
        f'<li><a href="{escape(path.relative_to(out).as_posix(), quote=True)}">'
        f"{escape(path.parent.name)} / {escape(path.stem)}</a></li>"
        for path in reports
    )
    with (out / "index.html").open("x") as stream:
        stream.write(
            '<!doctype html><html lang="en"><meta charset="utf-8">'
            "<title>Synthetic backtests</title><h1>Synthetic backtests</h1>"
            "<p>Authored test prices, not historical market results. Three independent "
            "runs with matched controls; this index is not a historical suite acceptance marker.</p>"
            f"<ul>{links}</ul></html>"
        )
    with (out / "demo.json").open("x") as stream:
        json.dump(
            {
                "format": "quadeplus-synthetic-demo-v1",
                "verified": True,
                "source": "synthetic",
                "runs": runs,
                "reports": len(reports),
            },
            stream,
            indent=2,
        )
        stream.write("\n")
    print(f"SYNTHETIC_DEMO_VERIFIED reports={len(reports)} index={out / 'index.html'}")


if __name__ == "__main__":
    main()
