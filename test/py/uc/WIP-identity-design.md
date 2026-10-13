# WIP — Unified test-identity model (UC → driver, iceberg-ready)

> Working log of a design conversation. **Not final docs** — to be synthesized into
> notes/docs/demos later. Captures decisions so work survives context compaction.
> Status markers: ✅ decided · 🔨 implementing (A) · ⏭ later · ❓ open.

## The problem

DB and OSS provisioners isolate per-test on **opposite axes** and derive `CAT.SCHEMA.TABLE`
differently, so "what is the test identity" has no single answer. `cmt`/`plain` even mean
different things per backend (commit-axis on DB; storage→schema on OSS). Bodies hardcode
`{UC_TEST_CATALOG}.{UC_TEST_SCHEMA}.id_name` — verbose and backend-divergent.

## Catalogs + storage ✅

**Clean-slate catalogs** (NOT a rename of existing): the new testing construct starts in fresh
`duckdb_tests_ro` / `duckdb_tests_rw`, created by account admin **with managed storage** (UC needs
a `MANAGED LOCATION` unless the metastore has a default). Cloning existing infra — the storage
credential over the bucket already exists — so `CREATE CATALOG … MANAGED LOCATION 's3://…'` reusing
the current credential, new subpaths.

**One bucket to rule them all**, single prefix scheme (mirrors the OSS UC proto layout):
```
s3://<bucket>/<prefix>/external/<cat>/<schema>/<table>   # external tables (_s3_location)
s3://<bucket>/<prefix>/managed/                          # catalog MANAGED LOCATION (UC adds __unity_catalog/… under it)
```
Managed-catalog root and external-table root share one bucket+prefix → `teardown_stale` sweeps both
from one place. `config`: `S3_BUCKET` (+ `S3_PREFIX`); `_s3_location` moves under `…/external/`;
catalog `MANAGED LOCATION` (account-set) is `…/managed/`.

## The unified model ✅

One physical model. **Expensive backends (DB, iceberg) are canonical; cheap OSS conforms.**

- **CATALOG** = attach / ACL boundary (read vs write; varies by backend). New names:
  `duckdb_tests_ro` / `duckdb_tests_rw` (replaces `duckdb_testing`/`duckdb_write_testing`).
  OSS hosts both catalogs in-container (cheap) so `{CATALOG}` is symmetric with DB.
- **SCHEMA** = the per-test **isolation cell** — carries the uniqueness token. **All backends
  isolate on the schema.** OSS switches from table-uniquification (`id_name_rw_<token>` in a
  shared schema) to a per-test cell schema with a bare table inside → matches DB byte-for-byte.
- **TABLE** = the **stable, bare** fixture name (`id_name`). No per-test table variance →
  bodies reference the table literally.

Same `@requires(Fixture("id_name"), access="rw", properties={commit:cmt,storage:managed})`:

| | CATALOG | SCHEMA (cell) | TABLE |
|---|---|---|---|
| DB  | `duckdb_tests_rw` | `cmt__managed__20260709_brave_otter_a1b2c3` | `id_name` |
| OSS | `duckdb_tests_rw` | `cmt__managed__20260709_brave_otter_a1b2c3` | `id_name` |

Only CATALOG can differ across backends; SCHEMA carries uniqueness; TABLE is constant.

## Injected env vars + body contract ✅

Short names, not `UC_TEST_*`. Per `TableBinding`, keyed by the `@requires` **key** (its `name=`,
default = bare table name, auto-suffixed on collision):

