"""OSS UC provisioner (sibling of uc.databricks.DatabricksProvisioner).

Serves BOTH the driver's generic `resources` fixture (the run path) and `--repl`:

  * RUN path (a session `uc_server` is active): reuse that container and INSTANTIATE
    each `@requires(source=TableSpec(...))` as a table via the duckdb middleman
    (load -> canonicalize -> map to UC types -> `uctl create`). rw specs get a unique
    per-test table `<name>_rw_<token>` (dropped on teardown); ro specs get a shared
    table created once per session. The body reads the rw table via ${UC_TEST_TABLE}.

  * --repl (no fixtures run, so the provisioner owns the container): start a fresh
    container and seed the legacy playground table `id_name` in both schemas. Unchanged.

The `commit` axis (cmt / plain) IS the seeded schema name, so it maps 1:1 to the schema.
Seed-data insertion into UC is not wired yet, so seeded fixtures must use
`TableSpec(...).Seed(None)` (empty table; the body does its own inserts) for now.
"""

import os
from dataclasses import dataclass, field

from ducktest import ProvisionFailed, TableSpec, find_duckdb
from ducktest.fixtures import canonicalize, load_table_spec, map_columns, resolve_seed

from uc import REPO_ROOT, server, uctl
from uc.identity import TableRef, build_env  # unified identity env contract

# Locally-built extensions the body needs; LOADed by full path (duckdb -unsigned).
_EXTS = ("parquet", "httpfs", "delta", "unity_catalog")

# Legacy --repl playground table (fixed shape), seeded in each schema for interactive use.
_SEED_TABLE = "id_name"
_SEED_COLUMNS = "id INT, name STRING"

# TableSpec root (uc-module-generic, shared by oss + databricks).
_FIXTURES = REPO_ROOT / "test" / "fixtures"

# DuckDB logical types -> UC/Spark types for `uctl create` column specs.
UC_TYPE_MAP = {
    "INTEGER": "INT",
    "BIGINT": "LONG",
    "SMALLINT": "SMALLINT",
    "TINYINT": "TINYINT",
    "VARCHAR": "STRING",
    "DOUBLE": "DOUBLE",
    "FLOAT": "FLOAT",
    "BOOLEAN": "BOOLEAN",
    "DATE": "DATE",
    "TIMESTAMP": "TIMESTAMP",
    "DECIMAL": "DECIMAL",
}


@dataclass
class OssBindings:
    """Result of provision(): what make_init / teardown need (cf. databricks Bindings)."""

    catalog: str
    default_schema: str
    token: str
    data_dir: str = None  # container's host bind-mount (only when we own the container)
    seeded: list = field(default_factory=list)  # (schema, table) pairs to drop on teardown
    env: dict = field(default_factory=dict)
    plan: list = field(default_factory=list)
    owns_container: bool = False  # True only on the --repl path (we started the container)


# The @requires `commit` axis carries the semantic table type -- "cmt" (catalog-managed)
# or "plain" -- which ARE the OSS seed schema names, so it maps 1:1 to the schema. (The
# databricks provisioner reads both `commit` and `storage`; OSS only needs `commit`.)
def _commit_schema(spec):
    return spec.property("commit") or "cmt"


def _default_schema_for(specs):
    """DEFAULT_SCHEMA to ATTACH: first rw spec's commit type (= schema), else cmt."""
    for s in specs:
        if s.access == "rw":
            return _commit_schema(s)
    return "cmt"


