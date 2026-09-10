# Testing architecture review — `unity_catalog` (`test/`, `scripts/`)

Scope: `test/` and `scripts/` only (not `uc/src`, the C++ extension). Written as a
companion to the `duckdb-pytest-driver` architecture review — this repo is the driver's
first and most complete real consumer, so it's also the best concrete example of what
"integrating a provisioner" looks like end to end.

## Part 1 — How this repo plugs into the driver

### Layout

```
test/oss_local/      OSS Unity Catalog (docker) backend — runs in CI
test/databricks/     live Databricks workspace backend — skips gracefully without creds
test/fixtures/       neutral SQL table fixtures (id_name, simple_table), shared by both
test/databricks/data/  Databricks-only Delta-artifact defs (schema evolution, etc.)
test/py/uc/           the actual Provisioner implementations (this is the "integration")
  __init__.py          shared paths + uctl() wrapper
  identity.py           unified {CATALOG}/{SCHEMA}/{TABLE} env-injection contract
  oss.py                 OssProvisioner — the OSS docker backend
  server.py               the OSS docker container: start/stop/first-worker-wins lock
  databricks/
    engine.py             DatabricksProvisioner — the live-workspace backend
    config.py             account config (catalogs, S3 bucket)
scripts/databricks_gen/  atomic Databricks SQL ops (databricks-sdk), + `databricks-gen` CLI
scripts/oss_uc_image/    the local OSS UC docker image: build, run, uctl, smoke_test
```

`test/py/uc/oss.py::OssProvisioner` and `test/py/uc/databricks/engine.py::DatabricksProvisioner`
are both textbook implementations of the driver's `Provisioner` protocol
(`provision`/`make_init`/`teardown`) — reading either one, side by side with
`driver/provision.py`'s docstring, is the fastest way for a new integrator (Iceberg, say)
to see the seam actually used. `oss_local/conftest.py` and `databricks/conftest.py` are the
two `register_provisioner(...)` call sites.

### What's distinctive here vs. a from-scratch integration

- **Two backends sharing one identity contract.** `uc/identity.py`'s `TableRef`/`build_env`
  turns each provisioned requirement into `{KEY_CATALOG}`/`{KEY_SCHEMA}`/`{KEY_TABLE}`/`{KEY}`
  env vars (plus bare `{CATALOG}`/`{SCHEMA}`/`{TABLE}` for the primary one), so `.test`
  bodies read the same shape regardless of which backend provisioned them. Marked
  UC-local-for-now, explicitly slated to migrate into the driver's `Provisioner` base
  (`test/py/uc/WIP-identity-design.md`) once a second backend (Iceberg) validates it's
  actually generic.
- **Table fixtures + non-fixture defs, side by side.** `Fixture(...)` (portable, driver-owned)
  covers `id_name`/`simple_table`; `test/databricks/data/*.sql` (Delta-artifact defs run
  verbatim via `run_sql_file`) covers cases too Databricks-specific to be portable fixtures
  — schema evolution, column mapping. `DatabricksProvisioner._instantiate` dispatches on
  which kind a `@requires` source is, cleanly, in one place.
- **A first-worker-wins docker service, hand-rolled** (`test/py/uc/server.py`): an `O_EXCL`
  lock file + a JSON state file, keyed by the driver's run-id, with a `reclaim_stale()`
  cleanup at controller `pytest_sessionstart`. This *predates* the driver's now-built
  `tiers.py`/`store.py` (`register_tier(..., services=[service(...)])` +
  `provision_service()`, which implements exactly this pattern generically — single-flight
  via `multiprocessing.managers`, controller-owned teardown, fully tested). See Part 2.
- **Credentials via the older broadcast seam**, not tiers. `databricks/conftest.py` uses
  `register_broadcast`/`get_broadcast` (`load_creds` fetched once on the controller,
  broadcast to workers) — this is exactly the mechanism `docs/TIERING.md` in the driver
  repo describes superseding with `register_tier(..., credentials=[credential(...)])`.
  `WIP-identity-design.md` marks this "Credentials — DONE (part b)" as of when it was
  written, i.e. before the driver's tiering phases landed.

## Part 2 — Findings

### The big one: this repo is TIERING.md's reference case, and the driver side is now built

`docs/TIERING.md` (in the driver repo) opens with UC's exact hand-rolled conftest pattern
as the problem statement, and its "before → after" section shows `test/databricks/conftest.py`
+ `test/oss_local/conftest.py`'s ~140 combined lines collapsing to ~18 via two
`register_tier(...)` calls. Reading the driver's `plugin.py` end to end (see the driver-side
`review.md`) confirms **Phases 0–4 of that design are fully implemented and tested** in the
driver — auto-marking by path, default-scan deselection + banner, eager pre-fork credential
fetch via a shared store, lazy first-need service provisioning, and the fail-loud backstop.

This repo hasn't adopted it yet. Concretely, today:

