# scan-plan-api--v1-main — decisions log

Written for review when picking this back up. Covers: bringing `origin/scan-plan-api--p1`
forward onto current history, and the delete-files (position/equality/deletion-vector)
follow-on work. Everything here is **local only, not pushed** — no push credentials in this
environment, and per instruction this branch stays local until reviewed.

## 1. Rebase ordering and scope

**Decision:** `upstream/main` (the true SoT for pre-scan-plan functionality) → rebase
`features/uc-pytest-driver-support`'s 6 commits on top → rebase `scan-plan-api--p1`'s 2
unique commits on top of that.

**Why this order:** confirmed `features/uc-pytest-driver-support` touches **zero files under
`src/`** (verified via `git diff --stat` against its merge-base with `upstream/main`) — it's
test/scripts-only, so there was nothing to reconcile between it and core code. `upstream/main`
was only 1 commit ahead of that merge-base, and that commit only touches `.github/workflows/`.
So the rebase was a clean two-step cherry-pick sequence, not a real 3-way integration — see the
conversation for the `v1.5-variegata`-vs-`upstream/main` dead end that preceded this (I initially
compared against the wrong parallel line; `upstream/main` is the one that matters here).

**Scope note:** `origin/scan-plan-api--p1` itself has 6 commits, but 4 are already absorbed
into current history under different hashes (a prior forward-fill). Only `6e04191` ("stake in
the ground: scan plan + pushdown filter") and `23c2f73` ("quick fixes") are genuinely unique;
those are the 2 that got rebased.

## 2. Conflict resolutions (rebasing `6e04191`/`23c2f73`)

All resolved by hand, favoring current `HEAD`'s design where the two sides actually diverged
in intent, and preserving the old branch's real value-adds:

- **`Makefile`**: dropped the old branch's `test_data_prepare` target (used the deleted
  `scripts/databricks_data_gen/generate_databricks_test_data.py`, superseded by
  `databricks_gen`'s auto-provisioning from `test/databricks/data/*.sql`). Kept HEAD's
  pytest-driven `test_release_internal`.
- **`TableInformation::MarkDirty`**: kept HEAD's `(const lock_guard<mutex> &_attach_lock)`
  signature (a later locking-discipline hardening the old branch predates) over the old
  branch's no-arg version. No new call sites needed updating — only one caller exists
  (`uc_delta_ccv2_commit.cpp`), already on the new signature.
- **`TableInformation::IsCatalogManaged`**: **kept a real addition** from the old branch — an
  `if (table_data->table_type == "MANAGED") return true;` check (OSS UC signals managed
  tables via `table_type` directly) that HEAD didn't have. Confirmed `table_type` already
  exists on `UCAPITable` and is populated, so this was safe to fold in.
- **`UCAPI::LoadTable` / `UpdateTable`** vs the old branch's `GetCommits`/`PostCommit`: kept
  HEAD's versions. Confirmed `GetCommits`/`PostCommit` are fully superseded (grepped — nothing
  outside the conflicting hunk itself referenced them) — `LoadTable`/`UpdateTable` are the
  current delta.yaml v1 protocol implementation.
- **`UCAPI::GetTableCredentials`**: kept HEAD's dual-path version (external tables via
  `temporary-table-credentials`, catalog-managed via the `/delta/v1/.../credentials`
  endpoint) over the old branch's single-path version.
- **Removed a real bug the merge would otherwise have left in**: a stray leftover debug log
  line inside the new `LoadTable` body (`UC_LOG_DEBUG(ctx, "uc-api.GetCommits table_id=%s ...",
  table_id, ..., result.latest_table_version)`) referencing `table_id` and
  `result.latest_table_version` — neither exists in the current signature/struct
  (`LoadTable` takes `catalog_name`/`schema_name`/`table_name`, not `table_id`;
  `UCAPICommitsResult` has `ratified_version`, not `latest_table_version`). This wasn't
  actually inside either `<<<<<<<`/`>>>>>>>` conflict block — it fell in a region git
  considered "unconflicted" — so it would have silently landed as a compile error (or worse,
  compiled against stale identifiers if any existed) if not caught by inspection. Deleted;
  the function already has an equivalent, up-to-date debug log at its `return`.
- **`test/sql/databricks/scan_plan.test`**: git flagged this as a location conflict (the
  `test/sql/databricks/` → `test/databricks/` rename happened on the pytest-driver-port side).
  Moved to `test/databricks/scan_plan.test`, kept verbatim for this commit — the pytest-driver
  model rework is a separate, later commit (see §3) so the "faithful port" and "intentional
  rework" diffs stay independently reviewable.

