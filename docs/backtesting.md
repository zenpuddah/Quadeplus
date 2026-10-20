# Daily research backtesting

Quadeplus includes a native financial host, exact accounting, risk controls and
reports for three strategies. Lexongine owns numerical feature computation;
Quadeplus owns decisions, orders, fills, accounts and P&L.

| Strategy | Included behavior |
| --- | --- |
| SMA20/50 | Long/cash target from completed-bar SMA comparison; next-open execution |
| Keltner/ATR | EMA20, ATR10, channel crossings, risk sizing, protective/trailing stops and drawdown latch |
| Stochastic | Exact five-Bar ratio, episode state and holding limits; fresh and legacy profiles |

Each run has independent cash/buy-and-hold controls and required financial evidence.
The public demo uses authored synthetic prices and does not require market data,
provider credentials, Python scientific packages or Arrow/Parquet.

## Build the native application

Verified native platform: macOS arm64 with Apple C++20/libc++, CMake 3.25+ and
Python 3.12+. Use new checkout names. From a common parent directory:

```sh
git clone https://github.com/zenpuddah/Quadeplus.git
git clone https://github.com/zenpuddah/Lexongine.git
git -C Lexongine checkout --detach aeac6e9112c4e52ca4fbdd5a460e20a81d5af771
git clone https://github.com/leethomason/tinyxml2.git
git -C tinyxml2 checkout --detach 9148bdf719e997d1f474be6bcc7943881046dba1

ROOT="$PWD/Quadeplus"
cmake -S "$ROOT" -B "$ROOT/build/backtest" \
  -DCMAKE_BUILD_TYPE=Release -DQUADEPLUS_BACKTEST=ON \
  -DQUADEPLUS_LEXONGINE_SOURCE_DIR="$PWD/Lexongine" \
  -DLEXONGINE_TINYXML2_SOURCE_DIR="$PWD/tinyxml2"
cmake --build "$ROOT/build/backtest" -j 4
ctest --test-dir "$ROOT/build/backtest" --output-on-failure -R '^backtest_'
```

The exact public dependencies are recorded in `dependencies/backtesting.lock.json`.
Supply explicit source checkouts; the native build does not download them. The
financial host needs the TinyXML2 source checkout for its source-build identity.
Canonical headers must match the engine exactly. Preserve generated binaries,
libraries and native sidecars together in the build directory.

## Generate and open all three synthetic reports

After the build above:

```sh
python3 -B "$ROOT/tools/run_backtest_demo.py" \
  --build-dir "$ROOT/build/backtest" \
  --out "$ROOT/build/synthetic-demo"
open "$ROOT/build/synthetic-demo/index.html"
```

The output directory must be new. The driver uses the existing native fixture
generators, runs SMA, Keltner and fresh stochastic with independent controls, and
checks their financial events and reports against independent Python references.
It does not compile, download inputs or execute the historical research package.

`index.html` links the **12 per-account HTML reports** from three runs. Alongside it:

- `demo.json`: synthetic-run verification summary.
- `sma/`, `keltner/`, `stochastic/`: required HTML/JSON/CSV report triplets,
  `financial.txt`, `summary.json` and effective configuration.
- Per-run `graph.xml`: numerical graph configuration; Keltner also records
  `identities.csv` and `producers.csv` for numerical identity/work inspection.
- `*-input/`: locally generated synthetic CSVs, sealed schedules and daily bundles.

The synthetic index is a convenience page, not the historical suite's acceptance
marker. Synthetic results demonstrate mechanics, not investment performance.

## CLI and input scope

```sh
"$ROOT/build/backtest/bin/quadeplus_backtest" --help
"$ROOT/build/backtest/bin/quadeplus_backtest" run \
  --config /absolute/config.xml --dataset /absolute/daily-bundle --out /absolute/new-run
```

The `prepare` command is intentionally restricted to a specific hash-pinned retained
SPY research package. That package and its historical CSVs are not distributed here.
The fixed historical `suite` command and the XML templates retain their original
profiles; running the full historical suite requires the matching external inputs.
The demo substitutes synthetic dates/symbol in copied configurations, preserving
the original templates and all model rules. Do not treat `prepare` as an arbitrary
CSV importer or bypass the package/daily-bundle integrity checks.

Applications can use `quant::research::prepare_daily_dataset` with explicit input
hashes, a sealed session grid and dataset specification. The fixture generators
illustrate that API; they are not provider/exchange calendar adapters.

## Financial assumptions and limits

- Long/cash, one position and pending intent per account, no pyramiding.
- Prices/quantities/money use fixed-point coefficients; settlement and ledger
  reconciliation are exact. Reporting-only statistics may use floating point.
- Prior-close holdings receive ex-date dividends; previous decisions execute at
  the next opening slot; new decisions follow successful completed-Bar advancement.
- Costs explicitly include commission and adverse slippage. Fees/slippage already
  embedded in equity are not subtracted again from P&L.
- The daily input convention uses split-adjusted OHLC/dividends on the same share
  basis and a declared synthetic clock. Adjusted close is retained evidence, not an
  additional return stream. There is no second split/dividend adjustment.
- Fresh profiles mark final holdings; the legacy profile uses explicit costed
  synthetic final-close liquidation and pre-funding episode reconstruction.
- Fresh scope is bounded to 8,000 rows. Keltner persisted reuse is bounded to 512
  selected rows; full historical runs are cache-disabled. Stochastic has no
  persistent Ratio codec. Lower computation counts do not guarantee lower latency.

See [architecture](architecture.md), [verification](verification.md) and
[development checks](contributing.md). Live trading and general execution-model
configuration are outside this research slice.
