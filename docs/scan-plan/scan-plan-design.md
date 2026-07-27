# scan-plan-api — design & architecture

Companion to `scan-plan-decisions.md` (which is the *why-we-did-it* log). This doc is the
*how-it-works* reference: the IRC scan-plan API contract, the Part-1 filter-pushdown path,
and — in depth, since it's the least-documented piece — the delete-file model.

Source of truth for the API shape: the vendored specs (both under
`/opt/workspace/src/d/uc/refs/`, the earlier checkout — not copied into this worktree):
`rest-catalog-open-api.yaml` (full Iceberg REST catalog OpenAPI) and
`rest-catalog-open-api--scan-plan.yaml` (a trimmed copy limited to the scan-plan
request/response types). Section/line references below are to the trimmed copy.

---

## 1. What the scan-plan API is, and why we use it

Unity Catalog can expose **catalog-managed** (a.k.a. "managed") Delta tables whose commits are
staged server-side and are *only* readable through the Iceberg REST Catalog (IRC) **scan
planning** endpoints. There is no client-visible Delta log to replay for these tables — the
server does the planning and hands back a concrete list of data files (and their deletes) plus
temporary storage credentials.

The flow, at the table-function level (`src/storage/uc_table_entry.cpp`):

```
DuckDB optimizer  ──pushdown_complex_filter──▶  UCScanPlanPushdownFilter
                                                    │
                    SerializeFiltersToIRC(filters)  │  (WHERE → IRC Expression JSON)
                                                    ▼
                                   UCAPI::PlanTableScan(... filter_json)   ── POST planTableScan
                                                    │
                            UCScanPlanResult { file_scan_tasks[], delete_files[],
                                               plan_tasks[], storage_credentials[] }
                                                    │
             storage_credentials ─▶ CREATE TEMPORARY SECRET (s3)   (httpfs auth)
             file_scan_tasks     ─▶ UCMultiFileList  (lazy; plan_tasks drained on demand)
                                                    │
                       a private copy of `parquet_scan`, with:
                         · get_multi_file_reader = UCMultiFileReaderFactory  (our file list)
                         · deletes wired per-file in UCMultiFileReader::FinalizeBind
                                                    ▼
                                   parquet_scan reads the data files
```

Key architectural choices (each expanded below):

- **We reuse `parquet_scan` wholesale** rather than re-implementing a reader. We hand it our
  file list through a custom `MultiFileReader`, and let its own bind flow do schema detection,
  projection, and row-level filtering.
- **The IRC filter is a server-side *pruning* hint only.** DuckDB keeps the original `WHERE`
  and re-applies it as a Filter operator over the rows parquet returns. The server's filter
  just lets it skip files. (This is what makes the filter path safe even when serialization is
  lossy — with one caveat, see §2.3.)
- **Deletes ride the same `parquet_scan`** via the DuckDB-core `DeleteFilter` seam, exactly the
  way the vendored `delta` extension applies its own deletion vectors.

---

## 2. Part 1 — filter pushdown (the pre-delete work)

### 2.1 Entry point

`UCScanPlanPushdownFilter` is registered as the table function's `pushdown_complex_filter`.
DuckDB's FilterPushdown optimizer calls it once with the collected `WHERE` expressions (it is
called even with an empty filter set, so the scan plan always happens here). On the first call
it: serializes the filters, calls `planTableScan`, installs storage-credential secrets, builds
the `UCMultiFileList`, binds the private `parquet_scan`, and latches `scan_plan_done` so a
second optimizer pass is a no-op.

### 2.2 Filter serialization — `SerializeFiltersToIRC` (`uc_irc_expression.cpp`)

Translates a conjunction of bound DuckDB predicates into IRC `Expression` JSON. Supported:

| DuckDB expression                         | IRC term                          |
|-------------------------------------------|-----------------------------------|
| `col <cmp> const` / `const <cmp> col`     | `eq`/`not-eq`/`lt`/`gt`/`lt-eq`/`gt-eq` (flipped if const-on-left) |
| `AND` / `OR` of supported terms           | nested `{"type":"and"/"or", "left":…, "right":…}` |
| `col IS [NOT] NULL`                        | `is-null` / `not-null`            |
| anything else                             | dropped (see rules below)         |

Two structural facts worth knowing:

