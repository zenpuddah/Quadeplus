"""Check source formatting and Python lint; --fix applies formatting only."""

import argparse
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="apply formatting before lint checks")
    args = parser.parse_args()
    for requirement in (ROOT / "requirements-quality.txt").read_text().splitlines():
        package, expected = requirement.split("==")
        try:
            actual = version(package)
        except PackageNotFoundError:
            actual = "missing"
        if actual != expected:
            parser.error(
                f"{package}: expected {expected}, got {actual}; install requirements-quality.txt"
            )

    entries = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
    )
    paths = sorted({path.decode() for path in entries.split(b"\0") if path})
    cpp = [p for p in paths if p.endswith((".cpp", ".hpp", ".h", ".cc", ".cpp.in", ".hpp.in"))]
    python = [p for p in paths if p.endswith(".py")]
    cmake = [p for p in paths if Path(p).name == "CMakeLists.txt" or p.endswith(".cmake")]
    tools = Path(sys.executable).parent
    if cpp:
        flags = ["-i"] if args.fix else ["--dry-run", "--Werror"]
        run(str(tools / "clang-format"), "--style=file", *flags, *cpp)
    if python:
        flags = [] if args.fix else ["--check"]
        run(str(tools / "ruff"), "format", *flags, *python)
        run(str(tools / "ruff"), "check", *python)
    if cmake:
        run(
            str(tools / "cmake-format"),
            "--config-files",
            str(ROOT / ".cmake-format.json"),
            "--in-place" if args.fix else "--check",
            *cmake,
        )
    run("git", "diff", "--check")
    run("git", "diff", "--cached", "--check")
    print(
        f"Style checks passed: {len(cpp)} C++ files, {len(python)} Python files, {len(cmake)} CMake files"
    )


if __name__ == "__main__":
    main()
