#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile

scratch = Path(sys.argv[2])
scratch.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix="stochastic.", dir=scratch)).resolve()
subprocess.run([sys.argv[1], str(root / "fixture")], check=True)
subprocess.run(
    [
        sys.executable,
        "-B",
        str(Path(__file__).with_name("verify_stochastic.py")),
        "--synthetic-only",
    ],
    check=True,
)
print(f"STOCHASTIC_UNIT_EVIDENCE={root}")
