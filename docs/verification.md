# Verification and limits

## Formatting and lint update — 2026-10-20

The formatted public tree passes the pinned [style checks](contributing.md),
including Python lint and a no-change formatter check. A fresh Debug storage build
passes **14/14 core/storage tests** with AppleClang 21 and Arrow/Parquet 26.0.0;
the benchmark target also compiles. Lexongine's four external reader/integration
tests pass against the same formatted headers. The shared canonical headers and
file-integrity helper remain byte-identical across both public repositories.

All 43 existing C++ files reproduce exactly from the pinned formatter applied to
the preceding release source. CMake token sequences and the Python inspector AST
are unchanged. GitHub Actions checks formatting and Python lint; native runtime
verification is the separate macOS profile described here.

## Initial v1.0.0 verification

The standalone source release passed **14/14 core and storage tests** in both normal
and ASan/UBSan builds on macOS arm64 with AppleClang 21 and Arrow/Parquet 26.0.0.
The dependency-free profile passed **10/10** tests. Lexongine's four checked-reader
and persisted-input integration targets also passed against this checkout in a
normal build. The optional benchmark target compiled successfully.

The external Databento SDK checkout and Python PyArrow package were unavailable;
those optional profiles were not rerun for this source release. No dependency was
downloaded and no credentialed provider request was made.

## Profiles

The dependency-free profile runs ten framework-free C++20 tests covering values,
references, queries, ports, quality, planning, synchronous ingestion and retained
integrity evidence. The storage profile adds four Arrow/Parquet targets, including
the generated end-to-end capture/publication/replay demonstration.

```sh
ctest --test-dir build/core --output-on-failure
ctest --test-dir build/storage --output-on-failure
```

Use a separate `QUADEPLUS_SANITIZERS=ON` build for ASan/UBSan. Prebuilt libraries are
not instrumented by the project flag.

## Invariants exercised

- Exact financial values, point-in-time mappings and 64-byte MboRecord layout.
- Half-open/dual-time queries, mapping splits and explain/execute consistency.
- Immutable StreamId bindings, single-stream files and exact reference snapshots.
- Canonical validity, range membership and original raw/row associations.
- Repeated sequences, snapshot rules, filtered gaps and acquisition-scoped order.
- Required ordering/groups/initial state despite permissive Degraded policies.
- Raw evidence and failure retention, explicit revalidation and attempt identity.
- Unsupported schema rejection and checked delivery before callbacks.
- Immutable correction publication, interrupted writes and current-selection failure.
- Full archive versus Replay projection and same-acquisition cross-kind replay.

## Scope limits

The first provider profile is normalized little-endian XNAS MBO. Checks use declared
source evidence and provider semantics; they are not an independent proof of exchange
completeness, authenticity or correct human-supplied mappings. Insufficient session/
reset/initial-state evidence remains unresolved.

The store is synchronous, single-writer and local-POSIX. Per-file verification is
bounded in memory and capture durability is conservative rather than throughput-
optimized. Fault tests do not certify all filesystems or power-loss scenarios.

Automatic repair/recovery scheduling, generalized persistent cache/catalog,
cross-acquisition merging, binary replay, live execution, financial simulation and
Python bindings remain outside this release. Immutable corrected versions alone
do not establish what was available to a historical trading decision.

The optional Databento SDK test requires the supported external v0.67.0 checkout.
Its earlier deterministic verification is separate from this standalone extraction;
no paid request or credentialed smoke is part of normal tests. The optional Python
inspector requires PyArrow and has its own environment prerequisite.
