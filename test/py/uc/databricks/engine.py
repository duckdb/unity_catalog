"""Databricks provisioning engine — concrete impl of driver's Provisioner protocol.

Turns a list[Requirement] into Databricks fixtures + a duckdb init script.

CELL GRAMMAR (pinned): an `rw` requirement is isolated in a schema named
    <commit>__<storage>__<token>      e.g.  cmt__managed__brave_otter
where <token> is the SQL-safe per-invocation id. Tables live BARE in that schema,
so a body references `simple_table` (not `simple_table_catalog_managed`) once the
default schema points at the cell.

THE 2x2 (commit x storage), per requirement:
                managed (UC-managed, no LOCATION)   external (explicit LOCATION)
  cmt   (catalog-managed commit protocol)  catalog-managed props, no LOCATION   catalog-managed props + LOCATION
  plain (no catalog-managed props)         no props, no LOCATION (UC-managed)    no props + LOCATION

`ro` requirements reference the source directly (no DDL); their binding is the
source FQN.

ASSUMPTIONS (cannot be verified here — no Databricks; see REPORT):
  - cmt + external (catalog-managed props WITH an explicit LOCATION) is accepted
    by Databricks. The existing generator only ever emits cmt-without-LOCATION, so
    this combination is UNVERIFIED. cmt + managed (the milestone target) mirrors
    the proven generator path exactly.
  - DROP SCHEMA ... CASCADE drops cloned managed tables and their storage.
"""

import os
import subprocess
import sys
from dataclasses import dataclass

import pytest

from ducktest import (
    State,
    TableSpec,
    find_duckdb,
    step,
)  # find_duckdb: resolve tools from one build; State: the working provision object
from ducktest.fixtures import (
    canonicalize,
    load_table_spec,
    map_columns,
    resolve_seed,
)
from ducktest.provision import (
    Bindings,
)  # the FRAMEWORK Bindings (make_init_sql receives it)
from ducktest.provision import Provisioner as _BaseProvisioner

# The databricks_gen library (atomic SQL primitives over the SDK) lives in scripts/.
# databricks -> uc -> py -> test -> <repo root>  (4 dirs up from this file's dir); put
# `scripts` on sys.path so `import databricks_gen` resolves even when the --repl flow loads
# this engine without the root conftest. databricks_gen imports the SDK lazily, so this
# stays cheap -- test COLLECTION never pulls in databricks-sdk.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
_SCRIPTS_DIR = os.path.join(_REPO_ROOT, "scripts")
if _SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, _SCRIPTS_DIR)

from databricks_gen import (  # noqa: E402  (needs _SCRIPTS_DIR on path)
    CATALOG_MANAGED_PROPS,
    DATABRICKS_TYPE_MAP,
    create_table,
    drop_schema,
    execute,
    insert,
    run_sql_file,
)

# Account config (S3 bucket, catalogs) -- env-overridable, in one place (see config.py).
from . import config  # noqa: E402
from ..identity import TableRef, build_env  # noqa: E402  (unified identity env contract)

# Two definition sources the provisioner instantiates from:
#   _FIXTURES  -- portable driver fixtures (id_name) for the write/attach tests: create + insert.
#   _DATA_DIR  -- Databricks Delta-artifact defs (evolution / column-mapping / catalog-managed)
#                 for the RO read tests: run verbatim via run_sql_file (`<table>.sql`, plus an
#                 optional `<table>.insert.sql` for the DuckDB UC write path).
_FIXTURES = os.path.join(_REPO_ROOT, "test", "fixtures")
_DATA_DIR = os.path.join(_REPO_ROOT, "test", "databricks", "data")


@dataclass
class TableBinding:
    """How a body should reference one provisioned requirement."""

    requirement_name: str  # bare name a body uses
    fqn: str  # fully-qualified physical table the name resolves to
    access: str  # ro | rw


# ---------------------------------------------------------------------------
# Env expansion + naming
# ---------------------------------------------------------------------------


def _expand(s: str) -> str:
    """Expand ${VAR} from the environment; raise on an unset referenced var."""
    out = os.path.expandvars(s)
    if "${" in out:
        # Clean UsageError (not a bare KeyError → INTERNALERROR) so a missing var
        # reads as a user fix, not a harness crash.
        raise pytest.UsageError(
            f"--repl: unresolved environment variable in @requires source {s!r} "
            f"(expanded to {out!r}). Set it in the environment "
            "(e.g. UC_TEST_CATALOG, or run under env_databricks)."
        )
    return out


