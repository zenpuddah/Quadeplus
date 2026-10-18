# Storage, evidence and replay

## Formats

| Representation | Contract |
| --- | --- |
| Raw retained provider data | `quant-provider-bytes-v1`, original normalized record bytes plus ordinal |
| Canonical/consumer observations | `quant-observations-v2`, MBO/Trade/OrderExecution plus row StreamId and acquisition ordinal |
| References and validation evidence | Bounded versioned `quant-evidence-v1` token-text records |
| Completion | Immutable manifest plus `COMPLETE` marker binding required files and digests |

Price uses scale 9, quantity scale 6 and timestamps epoch nanoseconds. Unknown
encodings and known labels with incompatible columns/meaning reject before row
delivery. Legacy `canonical-v1` files are not eligible internal inputs and are not
silently relabeled.

Product version v1.0.0 and observation encoding v2 identify different things.

## Publication

The synchronous local store retains raw acquisition evidence before conversion.
Canonical preparation records original row associations and validation outcomes.
Publication writes separate raw, reference, canonical, archival-consumer and evidence
files, checks their content and associations, then exposes a non-replacing completion
marker. Required files are flushed/synced before completion; local POSIX semantics
are the supported storage boundary.

Existing published versions cannot be overwritten, even with equal content.
Corrections receive new versions. A separate atomic current-selection update does
not mutate existing version handles. New acquisition IDs distinguish new network
input; preparation/publication retries use explicit attempts and retained-input
identity.

Layout:

```text
staging/<operation>/<attempt>/<acquisition>/
catalog/
acquisitions/
versions/<version>/.attempt-<attempt>/
versions/<version>/COMPLETE
current
```

The small create-only identity catalog is not a general database-backed discovery
service. Staging is not a persistent ingestion cache or automatic repair engine.

## Checked access

`read_batches_checked` accepts explicit consumer requirements. Ordered replay uses
`stream_ordered_events` and cannot weaken mandatory ordering/group checks; MBO
consumption additionally needs adequate initial state. A reader verifies the complete
package before invoking consumers and preserves exact versions and references.

The archival Full projection includes original source-byte witnesses. Replay omits
that archive-only column while retaining canonical meaning and binding information.
Partitions from one acquisition merge by the shared ordinal, not timestamps.

## Synthetic demonstration

With the storage profile built:

```sh
build/storage/bin/quant_storage_manual_round_trip
```

The executable creates a disposable local directory. To retain the output, pass a
fresh explicit path:

```sh
build/storage/bin/quant_storage_manual_round_trip /absolute/new/demo-directory
```

It captures generated provider records, reprocesses them without a provider request,
publishes/replays a complete version, keeps an invalid candidate staged, and publishes
a correction while the old version retains its original value.

An optional Python inspection utility requires a compatible installed `pyarrow`:

```sh
python3 integration/storage/inspect_parquet.py /absolute/new/demo-directory/versions/demo-v1
```

It independently inspects schemas, rows and package/file/parent digests. It does not
replace the C++ eligibility gate or prove provider authenticity.