- **Comparisons are `BOUND_FUNCTION` nodes now, not `BOUND_COMPARISON`.** Current DuckDB binds
  `a = 1` as a scalar-function call; `ExpressionClass::BOUND_COMPARISON` is legacy/deserialize-
  only. So the code detects comparisons via `BoundComparisonExpression::IsComparison(expr)` and
  reads operands with `BoundComparisonExpression::Left/Right(cmp)` after casting to
  `BoundFunctionExpression`. (This was the one real structural port in Part 1 — see the
  decisions log.)
- **Unsupported-child handling is direction-aware and conservative.** In an `AND`, an
  unrepresentable child is simply dropped (the remaining conjunction is a valid *weaker*
  constraint — safe to send). In an `OR`, an unrepresentable child collapses the *whole* `OR`
  to "no constraint" (an `OR` is only as prunable as its least-prunable branch). This keeps the
  server filter a strict over-approximation of the real predicate: it may return *too many*
  files, never too few — **provided each serialized term is itself exact** (§2.3).

### 2.3 The filter is a pruning hint — with a precision caveat

Because DuckDB re-applies the original `WHERE`, an *over*-approximate server filter is
harmless: extra files are read and then filtered out row-by-row. The one way this becomes a
**correctness** problem is if a serialized term is *more* restrictive than the true predicate —
then the server prunes a file that actually holds matching rows, and those rows never reach
DuckDB to be recovered.

That is exactly the risk in `ValueToIRCJson` for floating-point constants (flagged and fixed in
the review pass — see §5): `std::to_string(double)` emits 6 fixed decimals, which both rounds
(potentially tightening a bound) and can emit non-JSON tokens (`inf`/`nan`). Integer, boolean,
and string constants are exact and unaffected. The tests exercise only integer keys, so this
never surfaced in-suite.

### 2.4 The residual filter is decoded but not re-applied