- `test/oss_local/conftest.py` hand-rolls: `pytest_collection_modifyitems` to mark every
  item `oss_local` (⇔ `register_tier(..., marker="oss_local")`'s auto-marker), a
  `pytest_sessionstart` calling `reclaim_stale()` (⇔ the driver's own leaked-service
  reclaim, not yet generalized but the store's single-flight already replaces the need for
  file-lock reclaim), and `pytest_sessionfinish` calling `teardown_shared()` (⇔
  `service(..., stop=teardown_shared)` + the driver's controller-owned teardown).
- `test/py/uc/server.py`'s `_StartLock` (`O_EXCL` + a JSON state file at a fixed tempdir
  path) reimplements exactly what `store.copy_or_provision` already does generically
  (single-flight owner election, waiters block, poison-pill on failure) — tested end-to-end
  under real xdist subprocess workers in the driver's `tests/test_services_store.py`.
- `test/databricks/conftest.py` hand-rolls the default-selection deselect
  (`pytest_collection_modifyitems` + `_no_selection`) and the up-front creds fetch +
  fail-loud (`pytest_configure` + `register_broadcast`) that `register_tier(...,
  credentials=[credential(...)])` now does in one declarative call, including the
  `-k`-selected-live-test backstop (`pytest_runtest_setup`) UC also hand-rolls here.

None of this is broken — it works, and `WIP-identity-design.md` shows it was built as a
deliberate, sequenced step (credentials via broadcast landed first; the tiers/store-based
service migration was always the anticipated next step, gated on the driver side landing
first). But the driver side *has* landed since that doc was written, and this repo is
exactly the validation case it was designed against. **Recommendation:** migrate
`oss_local/conftest.py` + `databricks/conftest.py` to two `register_tier(...)` calls per
`TIERING.md`'s own before/after sketch. This would delete the hand-rolled marker/deselect/
backstop code in both conftests and `server.py`'s `_StartLock`/state-file machinery,
replacing it with `service(key="oss-uc-server", start=..., stop=..., fixture="uc_server")` +
`credential(key="databricks_creds", fetch=load_creds, ...)`.

**Not attempted here.** This is a real behavioral migration of live CI/docker/credential
coordination code, and this sandbox has no docker and no Databricks credentials to verify
it end to end — exactly the kind of change that needs a real run before landing. Flagging
it as the highest-value next step rather than guessing at it blind.

### Confirmed gap: no `pytest.ini` / `duck-test configure` at the repo root

Grepped the whole repo (working tree + git history): there is no `pytest.ini` anywhere,
`duck-test configure` is not called from the `Makefile`, `scripts/env_databricks`, or
any `.github/workflows/*.yml`, and there is no root `conftest.py` either — only the two
subtree ones. `Makefile`'s `test_release_internal` target runs a bare
`${PYTHON_BIN} -m pytest test`.

This matters concretely because the driver's own docs are explicit that
`--import-mode=importlib` (which only `duck-test configure`'s `pytest.ini` sets — a plugin
cannot inject it) **is required for same-stem sibling drivers** — and this repo has exactly
that shape today: `test/oss_local/table-cmt/catalog_managed.py` vs.
`test/oss_local/table-plain/catalog_managed.py`, and the same pairing for `checkpoint.py`,
neither directory containing an `__init__.py`. Under pytest's default `prepend` import
mode this is the textbook "import file mismatch" collection error the moment both are
collected in one run (`pytest test`, or CI's `test_release_internal`) — well-documented,
standard pytest behavior, not something specific to this driver.

I could not reproduce this live in this sandbox: `oss_local/conftest.py`'s
`pytest_sessionstart` unconditionally shells out to `docker inspect` (via `reclaim_stale`),
which isn't available here, so collection never gets far enough to hit the import-mode
question in an end-to-end run. The finding rests on (a) the confirmed absence of any
`pytest.ini`/`duck-test configure` anywhere in the repo or its history, (b) the confirmed
presence of same-stem sibling driver files with no `__init__.py`, and (c) the driver's own
README stating this exact scenario (`table-cmt/read.py` vs `table-plain/read.py`, its own
example) requires `--import-mode=importlib`. High confidence, not independently reproduced
here. **Recommendation:** run `duck-test configure .` once at the repo root and commit the
resulting `pytest.ini` (also picks up `-n auto --dist=loadgroup` and `python_files=`, both
of which this repo is currently also running without).

### Fixed directly (small, isolated, verified)

- **`scripts/databricks_gen/sql.py`: `split_statements` / `sql_literal` / `build_insert`
  were byte-for-byte duplicates** of `duckdb_pytest_driver.sqldef`'s versions of the same
  three functions (confirmed via diff). `duckdb_pytest_driver` is already an explicit
  runtime dependency of this module (its own CLI already does
  `from duckdb_pytest_driver.fixtures import ...`; the entry-point shim's docstring lists
  it as a dep). Replaced the three local definitions with an import from
  `duckdb_pytest_driver.sqldef`; kept the Databricks-specific `run_sql_file` (different
  substitution-key contract) local. Verified: `databricks_gen.sql`'s public functions
  (`split_statements`, `sql_literal`, `build_insert`, `build_create_table`) still resolve
  and behave identically; `ruff check` clean.

- **`scripts/oss_uc_image/run`: stale default image tag.** Defaulted `FINAL_IMAGE` to
  `duckdb/unitycatalog:local`, the pre-rename Docker Hub namespace. `build_image` (the
  script that actually produces the local alias tag) and `smoke_test` (which already gets
  this right) both agree the current alias is `ghcr.io/benfleis/unitycatalog-ducklabs:local`
  — confirmed via `build_image`'s own header comment (`alias:
  ghcr.io/benfleis/unitycatalog-ducklabs:local (local run/uctl/smoke)`) and its
  `IMAGE_NS`/`IMAGE_NAME` defaults. `server.py` (the Python test harness) always passes
  `FINAL_IMAGE` explicitly, so this never bit the automated test path — but `run`'s own
  usage comment advertises direct manual use (`./run`, `./uctl create ...`), and a manual
  invocation with no override would have tried to run a nonexistent local image. Fixed the
  default (and its usage-comment) to match.

- **`scripts/env_databricks`: dead `UC_TEST_SCHEMA` export.** Set
  `UC_TEST_SCHEMA="test_schema_$schema_rand"`, but nothing in `test/py/uc/databricks/`
  reads `UC_TEST_SCHEMA` anymore — grepped `engine.py`/`config.py`, no hits.
  `WIP-identity-design.md`'s implementation checklist confirms the Databricks provisioner
  was fully ported off the legacy `UC_TEST_*` contract to its own computed per-test cell
  schema; `UC_TEST_CATALOG` was deliberately kept (still read via
  `os.environ["UC_TEST_CATALOG"]` in `engine.py`), but `UC_TEST_SCHEMA` was not — this
  export is a leftover from before that port. Removed the export and its now-unused
  `schema_rand` computation. Also dropped a dangling `# NOTE: _env below gets the 3 vars
  above directly` comment at the top of the file — there are no "3 vars above" in the
  current file (an edit-history orphan), so it only confuses a reader.

### Noted, not changed

- **`scripts/oss_uc_image/README.md` is significantly stale**, beyond the one line fixed
  in `run`: it documents a `./build` script (the actual script is `build_image`; `build`
  doesn't exist in the tree) and a `run.sh` (actual name: `run`), and its "Image naming"
  section is entirely in the old `duckdb/unitycatalog*` namespace/tag scheme (6+
  references) that `build_image`'s comments and `smoke_test`'s default show has moved to
  `ghcr.io/benfleis/unitycatalog-ducklabs*`. This looks like the README predates a rename
  that touched both the image registry and at least one script name. Didn't attempt a full
  rewrite — I can't run `build_image`/`docker build` here to confirm the current two-step
  flow still matches the "Quick start" section's shape, and a guessed rewrite risks making
  a stale doc confidently wrong instead of vaguely wrong. Recommend a pass by whoever last
  touched the rename.

- **`scripts/oss_uc_image/Dockerfile`: `ARG BASE=duckdb/unitycatalog-base:local`** — same
  stale namespace, but `build_image` always passes `--build-arg "BASE=$BASE_ARCH_IMAGE"`
  explicitly, so this default is a fallback-only path (same shape as the `run` issue, but
  lower-traffic: a bare `docker build .` bypassing `build_image` is a less-documented way
  to use this kit than `run`'s advertised manual `./run`/`./uctl` flow). Left as part of
  the same README-adjacent sweep above rather than a one-off fix.

- **`scripts/oss_uc_image/exec_spark_sql.py` is dead and has two real bugs if it were ever
  run**: `.config("spark.jars.packages",)` (a config call missing its value argument — a
  `TypeError` at import time) and a broken `-f`/`--file` arg loop where `is_file = False`
  is set unconditionally in the same branch that reads `is_file`, so a file path is never
  actually read (the raw path string gets treated as a literal SQL statement instead).
  Grepped the whole repo (`.py`/`.sh`/`Makefile`/`.md`) — nothing invokes this script; it
  looks like a one-off scratch tool from before `databricks_gen` existed, pointed at a demo
  Unity Catalog server (`uc.openlakehousedemos.dev`), not this repo's own test
  infrastructure. Left alone rather than fixing dead code — flagging so someone can decide
  delete-vs-keep-and-fix; fixing bugs no path exercises isn't a real improvement on its own.

### What I did *not* find

- The `.py` driver files (`test/oss_local/**/*.py`, `test/databricks/**/*.py`) are
  uniformly terse, information-dense docstrings — no verbose/historical-narration comments
  to sweep, unlike some spots in the driver's own `plugin.py`. Consistent style throughout;
  no cleanup needed there.
- `identity.py`, `oss.py`, `engine.py`, `config.py` are each internally consistent with
  their own docstrings and with `WIP-identity-design.md`'s decisions; no other doc/code
  mismatches found beyond the tiering-migration gap above.
