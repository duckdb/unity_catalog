"""Driver for char_varchar_width.test: an over-length write through the UC catalog path.

A write through `ATTACH … (TYPE unity_catalog)` reaches `DeltaCatalog::PlanInsert` via
`child_catalog_mode` -- a different branch from the plain `ATTACH … (TYPE delta)` that
duckdb-delta's own tests cover. So this exists to prove the width check is REACHABLE through the
catalog plumbing, not to re-test the rule itself.

The fixture is a committed Delta log rather than a TableSpec, and has to be: the bound lives in
Delta field metadata (`__CHAR_VARCHAR_TYPE_STRING`) that only Spark writes. DuckDB has no
fixed-length char type at all -- it parses CHAR(5) and discards the width -- so neither the duckdb
middleman nor `uctl create` can express it. We copy `data/char_varchar_c1` into the table's
storage location (a copy, so the INSERTs below never mutate the committed tree) and register that
location with UC.

TODO: once Spark-backed fixtures land, express this as `@requires` and let the provisioner do it.
That supplies the per-run token and teardown, and most of this driver goes away.
"""

import os
import pathlib
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import uctl

CATALOG = "duck"
SCHEMA = "plain"  # EXTERNAL -> uctl gives the table a location we can stage into
FIXTURE = pathlib.Path(__file__).resolve().parents[2] / "data" / "char_varchar_c1"


@pytest.mark.oss_local
def test_char_varchar_width(request, uc_server):
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table name and its storage location.
    table = f"char_varchar_c1_{uuid.uuid4().hex[:8]}"

    # uctl gives a plain table `<data dir>/<catalog>/<schema>/<table>`; stage the log there so the
    # registration lands on it. The data dir is bind-mounted at the same path host and container.
    location = pathlib.Path(uc_server.data_dir) / CATALOG / SCHEMA / table
    shutil.copytree(FIXTURE, location)

    # UC's own column types are irrelevant to the check -- enforcement reads the Delta schema,
    # and UC drops the width anyway -- but keep them honest.
    uctl("create", SCHEMA, table, "id INT, code STRING")

    env = {
        **os.environ,
        "UC_TEST_CATALOG": CATALOG,
        "UC_TEST_SCHEMA": SCHEMA,
        "UC_TEST_TABLE": table,
    }
    try:
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, table, check=False)
        shutil.rmtree(location, ignore_errors=True)