- `{<KEY>_CATALOG}` `{<KEY>_SCHEMA}` `{<KEY>_TABLE}` and `{<KEY>}` (= full FQN).
- The **primary** (sole/first) binding also gets bare `{CATALOG}` `{SCHEMA}` `{TABLE}` `{ID_NAME}`.
- Env keys use `_` (env vars can't hold `.`).

Single-table body:
```sql
require-env CATALOG

statement ok
ATTACH '{CATALOG}' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA '{SCHEMA}');
statement ok
USE unity.{SCHEMA};
query II
SELECT id, name FROM id_name ORDER BY id;   -- bare, stable
----
...
```
`ATTACH … AS unity` → everything below the attach uses the constant local `unity`; `{CATALOG}`
(remote name) appears only in ATTACH. `{ID_NAME}` = `{CATALOG}.{SCHEMA}.id_name` is the FQN
escape hatch when USE can't cover it.

Multi-table join (custom vars per layer):
```
@requires(source="...id_name", access="ro", name="src")
@requires(source=Fixture("id_name"), access="rw", name="dst")
```
```sql
SELECT * FROM {SRC_CATALOG}.{SRC_SCHEMA}.id_name s
JOIN   {DST_CATALOG}.{DST_SCHEMA}.id_name d USING (id);   -- or FROM {SRC} s JOIN {DST} d
```

## RW vs RO addressing ✅ (+ presence policy ⏭)

- **RW** → token'd cell: `duckdb_tests_rw.<commit>__<storage>__<date_token>.id_name`. Isolated;
  torn down; date-stamped so stragglers are age-sweepable.
- **RO** → **deterministic, token-free** address: `duckdb_tests_ro.<commit>__<storage>.id_name`
  (or a premade curated FQN like `...main.id_name__cmt__spark`). Same fixture+props → same place →
  **reusable**. Not date-stamped (meant to persist), never swept.
- `access` decides token-vs-stable. The determinism *is* the reuse signal.
- **Presence policy** ⏭ (Part 2): `assume` (bind, no DDL — default for premade/versioned RO) ·
  `validate` (cheap exists/version check, error if absent, no create) · `provision` (build/refresh).
  Gate wraps the `_instantiate` call; generalizes the current session `_shared_ro` memo. By-fact
  exists check: DB `SHOW TABLES … LIKE` / `DESCRIBE`; OSS `uctl get` (rc==0).

## Date-stamped token ✅

Provision token today = `mnemonic_sha1[:6]` (no date). Add sortable date →
`20260709_brave_otter_a1b2c3` so every RW cell encodes its birth time. Run *temp dirs* already
stamp `2026-06-23T08-48-12Z--brave-otter`; extend the scheme to provisioned schemas.

## Env translation (slice 0) 🔨

External `DATABRICKS_UC_*` (env-script overrides) → internal short vars injected on both backends.
`env_databricks` catalog exports become commented `# export DATABRICKS_UC_*` override
templates (pytest supplies defaults via config.py). Internal contract = the short `CATALOG`/
`SCHEMA`/`TABLE` + aliases.

## Other unifications 🔨

- `READ_ONLY` on ATTACH driven by `access` (ro) — on **both** backends (today DB-only).
- OSS endpoint from env (like DB's `${DATABRICKS_ENDPOINT}`), not hardcoded `127.0.0.1:8080`.
- Collapse OSS's two catalog-injection points (provisioner `setdefault` + autouse conftest
  monkeypatch) into one (`bindings.env`).
- De-overload `cmt`/`plain`: keep `properties={commit,storage}` abstract; document each backend's
  property→physical mapping.

## Service lifecycle: invocation-scoped, shared across workers ✅ (OSS done)

**Mental vs execution model:** pytest "session" = one pytest *process* → under xdist, **per-worker**,
NOT per-invocation. So a `scope="session"` service = one-per-worker; the old `xdist_group` pinned
all OSS tests to one worker as a workaround (→ serialized OSS). Correct model: **one service per
invocation, shared by all workers, isolation logical (cell/schema/path)** — exactly how Databricks
already behaves.

**xdist comms** (what's possible when): controller `pytest_configure` (pre-fork, guard
`not hasattr(config,"workerinput")`) → broadcast via `pytest_configure_node` → `node.workerinput`
(top-down, picklable, at worker init); during run, workers only report *back* (no controller→worker
push, no worker↔worker); filesystem+lock is the sideways escape hatch. Driver already broadcasts the
run-id this way (`_run_id`/`workerinput["sqllogic_run_id"]`).

**Two provisioning styles:**
- **Top-down** (controller `configure` + broadcast): serial/upfront, deterministic, secrets stay in
  memory. Best for **credentials** (fast, always-needed, secret).
- **First-worker-wins** (lock + shared state): the first worker to request a service boots it (behind
  a lock); dependents block on the fixture until up; **different services boot in parallel**, lazily,
  overlapped with tests. Best for **services** (heavy, conditional, parallel).

**OSS service — DONE (first-worker-wins):** `server.py` — one shared container per invocation,
booted under a no-dep `O_EXCL` lock (`_StartLock`), state at fixed host paths keyed by container
name, tagged with the invocation id (run-id) so a stale prior container is replaced (ALWAYS_CREATE).
Fixture no longer stops the container; **controller `pytest_sessionfinish` tears it down once**
(outlives all workers). **`xdist_group` dropped** → OSS tests distribute across workers. Needs a
live `-n auto` run to confirm parallel distribution + single container.

**Two classes of global (invocation-level) resource — same backbone, different scheduling:**

| | Class 1: credentials | Class 2: services |
|---|---|---|
| When | controller, pre-fork, upfront | first worker that needs it, scheduled |
| Mechanism | top-down `workerinput` broadcast | filesystem lock + shared state |
| Why | secret (off-disk), fast, always-needed, deterministic | heavy, conditional, parallel boot |
| Backbone | *compute-once → make-available-to-all-workers* (one primitive, two policies) |

**Credentials — DONE (part b).** Driver: `register_broadcast(config,key,factory)` /
`get_broadcast(config,key,default)` (plugin.py) — factory computes ONCE on the controller (cached),
`pytest_configure_node` broadcasts every registered key via `workerinput`; run-id is consumer #1,
creds #2. Exported from `driver`; offline self-test `tests/test_broadcast.py` (27 pass). UC:
`engine.load_creds` (env-wins **per var**, `op` fills only gaps — a partial override like a personal
TOKEN survives; complete env → no op; op-unavailable partial → skip) + `_op_fetch`; `test/databricks/conftest.py`
`register_broadcast(..., load_creds)` + `os.environ.update(get_broadcast(...))`. Stale PYTEST.md refs
removed. **Verify live:** `env_databricks pytest test/databricks` (env path, no `op`) AND bare
`pytest test/databricks` (controller `op`-fetch once → broadcast). Two-repo change (driver + uc).
Design that got us here:
- Supersedes the prior decision (front-load via `env_databricks`, "never fetch in-process — wrong
  under xdist"). That was right about *per-worker* `op`; the fix is **controller-fetch-once +
  broadcast** — the in-process pattern the old decision lacked. (Stale `PYTEST.md` ref in
  `test/databricks/conftest.py` + `engine.py` to remove.)
- Flow: controller `pytest_configure` (guard `not hasattr(config,"workerinput")`) loads creds once —
  **prefer env** (wrapper/CI already set them → no `op`), else run `op read op://testing-rw/
  databricks_ccv2/_env | op inject` once + parse. Broadcast via `pytest_configure_node` →
  `workerinput["databricks_creds"]`. Workers `os.environ.update(...)`. Downstream unchanged
  (`ensure_env`/`_require_creds`/body `{DATABRICKS_TOKEN}` read env); missing/failed `op` → skip.
  Cred vars: `DATABRICKS_{TOKEN,ENDPOINT,REGION,WAREHOUSE_ID}`.
- **WHERE = the driver.** A subtree conftest's `pytest_configure_node` fires for `pytest
  test/databricks` (initial conftest) but NOT `pytest test` (loaded lazily, after workers spawn).
  The driver plugin's `configure_node` **always** fires (that's why run-id broadcast lives there). So
  build a **generic broadcast seam in the driver** ("controller computes once → broadcast to
  workers"): run-id is consumer #1, creds #2 (justifies generalizing). UC supplies only the op-fetch.
  Makes bare `pytest test/databricks` wrapper-free; `env_databricks` op-exports become optional
  overrides (ties to step 6 env translation).

This is the concrete form of the `Backend`/`Service` layer in step B: **invocation-scoped + shared**;
minio/azurite/OSS-UC are the same shape (ambient, addressable, logically partitioned), and their
creds ride the same driver broadcast seam.

## B — driver Provisioner base (the "demo") ⏭

Two abstractions, both into the driver:
- **`Backend`/`Service`** (session-scoped): `start()`/`stop()`, exposes `endpoint` + `execute`
  transport. Docker image, Spark session, local REST bring-up. (Today `uc_server` conflates this.)
- **`Provisioner`** (per-test): identity model + lifecycle + RO presence-policy + `teardown_stale`,
  all generic. `execute` delegates to the Backend transport.

Backend implements ~5 primitives: `execute`, `instantiate_fixture`, `table_exists`, `attach_sql`,
`catalog_for(access)`. Iceberg = first consumer (Backend: local REST+Spark; Provisioner: namespace/
table). UC `engine.py`/`oss.py` become thin subclasses. Subsumes the earlier "slide generic code down."

Naming (nits): `provision`↔`teardown`; **`teardown(bindings)`** (drop redundant `token` arg — it's in
bindings); **`make_init` → `make_init_sql`**; **`sweep_stale` → `teardown_stale(older_than)`**.

Image/docker provisioning = **Backend layer**, orthogonal to table `Provisioner` — but real work to
formalize the split (do it as part of B since iceberg needs the service layer).

## D — requires_matrix ✅ (verify)

Should "just work": each matrix cell = distinct properties → distinct cell schema → distinct
`{SCHEMA}`/`{ID_NAME}` injected per parametrized run. **Verify** per-cell env injection re-runs
`provision` per parametrization. **Open** ❓: a single body needing several cells live at once
(holes/partial matrices) needs multi-spec-per-invocation — explicit later decision, not assumed.

## Parallelism finding ✅

Batching = one binary invocation, one env → per-test RW isolation (unique `{SCHEMA}`) is unbatchable.
But: (1) it's isolation, not `id_name` reuse, that causes it; (2) provisioned/paired tests **don't
batch today anyway** (`run_paired` = one invoke each) → no regression; (3) xdist process-parallelism
still applies; (4) batch amortizes only binary startup (ms) — noise vs DB/iceberg provisioning
(seconds) — so losing it for RW costs ~nothing; (5) RO shared-fixture tests share env → **batchable**,
exactly where startup could matter. Future lever ⏭: group same-resolved-env paired RO tests into a
batched invocation.

## A — implementation checklist 🔨

1. ✅ Provisioner emits unified `bindings.env` (short vars + per-key `{KEY_*}` + `{KEY}`/`{ID_NAME}`
   aliases). engine.py + oss.py. *No core dep.* DONE additively: `uc/identity.py` (`TableRef`,
   `build_env`), wired into both provisioners **alongside** legacy `UC_TEST_*` (transition — drop
   the legacy keys after step 5 migrates bodies). Pure `build_env` verified offline.
2. ⏭ OSS → schema-isolation — **deferred but REQUIRED** (not optional). FIRM PRINCIPLE: **table
   names are never vars** — bodies write bare `id_name`, never `{TABLE}`/`{ID_NAME}` in its place.
   So OSS must gain per-test cell schemas so bare `id_name` resolves; `{ID_NAME}`-as-default is
   rejected. Blocker: `uctl` hardcoded to `duck.cmt.*`/`duck.plain.*` (no dynamic schema/catalog
   creation) → needs `uctl`+entrypoint work. Until done, OSS bodies stay on the legacy form; DB
   (already schema-isolated) migrates first. `{ID_NAME}`/`{KEY}` remain ONLY the FQN escape hatch
   for qualification/joins, never the primary table reference.
   - ❓ OPEN: bare-table **joins of the same fixture** (`{CAT1}.{SCHEMA1}.id_name JOIN
     {CAT2}.{SCHEMA2}.id_name`) need the alias **key** separated from the **physical name** on
     `@requires` — today `name=` conflates them (`resolved_name()` is both). Likely a small driver
     field (`as=`/`key=`): physical = fixture/source name (stays `id_name`), key = the namespace.
3. ✅ Date-stamp the provision token — DONE in driver `_provision_token`: `<YYYYMMDD>_<mnem>_<hash>`.
   ruff clean, 22/22 self-tests pass.
4. Catalog rename `duckdb_tests_ro`/`_rw` + `ATTACH … AS unity`. **DB rename needs the Databricks
   account to have those catalogs** (else live tests break) — code+account must land together. OSS
   catalog stays `duck` (abstracted by `{CATALOG}` + `AS unity`). ❓ rename now or hold?
5. ✅ (DB) Rewrite `.test` bodies to the short contract (`ATTACH '{CATALOG}' AS unity` + `USE unity`
   + short vars + bare tables). All 6 DB bodies ported: write, write_catalog_managed,
   read_column_mapped, time_travel, attach (FQ reads via `unity.{SCHEMA}.id_name`, `DETACH unity`),
   catalog_managed_delta_read (two-table, bare). tpch.test_slow non-provisioned — left. Live run:
   **all passing.** `make_init` already `AS unity`/`USE unity`. **Legacy `UC_TEST_*` emission DROPPED
   from `engine.py`** (DB fully ported); py docstrings refreshed. `oss.py` **keeps** legacy
   `UC_TEST_*` (OSS bodies not yet migrated — gated on the step-2 container work). `conftest.py`
   `UC_TEST_CATALOG` note = the provisioner *input* var (step-6 env translation), correctly retained.
6. Env translation (`DATABRICKS_UC_*` → short internals; comment `env_databricks` exports).
7. `READ_ONLY` from access; OSS endpoint from env; collapse OSS double-injection.
8. `teardown_stale(older_than)` + `duck-test clean --older-than 30d`.

Steps 1–4, 6–8 stand alone today; step 5's preamble fully disappears once core `--init-sql` /
`--env-passthrough` land (later — model works without them).

## Core/unittest asks ⏭ (not blocking)

- `--env-passthrough=UC_TEST_*,...` (prefix/glob allowlist so driver needn't enumerate).
- `--init-sql='CREATE SECRET …; ATTACH … AS unity; USE …'` — pushes the preamble out of bodies.
Some may already exist in test-configs — check when we get there.