Each `FileScanTask` may carry a `residual-filter` (the server's predicate *after* partition
pruning). We parse it into `residual_filter_json` but don't translate it back into DuckDB
expressions — DuckDB's own Filter operator already covers row-level correctness, so re-applying
the residual would be redundant work. Translating it back would only be worth it as an
optimization (skip DuckDB's redundant row filter), and needs bidirectional IRC↔DuckDB
expression translation we don't have.

---

## 3. The delete-file API contract (what the server provides and expects)

This is the part not previously written down. All types are in the scan-plan yaml under
"Scan planning request/response types" (§ around lines 820-1075).

### 3.1 Where deletes live in the response

`ScanTasks` (the completed-plan payload) carries **two parallel arrays**:

```yaml
ScanTasks:
  file-scan-tasks: [ FileScanTask, … ]   # one per data file
  delete-files:    [ DeleteFile, … ]     # the pool of delete files, shared
```

and each `FileScanTask` points into the pool by index:

```yaml
FileScanTask:
  data-file: DataFile                    # required
  delete-file-references: [int, …]       # 0-based indices into THIS response's delete-files
  residual-filter: Expression            # optional (see §2.4)
```

Two consequences the implementation must respect, and does:

1. **The indices are response-scoped, not global.** A `plan-task` fetched later comes back with
   its *own* `delete-files` array, and its tasks' references index into *that* one. So the
   resolution from `delete-file-references` → concrete `DeleteFile` records must happen at the
   moment each task is ingested, before that response's array goes out of scope. `UCMultiFileList`
   does this in `AddFileScanTask(task, all_delete_files)` (called from both the inline-seed path
   and `ExpandNextPath`), storing the resolved records in `file_deletes[i]` parallel to
   `expanded_files[i]`. A reference past the array's end is treated as a malformed response
   (`IOException`).
2. **One delete file can be referenced by several data files** (e.g. a single position-delete
   file covering a whole partition). That's why positional-delete application filters by data
   file path (§4.2).

### 3.2 The `DeleteFile` type — a discriminated union

`DeleteFile` is a `oneOf` discriminated by its `content` string:

```
ContentFile (base: spec-id, partition, content, file-path, file-format,
             file-size-in-bytes, record-count, …)
  ├─ content="data"              → DataFile           (column stats, bounds — the data files)
  ├─ content="position-deletes"  → PositionDeleteFile
  └─ content="equality-deletes"  → EqualityDeleteFile
```

`FileFormat` ∈ { `avro`, `orc`, `parquet`, `puffin` }.

**`PositionDeleteFile`** — "row at position N of data file F is deleted." Adds:
- `content-offset` (int64, optional) — offset *within* the delete file where the delete content
  begins.
- `content-size-in-bytes` (int64, optional) — length of that content; required when
  `content-offset` is present.

These two fields are the crux of the two physical sub-forms (§4):
- **absent** → the delete file is a *whole* file of `(file_path, pos)` rows (classic Iceberg v2,
  `file-format: parquet` in practice).
- **present** → the delete content is a **deletion-vector blob packed at a byte range** inside a
  larger file (Iceberg v3 / puffin `deletion-vector-v1`). `content-offset`/`content-size` locate
  the blob.

**`EqualityDeleteFile`** — "any row matching these column *values* is deleted." Adds:
- `equality-ids` (array of int) — the Iceberg **field-ids** of the columns whose values define a
  match. The delete file itself is a data file containing those columns; a data-file row is
  deleted if it equals any delete row on exactly the `equality-ids` columns.

### 3.3 What the server expects of the client

Nothing in the request selects deletes — the client sends `planTableScan` (optionally with a
filter), and the server decides which delete files apply to which data files and returns the
references. The client's contract is purely to **apply** them: for each data file, subtract the
rows its referenced delete files mark, before returning rows to the query. The mapping from
"which delete rows" to "which data-file rows" is:
- positional: by absolute row position within the data file;
- equality: by matching the `equality-ids` column *values*.

---

## 4. Part 2 — how deletes are applied

All application funnels through `BuildUCDeleteFilter(context, data_file_path, deletes)`
(`uc_multi_file_list.cpp`), which returns a DuckDB-core `DeleteFilter` (or `nullptr` for none)
that `UCMultiFileReader::FinalizeBind` hangs on `BaseFileReader::deletion_filter` — the exact
seam the `delta` extension uses (`DeltaMultiFileReader::FinalizeBind`). Once set, parquet's
scan calls `DeleteFilter::Filter(start_row_index, count, result_sel)` per chunk and the deleted
rows are dropped natively.

### 4.1 The common shape: `UCPositionDeleteFilter`

Both positional sub-forms reduce to **a set of absolute deleted row positions** within the data
file. `UCPositionDeleteFilter` holds an `unordered_set<int64_t>` and, per chunk, keeps rows
whose `start_row_index + i` is *not* in the set. This mirrors `delta`'s `DeltaDeleteFilter` and
iceberg's `IcebergPositionalDeleteFilter` (same contract, O(1) membership).

### 4.2 Sub-form A — classic parquet position-delete file

`content-offset` absent. The file is a parquet table of `(file_path VARCHAR, pos BIGINT, [row])`.
`ScanPositionalDeleteFile` reads it by binding and fully draining a private `parquet_scan`
(delete files are small — KB to low MB — so full materialization is fine; the bind/scan dance
is factored into `ScanParquetFile`). Because one delete file can reference multiple data files,
it keeps only rows whose `file_path` column equals **this** data file's path.

That path comparison uses `UCMultiFileList::GetDataFilePath(idx)` — the *literal* `file-path`
string the server sent — deliberately **not** `BaseFileReader::GetFileName()`, which reflects
whatever DuckDB's parquet reader normalized the path to. A silent normalization mismatch there
would make deletes quietly fail to apply (wrong results, no error), so the literal-string
compare is the safe choice. `avro`/`orc` position-delete files raise `NotImplementedException`
(UC's Delta-via-UniForm path produces parquet).

### 4.3 Sub-form B — Iceberg v3 deletion vector (puffin)

`content-offset` present. `ScanDeletionVectorFile` reads exactly the
`[content-offset, content-offset + content-size)` byte range and decodes it as a bare
`deletion-vector-v1` blob via `UCDeletionVectorData::FromBlob` (`uc_puffin.cpp`). The blob is:

```
vector_size(4, BE) | magic(4)=D1 D3 39 64 | n_bitmaps(8, LE) |
{ key(4, LE) | portable-roaring-bitmap }*  | CRC32(4, BE)
```

Each `key` is the high 32 bits of a bucket; the roaring bitmap holds the low 32 bits, so one
blob addresses the full int64 position range in 2³²-sized buckets. `ToSet` reassembles absolute
positions as `(key << 32) | value`. A CRC32 over the magic-onward bytes is verified. The decoder
is a **reader-only port of ducklake's `ducklake_puffin.cpp`** (same MIT license / same
foundation) using the CRoaring vcpkg port (`roaring`) — added to `vcpkg.json` and linked in the
top-level `CMakeLists.txt` following ducklake's own pattern.

`uc_puffin.cpp` also contains a full puffin *container* reader (`UCPuffinReader`: magic + JSON
footer + per-blob metadata), for the case a server ever points at a whole puffin file rather
than a bare blob. The scan-plan path never exercises it — see §5.

### 4.4 Equality deletes — deliberately not implemented

`BuildUCDeleteFilter` raises `NotImplementedException` (naming the exact gap) the moment any
referenced delete is an equality delete. **Why it's hard here specifically:** applying one
correctly requires mapping each `equality-ids` field-id to *this table's* output column. The
iceberg extension gets that mapping from a full field-id-carrying Iceberg schema it fetches
separately from the REST catalog; UC's scan-plan path binds via a bare `parquet_scan` and never
threads a field-id-carrying schema through at all. Matching by *column name* instead would be
silently wrong under a column rename, so the code fails loud rather than guessing. Ducklake
draws the same boundary (positional/DV only, no equality-delete application), which is a useful
confirmation this is a scope line, not an oversight. Building it is a real design task (fetch +
thread the schema), not a follow-up patch — see decisions log open-question #3.

---

## 5. Known gaps, assumptions, and review notes

The formerly load-bearing assumption is now **live-verified**: a DV-enabled `DELETE` on a
Databricks catalog-managed table *does* surface through the scan-plan endpoint as a
`PositionDeleteFile` with `content-offset` pointing at a `deletion-vector-v1` puffin blob.
`scan_plan_deletes.test` passed against a live warehouse, and it now asserts the puffin/DV path
specifically (via the `api-irc.DeletionVector` log line) — not just that rows were dropped.

Design points from the review pass — status updated:

- **`UCPuffinReader` container path is no longer dead code.** The `uc_read_deletion_vector` table
  function's whole-file form (`functions/uc_deletion_vector.cpp`) reads a puffin container via
  `UCPuffinReader`; the scan-plan hot path still calls `FromBlob` directly on the byte range. So
  the container reader now has a real caller (review-finding S1 resolved by use, not deletion).
- **Truncated-blob over-read (C3) — fixed.** `FromBlob` guarded only `blob_length < 12`; the
  smallest legal blob is 20 bytes, so a declared `content-size` of 12–19 could read past the
  buffer before the bound check fired. Now guarded (min 20 + a pre-`Load` bound).
- **Float filter serialization precision — fixed** (`%.17g` round-trip + non-finite guard; §2.3).
- **DV routing keys on `content-offset >= 0`** rather than `file-format == "puffin"`. Correct per
  the API's intent; a `file-format` cross-check would catch a malformed response earlier. Reserved.
- **`CRC32`'s static lookup table initializes without a lock** — a benign-but-real data race if
  two DV blobs decode concurrently (inherited from the ducklake port). Reserved.

## 6. Async planning, cancellation, and SQL tooling

**Async poll (`UCAPI::PlanTableScan`, `uc_api.cpp`).** `planTableScan` may return `submitted`;
the client then polls `fetchPlanningResult` until a terminal status (`completed`/`failed`/
`cancelled`). The loop honors the response `Retry-After` header (clamped to 60s) when present,
else backs off exponentially 100ms→1s; it has no fixed iteration cap (a slow server plan must
not be cut off) but a 5-minute wall-clock budget as a backstop. Each sleep is chunked so a query
cancel (`ctx.InterruptCheck()`) lands within ~100ms, and on abort (cancel / timeout / mid-poll
error) the plan is best-effort `DELETE`d so the server can release it.

**Inspection / test tooling (SQL table functions).**
- `uc_read_deletion_vector(path [, content_offset =>, content_size =>])` — decodes a DV to its
  deleted positions (byte-range → `FromBlob`, or whole file → `UCPuffinReader`). Public-utility
  naming; a shared-lib candidate for a non-UC-specific reader.
- `__internal_uc_plan_table_scan(endpoint, catalog, schema, table, token [, filter =>])` —
  drives `PlanTableScan` directly (bypassing catalog attach). `__internal_` per convention
  (core `__internal_*`, `__internal_delta_ccv2_commit_staged`) — it's test/inspection plumbing.

**Offline test.** `test/irc/test_irc_api_retry.py` runs a Python mock HTTP server (3 scan-plan
routes) against the built `duckdb` CLI + `__internal_uc_plan_table_scan`, asserting the poll
behavior (submitted→completed, Retry-After pacing, failed status, cancel→`DELETE`, filter-in-body)
with no creds — the surface a live test can't easily exercise.
