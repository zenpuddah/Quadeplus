# Code style and checks

Use Python 3.12+ to install the pinned development tools into an isolated environment:

```sh
python3 -m venv .venv-quality
.venv-quality/bin/python -m pip install -r requirements-quality.txt
.venv-quality/bin/python tools/check_style.py
```

To apply formatting, then check Python lint:

```sh
.venv-quality/bin/python tools/check_style.py --fix
.venv-quality/bin/python tools/check_style.py
```

- C++: clang-format 21.1.8, LLVM-based style, four-space indentation, 100 columns,
  expanded short statements, and preserved include/using declaration order.
- Python: Ruff 0.14.0 formatting and E4/E7/E9/F lint checks. Lint fixes are reviewed
  separately; `--fix` only applies formatting.
- CMake: cmake-format from cmakelang 0.6.13, four spaces and 100 columns, without
  argument sorting or comment rewriting. Its six dependency is pinned to 1.17.0.

The checker verifies tool versions and processes Git-listed tracked and nonignored
new source files, including C++ templates. Ignored build/dependency outputs are not
formatted. Keep external dependencies outside tracked source directories.

GitHub Actions runs the same check on pushes and pull requests. CLion can use the
root `.clang-format`; use the pinned command-line tool for the authoritative check.
Keep shared canonical headers consistent with Lexongine. Formatting must preserve
program behavior, literals and serialization contracts. Rebuild and run the relevant
[test profiles](build.md) after source formatting; a passing style job alone does
not establish runtime correctness.