## 3. Test rework (separate commit from the rebase)

`scan_plan.test` landed via the rebase still on the pre-port `require-env
DATABRICKS_TOKEN/ENDPOINT/REGION` + raw `CREATE SECRET` pattern, with `CATALOG`/`SCHEMA`
hardcoded to `duckdb_testing`/`main`. Reworked into a `.py` driver + `.test` body pair matching
`time_travel.py`'s shape:

- `test/databricks/scan_plan.py` — `@requires(source=f"{config.READ_CATALOG}.main.scan_plan_days_managed", access="ro")`.
- `test/databricks/data/scan_plan_days_managed.sql` — moved from
  `scripts/databricks_data_gen/custom_data_sources/`; content unchanged (already used the
  current `{table_name}` templating), only the stale header comment (pointing at the deleted
  generator script) dropped.
- `test/databricks/scan_plan.test` — `ATTACH '{CATALOG}' AS unity (..., DEFAULT_SCHEMA
  '{SCHEMA}', scan_plan_endpoint '{DATABRICKS_SCAN_PLAN_ENDPOINT}')`, tables referenced bare
  after `USE unity` instead of schema-qualified.
- Dropped `scripts/databricks_data_gen/duckdb_data_sources/irc_tables.sql`
  (`irc_simple_table`) — grepped the whole repo, nothing referenced it.
- `DATABRICKS_SCAN_PLAN_ENDPOINT` stays a plain ambient `require-env` var (matching how
  `DATABRICKS_TOKEN`/`ENDPOINT`/`REGION` already work post-port — they flow through
  `os.environ` via the credentials-broadcast pipeline, not through `resources.env`) since
  it's workspace config, not a per-test provisioned value.

## 4. DuckDB core API drift (the bigger-than-expected part)

`origin/scan-plan-api--p1` forked from an old snapshot; `upstream/main`'s DuckDB core has moved
~8800 commits since. The branch didn't just have merge conflicts — `uc_table_entry.cpp` and
`uc_irc_expression.cpp` didn't **compile** against current core. Fixed, not just patched around:

- `CreateSecretInput::name`/`type`/`provider` are now `Identifier`, not `string`. Fixed the one
  concatenated-string assignment to `sec.name` (`Identifier(...)` explicit construction) — the
  other two were already string-literal assignments, which construct implicitly.
- `Catalog::GetEntry(context, schema, name)` is deprecated in favor of
  `GetEntry<T>(context, QualifiedName(...))`. Migrated the one call site (looking up
  `parquet_scan`) to `QualifiedName({Identifier(DEFAULT_SCHEMA)}, Identifier("parquet_scan"))`
  — this is a real behavior-preserving migration, not just silencing a warning, since the
  deprecated overload would still have worked; did it anyway since the whole exercise is about
  landing on the *current* pattern, and I reused the exact same call twice more while writing
  the delete-files code (see §5), so getting it right once mattered.
- `TableFunctionBindInput`'s 4th constructor arg is now `vector<Identifier>&`, not
  `vector<string>&`. Retyped the one local (`input_table_names`) at the call site.
- `BoundOperatorExpression::children` / `BoundConjunctionExpression::children` are now
  private; use `.GetChildren()`.
- `BaseExpression::type` is now protected; use `.GetExpressionType()`.
- `BoundColumnRefExpression::GetName()` now returns `Identifier`, not `string&`. Copied to a
  local `string` via `.GetIdentifierName()` rather than binding a reference — binding
  `const string&` to a temporary `Identifier`'s member would dangle (the temporary's lifetime
  doesn't extend through a member-function call).
- **The one structural rewrite, not just a signature fix:** `ExpressionClass::BOUND_COMPARISON`
  no longer exists (renamed `LEGACY_BOUND_COMPARISON`, kept only for deserializing old
  serialized plans) — comparisons (`=`, `<`, etc.) now bind as `ExpressionClass::BOUND_FUNCTION`
  nodes. Rewrote the `case` in `ExprToIRCJson` to `case ExpressionClass::BOUND_FUNCTION:` with
  an `if (!BoundComparisonExpression::IsComparison(expr)) return "";` guard, cast to
  `BoundFunctionExpression`, and extract operands via
  `BoundComparisonExpression::Left/Right(cmp)` instead of `.left`/`.right`. Verified this exact
  pattern against `duckdb/src/function/table/table_scan.cpp` and
  `duckdb/src/optimizer/filter_combiner.cpp` (both already use it) before applying it, rather
  than guessing at the new shape.

**Verification:** the targeted `unity_catalog_extension` static-lib link succeeded after these
fixes (confirms `uc_table_entry.cpp`/`uc_irc_expression.cpp` compile clean against current
core) — see the "Build verification" section below for what's confirmed vs. still pending.

