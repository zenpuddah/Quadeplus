# Build and test

## Core

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core -j 4
ctest --test-dir build/core --output-on-failure
```

The core is header-based C++20 under `src/quant`. `Quadeplus::core` supplies its
include path and language requirement to CMake consumers. The default build runs
ten deterministic tests. It does not configure external SDKs or fetch data.

## Backtesting

For the optional native strategy application, use the separate
[backtesting build and synthetic-report guide](backtesting.md). Enable
`QUADEPLUS_BACKTEST` with explicit compatible Lexongine and TinyXML2 source paths.
That daily research profile does not require Arrow/Parquet or a provider SDK.

## Storage

Enable `QUADEPLUS_STORAGE` and supply native Arrow/Parquet CONFIG packages. The
verified package pair is 26.0.0.

```sh
cmake -S . -B build/storage \
  -DCMAKE_BUILD_TYPE=Debug -DQUADEPLUS_STORAGE=ON \
  -DArrow_DIR=/absolute/path/to/Arrow/cmake \
  -DParquet_DIR=/absolute/path/to/Parquet/cmake
cmake --build build/storage -j 4
ctest --test-dir build/storage --output-on-failure
```

The storage profile includes the ten core tests plus four storage targets. Native
packages are discovered through CMake; the project does not silently substitute
a Python wheel or download libraries.

## Sanitizers

Use a separate build directory and `-DQUADEPLUS_SANITIZERS=ON`. This enables
ASan/UBSan on project code. Supply the storage options above if that profile is
required. Prebuilt Arrow/Parquet and system libraries are not instrumented by this
switch.

```sh
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug -DQUADEPLUS_SANITIZERS=ON
cmake --build build/sanitized -j 4
ctest --test-dir build/sanitized --output-on-failure
```

## Optional Databento SDK integration

The supported SDK is `databento-cpp` **v0.67.0**, supplied as a local checkout:

```sh
cmake -S . -B build/databento \
  -DCMAKE_BUILD_TYPE=Debug -DQUADEPLUS_DATABENTO=ON \
  -DDATABENTO_SOURCE_DIR=/absolute/path/to/databento-cpp-v0.67.0
cmake --build build/databento -j 4
ctest --test-dir build/databento --output-on-failure
```

The project does not fetch this checkout. Prepare the SDK's own dependencies using
its upstream instructions. The added CTest is deterministic translation/metadata
verification, not a provider request.

`QUADEPLUS_DATABENTO_SMOKE=ON` separately builds `quant_databento_smoke`. Running it
requires provider credentials and can incur data charges; it is never run by CTest.
It is a provider inspection smoke tool, not an evidence-complete publication path.
Ordinary verification uses synthetic or retained input instead.

## Other options

- `QUADEPLUS_BENCHMARKS=ON` builds `quadeplus_data_model_benchmark`; timing results
  describe that workload and machine, not production throughput guarantees.
- Executables are written to `build/<profile>/bin` by default.
- CLion can open the repository root. Configure explicit dependency paths in its
  CMake profile or an ignored local `CMakeUserPresets.json`.

The original isolated `integration/storage` and `integration/databento` CMake entry
points remain available. The root project is the recommended common entry point.