def _default_catalog_env():
    """Default the neutral UC_TEST_CATALOG so `--repl` works without a wrapper.

    Honors an explicit UC_TEST_CATALOG (set by you, env_databricks, or CI);
    else falls back to the standard write-test catalog. Override by exporting
    UC_TEST_CATALOG.
    """
    os.environ.setdefault("UC_TEST_CATALOG", config.WRITE_CATALOG)


def cell_schema_name(commit: str, storage: str, token: str) -> str:
    """The pinned cell grammar: <commit>__<storage>__<token>."""
    return f"{commit}__{storage}__{token}"


def _split_source(source_fqn: str):
    """catalog.schema.table -> (catalog, schema, table)."""
    parts = source_fqn.split(".")
    if len(parts) != 3:
        raise ValueError(f"@requires source must be catalog.schema.table, got {source_fqn!r}")
    return parts[0], parts[1], parts[2]


# ---------------------------------------------------------------------------
# Per-spec DDL (full 2x2). cmt+managed reuses the generator's build_create_sql
# (proven path); the other cells build DDL locally for LOCATION/props control.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Credentials (the Databricks provider's connection creds)
# ---------------------------------------------------------------------------

# All four are REQUIRED. WAREHOUSE_ID is only used on the write path, but we require it regardless:
# a warehouse-less run would otherwise "validate" on the core three, then fail confusingly deep in a
# write test on an empty ${DATABRICKS_WAREHOUSE_ID}. Requiring it fails fast + clear up front instead.
_CRED_VARS = (
    "DATABRICKS_TOKEN",
    "DATABRICKS_ENDPOINT",
    "DATABRICKS_REGION",
    "DATABRICKS_WAREHOUSE_ID",
)
_LAST_OP_ERROR = None  # op's error from the last _op_fetch (controller) -- for the hard-fail message


def have_core_creds():
    """True if the required creds (TOKEN/ENDPOINT/REGION/WAREHOUSE_ID) are in the environment."""
    return all(os.environ.get(k) for k in _CRED_VARS)


def creds_complete(value):
    """True if a FETCHED creds dict carries the core vars -- value-based, for the driver's
    credential(validate=...) contract (validate(value)). Distinct from have_core_creds(), which reads
    os.environ (the driver adopts creds into env only AFTER this validate passes)."""
    return bool(value) and all(value.get(k) for k in _CRED_VARS)


def cred_failure_detail():
    """Why creds are unavailable: op's (sanitized) error if op ran and failed, else the missing env
    vars. Feeds the conftest's hard-fail message."""
    if _LAST_OP_ERROR:
        return _LAST_OP_ERROR
    missing = [k for k in _CRED_VARS if not os.environ.get(k)]
    return "not in the environment: " + ", ".join(missing) if missing else "unavailable"


def _require_creds():
    """Raise pytest.UsageError unless the core creds are in the environment.

    Creds are fetched once on the controller (load_creds: env-wins, else 1Password) + broadcast to
    workers; the conftest hard-fails when they're unavailable. This is the --repl-path guard.
    """
    if not have_core_creds():
        raise pytest.UsageError(f"Databricks credentials unavailable ({cred_failure_detail()}).")


def ensure_env(*, dry_run=False):
    """Make the Databricks env ready for a test or a provision step.

    Catalog default (cheap, always) + a creds CHECK (skipped on dry_run so
    --co / --provision-dry-run never need creds). Creds are populated once on the controller and
    broadcast to workers (see load_creds + the conftest); this only verifies. Safe to call
    repeatedly. The conftest hook calls this so the run path (run_paired) gets the catalog
    default + the creds check; --repl calls it too.
    """
    _default_catalog_env()
    if not dry_run:
        _require_creds()


# The 1Password item holding the databricks _env bundle (TOKEN/ENDPOINT/REGION/WAREHOUSE_ID).
_OP_CRED_SECRET = "op://testing-rw/databricks_ccv2/_env"
_ENV_VARS = _CRED_VARS  # the full bundle == the required set now (warehouse is required, see _CRED_VARS)


def load_creds(config=None):
    """Databricks creds as a {VAR: value} dict, fetched ONCE (called on the controller via the
    driver broadcast seam; `config` unused, matches the factory signature).

    Env-set vars ALWAYS win, per variable; 1Password fills only the gaps:
      - all required vars (TOKEN/ENDPOINT/REGION/WAREHOUSE_ID) in env -> return env, NO `op`
        (the wrapper / CI path);
      - any required var missing -> fetch the bundle, then overlay whatever env DID set, so a PARTIAL
        override survives (e.g. a personal TOKEN with ENDPOINT/REGION/WAREHOUSE from 1Password).
    If `op` is unavailable the result is just the env partials -> _require_creds then skips.
    """
    env = {k: os.environ[k] for k in _ENV_VARS if os.environ.get(k)}
    if all(k in env for k in _CRED_VARS):
        return env  # complete from env -> no op
    return {**_op_fetch(), **env}  # op fills the gaps; env wins per-var


