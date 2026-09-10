# Testing this extension

Tests are **pytest-driven** via the `duckdb-pytest-driver` (ducktest). Most tests are a `.test`
(SQLLogic) file run through the duckdb `unittest` binary with a same-stem Python `.py` driver
alongside for declarative `@requires` provisioning (tables, catalog setup) and managed teardown —
the `.test` file stays the central artifact, the Python is thin glue. Where a flow doesn't fit
SQLLogic (e.g. concurrency), a **pure-Python** `test_*.py` is a first-class test too.

## Running

First run builds the extension; after that pytest drives the suites. **No credentials and no
environment variables are needed for the default (OSS) path** — the OSS Unity Catalog container is
started and torn down for you.

```bash
make venv            # one-time: create the uv venv (uv sync)
make test            # build + run the OSS-local suite (boots the UC docker container)
make test_databricks # the live Databricks suite (creds auto-fetched via 1Password; see below)
make test_all        # both
```

Once built, drive pytest directly in the venv (selection is by suite marker):

```bash
uv run pytest                     # the default (oss_local) suite
uv run pytest -m oss_local        # just the OSS subtree
uv run pytest -m databricks       # just the Databricks subtree (needs creds)
uv run pytest -n0 -s <path>       # serial + live output — debugging one test / the container
uv run pytest --repl <a_test>     # provision the test's @requires, drop into an attached duckdb
```

The OSS suite needs the container image available. `make test` defaults to the locally-built
`:local` tag; set `DUCKTEST_UC_IMAGE=ghcr.io/benfleis/ducktest-unitycatalog:ci` to use the published
one. Building/publishing the image: [`scripts/oss_uc_image/`](../scripts/oss_uc_image/README.md).

## Layout

- **`oss_local/`** — run against a **local OSS Unity Catalog** server (the ducktest docker image,
  started per session by the `uc_server` fixture). Runs in CI. `-m oss_local` selects the subtree.
  Holds both `.test`+`.py` pairs and pure-Python `test_*.py` (e.g. `test_concurrent_rw.py`).
- **`databricks/`** — run against a **live Databricks workspace** (creds via `make test_databricks` /
  `scripts/env_databricks` → `DATABRICKS_*`; auto-fetched from 1Password if not already in env). Not
  in CI; skips cleanly without creds. Read tables auto-provision from the Delta-artifact defs in
  `databricks/data/`; write tests seed the `id_name` table into an isolated cell and tear it down.
- **`fixtures/`** — neutral, backend-agnostic table definitions (`id_name`, `simple_table`),
  instantiated per backend via the driver's `TableSpec` path (`load_table_spec`). Shared by OSS and
  Databricks.
- **`databricks/data/`** — Databricks-specific Delta-artifact definitions (schema evolution, column
  mapping, catalog-managed staged/backfilled commits) that aren't portable fixtures; run verbatim
  via `databricks-gen from-sql`.

## Coverage today

Local OSS (`oss_local/`):

- delta.yaml v1 read/write on a catalog-managed (CMT) table: `LoadTable` / `UpdateTable`
  (`table-cmt/catalog_managed.test`).
- Managed vs non-managed dispatch: `IsCatalogManaged()` selects the v1 path; a MANAGED table without
  the `catalogManaged` feature and EXTERNAL tables take the plain Delta path and do **not** call
  `LoadTable`/`UpdateTable` (asserted via `duckdb_logs()` counts — `table-plain/`).
- Backfill watermark advance across DETACH/ATTACH (`set-latest-backfilled-version`).
- **Concurrent readers/writers** (pure-Python, matrixed managed × plain):
  `test_concurrent_rw.py` runs N racing writers (`INSERT … max(id)+1`, retrying on version
  conflict) + M readers, asserting the gapless commit invariant `max(id) == count(*) == version` at
  every snapshot.

Databricks (live):

- CMT delta read, column-mapped read, time-travel, attach, and a TPC-H read pass (`tpch.test` —
  hand-written scans / pushdown / join vs the premade sf0.01 tables); write + write-CMT under
  `write_tests/`.

## Testing gaps / TODO