## 5. Delete-files: what's implemented, what isn't, and why

The rebased `6e04191` already **parses** delete-files in full (`UCScanDeleteFile`,
`ParseDeleteFile`, `ParseScanTasksPayload` in `uc_api.cpp`) but never **applied** them — the
pre-existing `UCMultiFileList` constructor called `AssertNoDeleteFiles`, which `D_ASSERT`-crashed
(debug builds) the moment a scan-plan response actually contained one. This session's task was
to replace that with real application.

**Research before writing anything:** read the vendored `delta` extension's own
`DeltaDeleteFilter` (the exact `DeleteFilter` pattern used for Delta's native deletion vectors —
confirms `DeleteFilter::Filter(start_row_index, count, result_sel)` is the right seam), then —
per your pointer mid-session — the checked-out `src/d/iceberg` and `src/d/ducklake` repos, both
of which have complete, independent implementations of exactly this problem (Iceberg
position-deletes, equality-deletes, and deletion vectors). Design below is grounded in reading
their actual code, not guessed at.

### Implemented: positional deletes (both sub-kinds), full application

- **Classic parquet-format position-delete files** (`content_offset` absent): a plain
  `(file_path VARCHAR, pos BIGINT, [row])` parquet file — read via a nested `parquet_scan`
  bind/init/scan loop (same 4-step dance `UCScanPlanPushdownFilter` already uses for the main
  data scan, factored into `ScanParquetFile` in the new `uc_multi_file_list.cpp`), filtered to
  rows whose `file_path` matches the data file being read (one delete file can reference
  multiple data files). avro/orc position-delete files raise `NotImplementedException` — not
  expected in practice for this table shape (Delta-via-UniForm is Parquet-based), and not
  something I could verify either way.
- **Iceberg v3 deletion vectors** (`content_offset`/`content_size_in_bytes` present): a
  `deletion-vector-v1` puffin blob. Implemented in new files `src/uc_puffin.{hpp,cpp}`, **ported
  from `ducklake`'s `storage/ducklake_puffin.{hpp,cpp}`** (reader half only — UC never writes
  deletion vectors) — same license, same org (Stichting DuckDB Foundation), so a direct port
  with attribution rather than a from-scratch reimplementation of a binary format I have no
  independent spec access to verify against. Covers: puffin container parsing (magic + JSON
  footer, or a bare blob with no container), CRC32 checksum verification, and the
  `deletion-vector-v1` blob's roaring-bitmap-per-high-32-bits encoding.
- Both reduce to the same shape — an absolute set of deleted `int64_t` row positions — served
  by one `UCPositionDeleteFilter : public DeleteFilter` (mirrors `DeltaDeleteFilter` /
  `IcebergPositionalDeleteFilter`, same `Filter()` contract).
- **New third-party dependency: `roaring`** (CRoaring, via vcpkg — `"dependencies": ["roaring"]`
  added to `vcpkg.json`; `find_package(roaring CONFIG REQUIRED)` + link added to the top-level
  `CMakeLists.txt`, copied from `ducklake`'s exact pattern). This is the one piece whose build
  I have **not** confirmed compiles end-to-end in this environment — see "Build verification."

### Deliberately not implemented: equality deletes

`BuildUCDeleteFilter` raises `NotImplementedException` (not a silent skip) for any equality-delete
file, with a message naming exactly what's missing. Why, concretely:

- Applying an equality-delete correctly requires resolving each Iceberg **field-id** in its
  `equality-ids` to *this table's own output column* — both the iceberg extension's and (by
  omission) ducklake's implementations confirm this needs a field-id-carrying schema for the
  table being scanned (`global_columns[i].identifier` in `MultiFileReader::FinalizeBind`'s own
  parameters, in the iceberg extension's case, populated by ITS OWN specialized schema-aware
  bind path).
- UC's scan-plan path binds via a bare `parquet_scan` over the returned file paths (see
  `UCScanPlanPushdownFilter`) — it never fetches or threads through a field-id-carrying schema
  for the table at all. `global_columns[i].identifier` would be null throughout. Building that
  would mean either fetching the Iceberg table's schema separately (no such `UCAPI` call exists
  today) or reading field-ids off the *data* files' own parquet metadata and hoping they're
  consistent across files (schema-evolution edge cases aside).
- Given that gap is real infrastructure, not a shortcut, and that **ducklake — a mature,
  actively-developed sibling project — doesn't implement equality-delete application at all**
  (grepped its `storage/ducklake_delete*.cpp`; positional/DV only), I judged matching that scope
  boundary (with a loud, specific error) more defensible than either (a) guessing at
  name-based column matching, which is wrong the moment a column is renamed and would silently
  under-delete, or (b) spending the remaining time on new schema-fetch infrastructure with no
  way to verify it against a live server.