def _op_fetch():
    """`op read <_env> | op inject`, parsed into {VAR: value}. Runs only when creds aren't already
    in the env, only on the controller. On failure returns {} and stashes op's (sanitized) error in
    _LAST_OP_ERROR so the conftest hard-fail can show WHY creds are unavailable.
    """
    global _LAST_OP_ERROR
    cmd = f"op read {_OP_CRED_SECRET} | op inject"
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        _LAST_OP_ERROR = f"`op` could not run: {e}"
        return {}
    if r.returncode != 0:
        detail = (r.stderr or r.stdout or "").strip() or f"op exited {r.returncode}"
        _LAST_OP_ERROR = f"`op` exit {r.returncode}: {detail}"
        return {}
    _LAST_OP_ERROR = None
    creds = {}
    for line in r.stdout.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("export "):
            s = s[len("export ") :].lstrip()
        key, sep, val = s.partition("=")
        if sep:
            creds[key.strip()] = val.strip().strip("\"'")
    return creds


# ---------------------------------------------------------------------------
# Provisioner protocol impl
# ---------------------------------------------------------------------------


class DatabricksProvisioner(_BaseProvisioner):
    """Concrete Provisioner (ducktest.provision.Provisioner) for Databricks Unity Catalog.

    The generic access-policy spec-loop + RO once-guard + teardown now live in the base
    (`ducktest.provision.Provisioner`); this class supplies the Databricks-specific hooks
    only. `access` decides POLICY (namespace + lifecycle), NOT how to instantiate:
      rw -> an isolated per-test cell in the write catalog, dropped on teardown;
      ro -> the shared table the source FQN names, instantiated once per session.
    The instantiation itself (fixture vs Databricks def) is `instantiate`'s job.
    """

    def __init__(self, config=None):
        super().__init__()
        # config lets make_init_sql resolve the duckdb build dir from the SAME build the
        # driver runs the unittest binary from (--build / $BUILD_DIR / --duckdb-bin),
        # instead of a hardcoded build/release. See uc.oss.OssProvisioner.
        self._config = config
        # Per-provision()-call bookkeeping for the unified identity env (see uc.identity /
        # WIP-identity-design.md) and the mono-cell default-schema reconciliation — reset
        # in before_provision(), accumulated in rw_target/ro_target, applied in
        # finalize_state/env_for. The base doesn't track this; it's backend-shaped.
        self._refs = []
        self._cell_for_default = None

    def before_provision(self, specs, token, dry_run):
        if not specs:
            # The framework allows --repl on a test with no @requires (bare REPL). The
            # databricks backend has no minimal-REPL path wired yet (it needs a cell to
            # ATTACH), so fail with a clean message rather than a bare ValueError.
            raise pytest.UsageError(
                "--repl on a databricks test needs @requires (no minimal-REPL path "
                "wired for databricks). Add @requires, or pick an OSS test for a bare REPL."
            )
        ensure_env(dry_run=dry_run)  # UC_TEST_CATALOG default + creds check (skipped on dry_run)
        self._refs = []
        self._cell_for_default = None

    def new_state(self, token, *, params=None) -> State:
        write_catalog = os.environ[
            "UC_TEST_CATALOG"
        ]  # before_provision's ensure_env defaulted it (config.WRITE_CATALOG)
        return State(token=token, catalog=write_catalog, default_schema="main")

    def rw_target(self, spec, token, state, dry_run):
        bare = spec.resolved_name()
        cell = cell_schema_name(spec.property("commit"), spec.property("storage"), token)
        target = f"{state.catalog}.{cell}.{bare}"
        self.ensure_isolated(f"{state.catalog}.{cell}", state, dry_run)
        if self._cell_for_default is None:
            self._cell_for_default = cell
        state.tables.append(TableBinding(bare, target, "rw"))
        self._refs.append(TableRef(bare, state.catalog, cell, bare, "rw"))
        return target

    def ro_target(self, spec, state):
        # ro sources are FQN strings naming a shared, premade/def table.
        target = _expand(spec.source)
        cat, sch, tbl = _split_source(target)
        state.catalog, state.default_schema = cat, sch
        state.tables.append(TableBinding(spec.resolved_name(), target, "ro"))
        self._refs.append(TableRef(spec.resolved_name(), cat, sch, tbl, "ro"))
        return target

    def finalize_state(self, state):
        # Mono-cell default schema: the (first) rw cell if any, else the ro source schema.
        if self._cell_for_default is not None:
            state.default_schema = self._cell_for_default

    def env_for(self, state) -> dict:
        # Primary (bare CATALOG/SCHEMA/TABLE) = the first rw cell, else the first ref.
        # Env a run path injects (body reads these; see the .test): the unified identity
        # contract (CATALOG/SCHEMA/TABLE + per-key {KEY}/{KEY_*} aliases). All DB bodies
        # are ported off the legacy UC_TEST_* keys.
        primary = next(
            (r for r in self._refs if r.access == "rw"),
            self._refs[0] if self._refs else None,
        )
        return build_env(self._refs, primary=primary)

    def dry_run_summary(self, state):
        print(f"cell schema(s): {state.isolated or '(none — all ro)'}")
        print(f"DEFAULT_SCHEMA: {state.default_schema}")

    def instantiate(self, spec, target, dry_run, state):
        """Instantiate `target` from `spec`'s definition. Dispatches on definition TYPE (the
        instantiation method) -- independent of `access`, which set target + lifecycle above.
        """
        if isinstance(spec.source, TableSpec):
            self._instantiate_fixture(spec, target, dry_run, state)
        else:
            self._instantiate_def(target, dry_run, state)

    def _instantiate_fixture(self, spec, target, dry_run, state):
        """Seed a portable TableSpec into `target` with the 2x2 props/location (create + insert).

        The one path for write/attach tests. Dry-run only NAMES the fixture + cell (no duckdb
        canonicalization, no I/O -- mirrors the OSS provisioner); the real path canonicalizes
        -> columns + seed, then create_table (with the cell's commit/storage props/location) +
        insert.
        """
        name = spec.source.name  # fixture logical name -- pure, no I/O
        cell_desc = f"{spec.property('commit') or 'plain'}/{spec.property('storage') or 'managed'}"
        state.plan.append(f"[{spec.access}] seed {target} from fixture {name!r} ({cell_desc})")
        if dry_run:
            return
        props = dict(CATALOG_MANAGED_PROPS) if spec.property("commit") == "cmt" else None
        location = self._s3_location(target) if spec.property("storage") == "external" else None
        definition = load_table_spec(spec.source, [_FIXTURES])
        tbl = canonicalize(self._duckdb_shell(), definition)
        cols = ", ".join(f"{n} {t}" for n, t in map_columns(tbl, DATABRICKS_TYPE_MAP))
        rows = resolve_seed(spec.source.seed, tbl.seed_data)
        with step(f"seed {target} from fixture {name!r} ({cell_desc})"):
            create_table(target, cols, properties=props, location=location)
            if rows:
                insert(target, rows)

    def _instantiate_def(self, target, dry_run, state):
        """Run a Databricks Delta-artifact def (test/databricks/data/<table>.sql) verbatim; a
        companion <table>.insert.sql seeds via the DuckDB UC write path (the cmt__duckdb case).
        No def -> the table is treated as premade (reference only, no DDL).
        """
        _, _, table = _split_source(target)
        def_path = os.path.join(_DATA_DIR, f"{table}.sql")
        if not os.path.isfile(def_path):
            state.plan.append(f"[ro] reference {target} (premade, no def)")
            return
        with open(def_path) as f:
            external = "{location}" in f.read()
        location = self._s3_location(target) if external else None
        insert_path = os.path.join(_DATA_DIR, f"{table}.insert.sql")
        state.plan.append(f"[ro] provision {target} from {os.path.basename(def_path)}")
        if os.path.isfile(insert_path):
            state.plan.append(f"    + DuckDB UC insert ({os.path.basename(insert_path)})")
        if dry_run:
            return
        with step(f"provision {target} from {os.path.basename(def_path)}"):
            run_sql_file(def_path, table=target, location=location)
            if os.path.isfile(insert_path):
                self._duckdb_insert(insert_path, target)

    def _s3_location(self, target):
        """The S3 LOCATION for an `external` target:
        s3://<bucket>/external/<cat>/<schema>/<table>.

        Only external tables get a LOCATION from us; the bucket's `managed/` half is the
        catalog's own managed root, which UC fills in server-side.
        """
        cat, schema, table = _split_source(target)
        return f"s3://{config.S3_BUCKET}/external/{cat}/{schema}/{table}"

    def _duckdb_shell(self):
        """The duckdb shell from the same build the driver resolves the unittest binary from."""
        wd = getattr(self._config, "sqllogic_working_dir", None) or os.getcwd()
        return find_duckdb(self._config, wd)

    def _duckdb_insert(self, path, target):
        """Run a companion .insert.sql through the build's duckdb (the UC write path). Its
        ${DATABRICKS_*} creds are expanded from the env (present on the run path); it addresses
        its table through the same `{table_name}` substitution the def file gets, plus the
        `{catalog}`/`{schema}` halves the ATTACH needs."""
        cat, schema, _ = _split_source(target)
        wd = getattr(self._config, "sqllogic_working_dir", None) or os.getcwd()
        duckdb_bin = find_duckdb(self._config, wd)
        with open(path) as f:
            sql = os.path.expandvars(f.read())
        sql = sql.replace("{table_name}", target).replace("{catalog}", cat).replace("{schema}", schema)
        proc = subprocess.run([duckdb_bin, "-unsigned", "-c", sql], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"DuckDB UC insert failed ({os.path.basename(path)}):\n{proc.stderr.strip()}")

    def execute(self, sql):
        """Route through databricks_gen's execute (the SDK transport) — used by the base
        teardown() default; this backend overrides teardown() below for step() narration,
        so execute() is only reached if something calls it directly."""
        execute(sql)

    def teardown(self, bindings=None) -> None:
        """DROP each cell schema CASCADE (databricks_gen.drop_schema). `bindings.isolated`
        holds full namespaces (catalog.cell — see ensure_isolated in rw_target)."""
        namespaces = list(bindings.isolated) if bindings else []
        if not namespaces:
            print(f"teardown: nothing to drop for token={bindings.token if bindings else '?'}")
            return
        for ns in namespaces:
            with step(f"drop cell schema {ns} CASCADE"):
                drop_schema(ns, cascade=True)

    def make_init_sql(self, bindings: Bindings, *, redact: bool = False) -> str:
        """duckdb init SQL for `duckdb -unsigned -init`.

        Mirrors write_catalog_managed.test: LOAD local extensions, CREATE SECRET,
        ATTACH the catalog with the cell as DEFAULT_SCHEMA, USE it. Extension paths come
        from the SAME build as the resolved tools (find_duckdb); redact=True (used by
        --provision-dry-run, which prints this without launching) falls back to the
        env/default so it needs no built binary, and masks the token.

        Receives the FRAMEWORK Bindings; the backend-shaped catalog/default_schema/tables
        live under `bindings.backend` (the working State), not on Bindings itself.
        """
        state = bindings.backend
        if redact:
            # dry-run print: must not require a built binary.
            build_dir = os.environ.get("BUILD_DIR", os.path.join(_REPO_ROOT, "build", "release"))
        else:
            wd = getattr(self._config, "sqllogic_working_dir", None) or os.getcwd()
            build_dir = os.path.dirname(find_duckdb(self._config, wd))

        def ext(name):
            return os.path.join(build_dir, "extension", name, f"{name}.duckdb_extension")

        token = "***REDACTED***" if redact else os.environ.get("DATABRICKS_TOKEN", "${DATABRICKS_TOKEN}")
        endpoint = os.environ.get("DATABRICKS_ENDPOINT", "${DATABRICKS_ENDPOINT}")
        region = os.environ.get("DATABRICKS_REGION", "${DATABRICKS_REGION}")

        table_lines = "\n".join(f".print '  {t.requirement_name}  ->  {t.fqn}  ({t.access})'" for t in state.tables)

        return f"""-- Auto-generated by uc.databricks.engine.make_init_sql for `duckdb -unsigned -init`.
-- -unsigned (launch flag) is required to LOAD locally-built extensions.
LOAD '{ext("parquet")}';
LOAD '{ext("httpfs")}';
LOAD '{ext("delta")}';
LOAD '{ext("unity_catalog")}';

CREATE SECRET (
    TYPE UC,
    TOKEN '{token}',
    ENDPOINT '{endpoint}',
    AWS_REGION '{region}'
);

ATTACH '{state.catalog}' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA '{state.default_schema}');
USE unity;

.print ''
.print '== pytest --repl ready =='
.print 'Attached: unity -> {state.catalog}, DEFAULT_SCHEMA {state.default_schema}'
.print 'Provisioned (reference tables BARE; the cell IS the default schema):'
{table_lines}
.print ''
"""