These exercise code paths that the **file://-backed local OSS server can't reach today**. The shared
unblock is the same: stand up **OSS + S3** (e.g. MinIO) so storage is real object storage, bring a
table to a specific state, then apply a **coordinated out-of-band modification** to force the
condition. Most depend on the **pytest infra** being built up (cf. `lakekeeper/tests/python`) to
orchestrate server state + a side-channel writer between SQL steps. Each below needs pattern work.

- [ ] **Storage-credential vending (managed + external).** `RefreshCredentials`
      (`src/storage/uc_table_set.cpp`) early-returns on `file://`, so neither credential path runs in
      CI: managed via delta/v1 `/credentials` → `storage-credentials[].config` S3 keys, nor external
      via `temporary-table-credentials` → `aws_temp_credentials`. Only Databricks covers them today.
      **Unblock:** OSS + S3 backing. Also lets us cover the *longest-prefix* selection that's still a
      TODO in `GetTableCredentials` (currently takes the first `storage-credentials` entry).

- [ ] **`latest-table-version` omitted → max-commit fallback + incoherent warning.** The fallback and
      `UC_LOG_WARNING` in `UCAPI::LoadTable` (`src/uc_api.cpp`) are unexercised. The `D_ASSERT` there
      intentionally crashes the suite on a contract-violating server (latest-version present but ≠ max
      commit), so the goal is only to exercise the *absent-field* path. **Unblock:** a coordinated
      response shape (mock/proxy, or a server state) that returns non-empty `commits` with no
      `latest-table-version`. Assert read correctness + the warning line via `duckdb_logs()`.

- [ ] **etag optimistic-concurrency conflict (`assert-etag`).** No test triggers a
      `CommitVersionConflictException` via the intended path. **Partly probed** by
      `oss_local/test_concurrent_rw.py` (racing in-process `UpdateTable`s, retry-on-conflict). Still
      open: the deliberate out-of-band stale-etag case — bring a CMT table to a known etag, commit
      out-of-band (second writer / API call) so the next `UpdateTable` sends a stale etag; assert the
      conflict surfaces. Also covers the `CommitState` snapshot under genuine read/write interleave.

- [ ] **Externally-written (Spark) staged commits + `max_catalog_version`.** DuckDB's own writes go
      through the backfill path, so the read-via-`max_catalog_version` path for commits that live
      permanently in `_staged_commits/` is never hit locally. **Unblock:** a non-DuckDB writer (Spark,
      or a script that stages a commit without promoting it to `_delta_log/`), then read and confirm
      visibility depends on `max_catalog_version`. This is also the fixture that would actually
      exercise the `latest-table-version` fallback meaningfully (no `_delta_log/` promotion to mask a
      wrong gate).

- [ ] **Backfill resume after an interrupted write.** Leave a commit acknowledged by UC (`UpdateTable`
      succeeded) but un-promoted to `_delta_log/`, then reattach and confirm `BackfillCommits` resumes
      it. **Unblock:** either a coordinated `exit`/kill between `UpdateTable` and the next attach, or
      directly stage the state. Note: for a DuckDB-written commit the next attach backfills it anyway,
      so pair with the Spark fixture above to make the gate observable.

- [ ] **Backfill copy failure.** `BackfillCommits` (`src/storage/uc_table_set.cpp`) stops + warns on a
      failed copy to avoid advancing the watermark past a gap; no test induces a failure. **Unblock:**
      make a `_delta_log/` destination unwritable (perms / read-only mount under S3) and assert the
      warning + that the watermark did not advance.

- [ ] **Concurrency / data races (TSan).** `commit_state` is now `MutexProtected<CommitState>`
      (`src/include/uc_mutex_protected.hpp`) so the etag/backfill pair can't be torn or read unlocked.
      `oss_local/test_concurrent_rw.py` now drives a concurrent reader/writer workload (functional,
      not TSan) against one shared attach; still open is the **TSan build** and the two `TODO(race)`
      unlocked `internal_attached_database` derefs (`GetInternalCatalog` / `InternalCheckpoint`).
      **Target design** (the lock pass, not piecemeal): widen the `MutexProtected` pattern to also
      cover `is_dirty` and `internal_attached_database` — fold all three into one
      `MutexProtected<AttachState>` and drop the bare `attach_lock`, making unlocked access
      unrepresentable everywhere. Must stay a single protected struct to keep `InternalAttach`'s
      critical section atomic. See the `TODO(locks)` note on `attach_lock` in
      `src/include/storage/uc_table_set.hpp`.
