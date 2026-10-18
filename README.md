# Quadeplus

C++20 market-data foundations for research: canonical financial types, historical
ingestion, retained source evidence, immutable datasets and checked replay.

**v1.0.0 — data foundation**

## Capabilities

- Exact fixed-point prices, quantities and money; point-in-time instrument mappings.
- A 64-byte, cache-aligned MBO record and canonical Trade/OrderExecution types.
- Provider-independent queries and synchronous ingestion with inspectable plans.
- Stream identity, acquisition-scoped ordering and source/canonical validation.
- Separate raw, canonical and consumer evidence with immutable publication.
- Arrow/Parquet storage and checked batch/event access through `MarketDataReader`.
- An optional Databento XNAS.ITCH historical MBO adapter.

The current implementation is a data layer, not a strategy engine or complete
backtester. Orders, fills, execution/cost models and portfolio/PnL accounting are
outside this release. [Lexongine](https://github.com/zenpuddah/Lexongine) is the
separate computation engine.

## Build the dependency-free core

Requires CMake 3.25+ and a C++20 compiler. The verified environment is macOS arm64
with AppleClang; the storage implementation uses local POSIX filesystem semantics.

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core -j 4
ctest --test-dir build/core --output-on-failure
```

This builds ten deterministic tests without Arrow or the Databento SDK. Include
headers from `src/` or link the CMake interface target `Quadeplus::core`.

## Build storage and run the synthetic demonstration

Supply installed Arrow and Parquet CMake packages:

```sh
cmake -S . -B build/storage \
  -DCMAKE_BUILD_TYPE=Debug -DQUADEPLUS_STORAGE=ON \
  -DArrow_DIR=/absolute/path/to/Arrow/cmake \
  -DParquet_DIR=/absolute/path/to/Parquet/cmake
cmake --build build/storage -j 4
ctest --test-dir build/storage --output-on-failure
build/storage/bin/quant_storage_manual_round_trip
```

The demonstration uses generated retained input, publishes immutable versions and
replays them. It needs no credentials or market-data download. See
[build options](docs/build.md), [architecture](docs/architecture.md) and
[storage/replay](docs/storage.md).

## Integrity model

A row's StreamId resolves to a fixed instrument/venue/source tuple. A separate
ordinal preserves original succession within one acquisition before filtering or
splitting by data kind. Validity, factual data quality and consumer eligibility are
distinct: a permissive quality policy cannot waive required ordering, event groups
or initial state.

Published datasets retain their references and validation evidence. Corrections
create new versions; current selection never rewrites an old version. Readers check
the complete package before delivering rows. Timestamp sorting and checksum equality
are not proofs of provider completeness.

## Scope

The first provider profile is normalized Databento XNAS MBO. Persisted data kinds are
MBO, Trades and OrderExecution. The supported dataset encoding is
`quant-observations-v2`; its version is independent of the product's v1 release.

Ingestion caching remains `NoopCache`. Automatic repair, cross-acquisition merging,
live trading, real-feed Bar construction and Python bindings are not implemented.
The optional Python utility inspects stored files; it is not a binding layer.

See [verification and limits](docs/verification.md) for tested guarantees and the
separate status of the optional provider SDK integration.