- If/when this needs to actually work: the shape to build is in
  `src/d/iceberg/src/core/deletes/iceberg_equality_delete.cpp` (`ScanEqualityDeleteFile`) +
  `src/d/iceberg/src/planning/iceberg_multi_file_reader.cpp` (the `FinalizeChunk`-time
  `ExpressionExecutor` application) — both fully read and understood this session, just not
  portable without the missing field-id schema first.

### Where the wiring lives

- `src/include/uc_multi_file_list.hpp` / `.cpp`: `UCMultiFileList` now resolves each
  `UCScanPlanFileScanTask::delete_file_references` into concrete `UCScanDeleteFile` records at
  the moment the file is added to `expanded_files` (both in the constructor for inline tasks and
  in `ExpandNextPath` for lazily-fetched ones) — the indices are scoped to that response's own
  `delete_files` array, so they can't be resolved later once that array is gone. Stored in a
  parallel `file_deletes` vector.
- `UCMultiFileReader::FinalizeBind` (new override) looks up the current file's resolved deletes
  via `reader_data.reader->file_list_idx`, calls `BuildUCDeleteFilter`, and sets
  `reader.deletion_filter` — the same seam (`BaseFileReader::deletion_filter`) the `delta`
  extension uses, confirmed by reading `delta_multi_file_reader.cpp`'s own `FinalizeBind`.

## 6. Test coverage

`test/databricks/scan_plan_deletes.py` / `.test` + a new
`test/databricks/data/scan_plan_days_deletes.sql` fixture: same 5-file/50-row shape as
`scan_plan_days_managed`, plus `'delta.enableDeletionVectors' = 'true'` and a `DELETE FROM ...
WHERE id IN (5, 15, 25, 35, 45)` (one deleted row per file) appended to the data-gen SQL.
Asserts the post-delete row count (45), that the deleted ids are gone, and that filter pushdown
and delete application compose correctly (a `day = 'Mon'` filter still prunes to one file, and
still excludes the deleted id within it).

**Explicitly not a substitute for live verification** — flagged in both the test's own header
comment and here: whether Databricks' scan-plan endpoint actually represents a
deletion-vector-enabled `DELETE` as a `deletion-vector-v1` puffin blob (the shape
`BuildUCDeleteFilter` is built for) is exactly the open question this test is designed to
surface on a first live run, not something already confirmed. I could not set up or reach a
live Databricks/UC endpoint in this environment to run it.

## 7. Build verification — confirmed

**Everything here compiles and links clean**, confirmed with real (not stale/cached) rebuilds
at each step:

- After the DuckDB-core-drift fixes (§4), `ninja -C build/debug unity_catalog_extension`
  succeeded — covers `uc_table_entry.cpp`, `uc_irc_expression.cpp`, `uc_api.cpp`,
  `uc_multi_file_list.cpp` *before* the delete-files work landed.
- `vcpkg.json`'s new `roaring` dependency resolved and built via a full reconfigure
  (`make debug`, since the new `find_package(roaring CONFIG REQUIRED)` in the top-level
  `CMakeLists.txt` needed a real CMake reconfigure, not just an incremental `ninja` rebuild) —
  confirmed by `-I.../vcpkg_installed/x64-linux/include` appearing in the actual compile
  command for `uc_puffin.cpp`, which compiled with zero warnings.