class OssProvisioner:
    """Provisioner protocol impl (driver/provision.py) for the OSS UC ducktest container."""

    def __init__(self, config=None):
        # config lets us resolve the duckdb shell from the SAME build the driver runs the
        # unittest binary from (--build / $BUILD_DIR / --unittest-binary), not a fixed path.
        self._config = config
        # (schema, table) of shared ro tables created this session (created once).
        self._shared_ro = set()

    def _duckdb_shell(self):
        """The duckdb shell from the same build the driver resolves the unittest binary from."""
        wd = getattr(self._config, "sqllogic_working_dir", None) or os.getcwd()
        return find_duckdb(self._config, wd)

    def provision(self, specs, token, *, dry_run=False, params=None) -> OssBindings:
        os.environ.setdefault("UC_TEST_CATALOG", server._CATALOG)  # "duck"
        catalog = os.environ["UC_TEST_CATALOG"]
        # A parametrized test's `schema` param picks the REPL context; else first rw
        # @requires storage, else cmt.
        default_schema = (params or {}).get("schema")
        if default_schema not in server._SEED_SCHEMAS:
            default_schema = _default_schema_for(specs)

        b = OssBindings(catalog=catalog, default_schema=default_schema, token=token)
        b.env = {"UC_TEST_CATALOG": catalog, "UC_TEST_SCHEMA": default_schema}

        if dry_run:
            b.plan.append(f"start OSS UC container {server.IMAGE} on {server.ENDPOINT}")
            for s in specs:
                if isinstance(s.source, TableSpec):
                    b.plan.append(f"instantiate fixture {s.source.name!r} -> {_commit_schema(s)} ({s.access})")
            if not any(isinstance(s.source, TableSpec) for s in specs):
                for schema in server._SEED_SCHEMAS:
                    b.plan.append(f'uctl create {schema} {_SEED_TABLE} "{_SEED_COLUMNS}"')
            b.plan.append(f"ATTACH '{catalog}' AS duck (DEFAULT_SCHEMA '{default_schema}')")
            print("provision plan (NO container started):")
            for line in b.plan:
                print(f"  {line}")
            return b

        if server.active_server() is None:
            # --repl path: own a fresh container + seed the legacy playground (unchanged).
            b.owns_container = True
            try:
                srv = server.start_container()
            except ProvisionFailed:
                raise
            except Exception as e:  # docker run / readiness failure -- semantically provisioning
                raise ProvisionFailed(f"starting the OSS UC container failed: {e}") from e
            b.data_dir = srv.data_dir
            for schema in server._SEED_SCHEMAS:
                uctl("drop", schema, _SEED_TABLE, check=False)  # idempotent clean slate
                uctl("create", schema, _SEED_TABLE, _SEED_COLUMNS)
                b.seeded.append((schema, _SEED_TABLE))
            return b

        # RUN path: reuse the session container; instantiate each TableSpec spec as a table.
        duckdb_bin = self._duckdb_shell()
        refs = []  # unified identity refs -> bindings.env (see uc.identity)
        primary_ref = None
        for s in specs:
            if not isinstance(s.source, TableSpec):
                continue
            schema = _commit_schema(s)
            name = self._instantiate(duckdb_bin, s, schema, token, b)
            ref = TableRef(s.resolved_name(), catalog, schema, name, s.access)
            refs.append(ref)
            if s.access == "rw" and primary_ref is None:
                primary_ref = ref
        if refs:
            if primary_ref is None:
                primary_ref = refs[0]
            b.default_schema = primary_ref.schema
            # Legacy UC_TEST_* kept during migration to the unified contract; drop once ported.
            b.env = {
                "UC_TEST_CATALOG": catalog,
                "UC_TEST_SCHEMA": b.default_schema,
                "UC_TEST_TABLE": primary_ref.table,
                **build_env(refs, primary=primary_ref),
            }
        return b

    def _instantiate(self, duckdb_bin, spec, schema, token, b) -> str:
        """Create one table for `spec` in `schema`; return its name.

        rw -> a unique per-test `<name>_rw_<token>` (dropped on teardown); ro -> a shared
        table created ONCE per session (guarded; left for the session).
        """
        try:
            definition = load_table_spec(spec.source, [str(_FIXTURES)])
            table = canonicalize(duckdb_bin, definition)
        except ProvisionFailed:
            raise
        except Exception as e:  # fixture read / duckdb-middleman inspection -- semantically provisioning
            raise ProvisionFailed(
                f"inspecting fixture {spec.source.name!r} via the duckdb middleman failed: {e}"
            ) from e
        col_spec = ", ".join(f"{n} {t}" for n, t in map_columns(table, UC_TYPE_MAP))

        if spec.access == "rw":
            name = f"{spec.resolved_name()}_rw_{token}"
        else:
            name = spec.resolved_name()
            if (schema, name) in self._shared_ro:
                return name  # created earlier this session -> reuse (shared)

        rows = resolve_seed(spec.source.seed, table.seed_data)
        if rows:
            raise NotImplementedError(
                f"OSS seed-data insertion is not wired yet (fixture {name!r} would seed "
                f"{len(rows)} rows). Use TableSpec(...).Seed(None) for an empty table until "
                "the UC insert path lands."
            )
        uctl("drop", schema, name, check=False)  # clean slate (rw) / first create (ro)
        uctl("create", schema, name, col_spec)
        if spec.access == "rw":
            b.seeded.append((schema, name))  # per-test -> drop on teardown
        else:
            self._shared_ro.add((schema, name))  # shared -> created once, lives for session
        return name

    def make_init_sql(self, b: OssBindings, *, redact: bool = False) -> str:
        """duckdb init SQL for `duckdb -unsigned -init` (the --repl playground).

        LOAD local extensions, CREATE SECRET (the OSS token is the literal 'not-used',
        so nothing to redact), ATTACH duck with the chosen DEFAULT_SCHEMA, USE it.
        Extension paths come from the SAME build as the resolved tools (find_duckdb);
        for --provision-dry-run (redact=True) we print without requiring a built binary.
        """
        if redact:
            # dry-run print: must not require a built binary -> env/default (unchanged).
            build_dir = os.environ.get("BUILD_DIR", os.path.join(str(REPO_ROOT), "build", "release"))
        else:
            # real launch: the duckdb shell (hence its build dir) is a precondition here.
            wd = getattr(self._config, "sqllogic_working_dir", None) or os.getcwd()
            build_dir = os.path.dirname(find_duckdb(self._config, wd))

        def ext(name):
            return os.path.join(build_dir, "extension", name, f"{name}.duckdb_extension")

        loads = "\n".join(f"LOAD '{ext(n)}';" for n in _EXTS)
        return f"""-- Auto-generated by uc.oss.OssProvisioner for `duckdb -unsigned -init`.
-- -unsigned (launch flag) is required to LOAD locally-built extensions.
{loads}

CREATE SECRET (
    TYPE UNITY_CATALOG,
    TOKEN 'not-used',
    ENDPOINT '{server.ENDPOINT}',
    AWS_REGION 'us-east-2'
);

ATTACH '{b.catalog}' AS duck (TYPE unity_catalog, DEFAULT_SCHEMA '{b.default_schema}');
USE duck;

.print ''
.print '== pytest --repl (OSS UC) ready =='
.print 'Attached: duck -> {b.catalog}, DEFAULT_SCHEMA {b.default_schema}'
.print 'Seeded: duck.cmt.{_SEED_TABLE}, duck.plain.{_SEED_TABLE}  ({_SEED_COLUMNS})'
.print 'USE duck.plain for the plain table; USE duck.cmt for catalog-managed.'
.print ''
"""

    def teardown(self, bindings=None) -> None:
        """Drop per-test tables; stop the container only if THIS provision owned it (--repl)."""
        for schema, table in getattr(bindings, "seeded", None) or []:
            uctl("drop", schema, table, check=False)
        if bindings and getattr(bindings, "owns_container", False):
            server.stop_container(getattr(bindings, "data_dir", None))
