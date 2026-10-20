# Architecture

```text
query + exact reference/mapping snapshot
    -> query planning and provider request
    -> retained provider records and acquisition ordinals
    -> source interpretation, selection and canonical validation
    -> staged canonical candidates and failure evidence
    -> immutable raw/canonical/consumer package
    -> compatibility/integrity/eligibility gate
    -> checked batches or ordered event replay
```

## Boundaries

| Module | Responsibility |
| --- | --- |
| `quant/data` | Canonical IDs, values, observations, references and row validators |
| `quant/ingestion` | Canonical queries, mapping/fetch plans, provider ports, quality and synchronous orchestration |
| `quant/integrity` | Acquisition/reference identity, original associations, validation reports and consumer requirements |
| `quant/storage` | Evidence codecs, retained capture, Arrow interchange, immutable publication and checked readers |
| `quant/research` | Immutable daily research bundles, exact normalization, sealed schedules and causal Bar projections |
| `quant/backtest` | Financial host, exact settlement/ledger, orders/fills, risk and reports |
| `quant/strategies` | SMA, Keltner/ATR and stochastic numerical features and decision policies |

Provider dataset/schema/symbol conventions stay behind adapters. Consumers use
`MarketDataReader` rather than the concrete Parquet implementation. Query selection
and save/publication policy remain separate.

## Identity and time

Instrument identity is canonical; provider symbols/IDs are point-in-time reference
evidence. A nonzero uint64 StreamId resolves to an immutable instrument/venue/source
tuple. Each canonical file contains one stream.

Event time and source receive time are distinct. Both requested constraints apply
after legitimate overfetch filtering. Time ranges are half-open. Mappings resolve
on event time; receive-only queries cannot invent event-time mapping coverage.

An acquisition-scoped uint64 ordinal is assigned before filtering and data-kind
splitting. It is separate from provider sequence numbers and stays outside the
64-byte MboRecord. Selected ordinal gaps are legitimate. Independent acquisitions
are not implicitly comparable or mergeable.

## Quality and eligibility

`DataState` describes factual range-local quality. It does not itself authorize a
consumer. Eligibility additionally checks structural/range/identity validity and
the consumer's required ordering, event-group and initial-state evidence.

Invalid or unresolved records remain staged with their original bytes, ordinals and
failure reasons. Retaining good rows does not prove full requested coverage. Explicit
recovery revalidates retained or newly acquired input; it does not silently clear
earlier failures.

## First provider profile

Databento XNAS.ITCH MBO is normalized into canonical MBO, Trade and OrderExecution
observations without exposing a provider-specific Fill type as canonical state.
Source checks preserve returned succession, allow documented repeated sequences and
snapshot behavior, and keep unresolved groups/initial state ineligible. Event-time
sorting and consecutive numbers are not substitutes for those checks.

The provider representation is SDK-normalized records, not original exchange packet
capture. Source evidence does not independently prove exchange completeness or the
correctness of supplied mappings.

## Integration with Lexongine

Lexongine's checked Trade adapter can use this repository as its explicit external
Qaedalus source checkout. Set `LEXONGINE_QAEDALUS_SOURCE_DIR` to this repository when
building that optional integration. The canonical data headers and reader APIs are
preserved from the compatible baseline; Lexongine checks shared type compatibility.
The computation engine itself is not included here.

## Daily research host

The daily research path is separate from canonical market-data Parquet schemas:

```text
retained daily CSV + sealed schedule + explicit input hashes
    -> immutable daily research bundle
    -> dividend and opening projections
    -> prior intent execution and exact ledger posting
    -> Lexongine completed-Bar numerical step
    -> close valuation, risk and next intent
    -> required report closure and acceptance
```

Numerical feature observers have no account capability. Independent financial
accounts remain separate even when their pure features share computation. Typed
Price and Ratio observations must be admitted before funding. Source exhaustion
is a separate handshake, not an inference from the last Bar. Failed runs retain
their prefix and cannot publish accepted numerical cache origins.

The public [synthetic demonstration](backtesting.md) exercises this host without
historical data. Its reports and source/build identities use the same native
application as externally supplied compatible research bundles.
