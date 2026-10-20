#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile

scratch = Path(sys.argv[2])
scratch.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix="stochastic-host.", dir=scratch)).resolve()
subprocess.run([sys.argv[1], str(root / "cases")], check=True)
print(f"STOCHASTIC_HOST_EVIDENCE={root}")