- After fixing two missing includes surfaced by that build (`TableFunctionRef`,
  `ThreadContext`/`ExecutionContext` — needed by the new `ScanParquetFile` nested-bind helper
  in `uc_multi_file_list.cpp`) and a follow-up correctness refinement (§5 —
  `UCMultiFileList::GetDataFilePath` instead of `BaseFileReader::GetFileName()` for the
  delete-file cross-reference, so a match can't silently fail if DuckDB's parquet reader ever
  normalizes the path differently than the server's literal string), a final targeted rebuild
  (`ninja -C build/debug unity_catalog_extension`) succeeded with **zero warnings, zero
  errors** under this build's `-Wall -Wunused -Werror=vla -Wnarrowing -pedantic`.
- **Full loadable-extension link also confirmed**: `ninja -C build/debug
  unity_catalog_loadable_extension` produced `extension/unity_catalog/unity_catalog.duckdb_extension`
  cleanly, including the `roaring::roaring` link — this is the real, complete artifact, not
  just the static-lib compile step.

**What this does *not* cover:** the `unittest` binary target and the two SQLLogic
`.test`/`.py` pairs (`scan_plan.test`, `scan_plan_deletes.test`) were never run — no
Databricks/UC credentials in this sandbox, and the full `ninja -C build/debug` (`all`) target
(needed to build `test/unittest` itself) OOM'd on two *unrelated* large vendored extensions
(`parquet`, `core_functions`, `delta` — none touched by this session's changes) linking in this
sandbox (14GB RAM, no swap); confirmed it's a resource ceiling and not a parallelism artifact
by retrying at `-j2` and still hitting it on the same two extensions. So: **the C++ compiles
and the loadable extension links; the tests themselves are unverified against a real server**,
same caveat as everything else in this session.

## 8. What I'd want a human to double-check first

1. The `roaring` vcpkg dependency resolves and links **on this platform** (linux-x64, this
   sandbox) — confirmed (§7). Still worth confirming on whatever other platforms this repo's
   CI targets (macOS/Windows arm64 etc.) — a new external dependency for one C++ extension's
   build is exactly the kind of thing that can surface platform-specific vcpkg quirks I can't
   see from a single-platform local build.
2. A live run of `scan_plan_deletes.test` against a real Databricks/UC endpoint — both to
   confirm the puffin-blob assumption above, and because none of this delete-file code has run
   against a real server at all yet.
3. The equality-delete scope decision (§5) — if equality deletes turn out to matter for a real
   near-term use case, the field-id-schema gap is the thing to design next, not a quick patch.

## Subsequent work (later in the same session)

The doc above records the original `scan-plan-api--v1-main` work. Since then:

- **Rebased onto `upstream/main` and squashed** into a single clean feature commit on
  `scan-plan-api--v2-main`. Upstream had independently landed its own ducktest test-suite port
  (#101/#102), so the branch adopts upstream's test infra as SoT and carries only the scan-plan
  deliverable (src + roaring dep + scan_plan tests + docs); `uc_api.cpp` was 3-way merged to keep
  upstream's `type_precision` null-parse fix.
- **Open question #2 resolved — live-verified.** `scan_plan_deletes.test` passed against a real
  Databricks warehouse: a DV-enabled DELETE surfaced as a `deletion-vector-v1` puffin blob (against
  the **legacy `…/iceberg`** endpoint). Superseded by the `iceberg-rest` migration + the pre-apply
  finding below.
- **Async polling hardened** (own commit): poll-until-terminal with `Retry-After` + exponential
  backoff + a wall-clock budget, `InterruptCheck` for cancellation, and a best-effort plan
  `DELETE` on abort — replacing the old fixed 10s cap (borrowed shape from duckdb-iceberg #1204).
- **C3 fix** (own commit): the truncated-DV-blob over-read in `FromBlob` (min-size 12→20 + a
  pre-`Load` bound).
- **SQL tooling + offline test**: `uc_read_deletion_vector` (public) and
  `__internal_uc_plan_table_scan` (internal) table functions, plus
  `test/irc/test_irc_api_retry.py` — a Python mock-server test covering the poll/retry/cancel
  path offline. The `uc_read_deletion_vector` whole-file path gave `UCPuffinReader` its first
  caller (review-finding S1 resolved by use). See `scan-plan-design.md` §5–6.
- **IRC scan-plan gating + `iceberg-rest` migration** (see `scan-plan-gating.md`). Scan planning is
  now opt-in via a `USE_IRC_SCAN_PLAN` ATTACH boolean; the URL is derived
  (`…/api/2.1/unity-catalog/iceberg-rest`), retiring the deprecated `…/iceberg` endpoint Databricks
  now rejects. Per-ATTACH availability tri-state — the `/plan` call is the probe (Databricks serves
  no usable `/config`); AVAILABLE is sticky, UNAVAILABLE re-probes after 15 min. A `/plan` failure
  falls back to Delta **for the same query** — the pushdown points the scan wrapper's inner-scan
  delegate at the Delta scan (the seam parquet uses on success) — and marks the endpoint unavailable
  so later scans skip it until the re-probe. Regression-tested by `scan_plan.test`'s fallback section.
- **Finding — Databricks pre-applies catalog-managed deletes server-side.** Live-verified against
  `iceberg-rest`: a DV-enabled DELETE yields a scan-plan of only surviving-row files with **no**
  delete file, so `BuildUCDeleteFilter` / `ScanDeletionVectorFile` do not engage on this path. The
  delete-file code stays intact and unit-covered (`dv_decode`, `unittest_cpp` position-set) for any
  server that does return delete files; `scan_plan_deletes.test` validates the end result via the
  committed differential oracle rather than a delete-path log line.
