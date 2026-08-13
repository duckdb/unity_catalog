"""Driver for char_varchar_width.test -- the reporter's C1 acceptance step, run through UC.

Their repro writes the over-length value through `ATTACH … (TYPE unity_catalog)`, which reaches
`DeltaCatalog::PlanInsert` via `child_catalog_mode` -- a different branch from the plain
`ATTACH … (TYPE delta)` that duckdb-delta's own tests cover. So this exists to prove the width
check is REACHABLE through the catalog plumbing, not to re-test the rule.

The fixture is a committed Delta log rather than a TableSpec, and has to be: the bound lives in
Delta field metadata (`__CHAR_VARCHAR_TYPE_STRING`) that only Spark writes. DuckDB has no
fixed-length char type at all -- it parses CHAR(5) and discards the width -- so neither the duckdb
middleman nor `uctl create` can express it. We copy `data/char_varchar_c1` into the table's
storage location (a copy, so the INSERTs below never mutate the committed tree) and register that
location with UC.
"""

import os
import pathlib
import shutil
import subprocess

import pytest

from ducktest import run_paired
from uc import uctl

TABLE = "char_varchar_c1"
SCHEMA = "plain"  # EXTERNAL -> uctl gives the table a location we can stage into
FIXTURE = pathlib.Path(__file__).resolve().parents[2] / "data" / "char_varchar_c1"


def _data_root(container):
    """The data dir the server is using -- bind-mounted at the identical path on host and
    container (scripts/oss_uc_image/run), so a host-side copy lands where UC will look."""
    r = subprocess.run(
        ["docker", "exec", container, "printenv", "DUCKTEST_UC_DATA_DIR"],
        capture_output=True,
        text=True,
    )
    return (r.stdout.strip() if r.returncode == 0 else "") or "/home/unitycatalog/etc/data"


@pytest.mark.oss_local
def test_char_varchar_width(request, uc_server):
    container = os.environ.get("DUCKTEST_UC_CONTAINER", "ducktest-uc")
    location = pathlib.Path(_data_root(container)) / "duck" / SCHEMA / TABLE

    # Stage a fresh copy: uctl create derives this same storage_location, so the registration
    # lands on the log we just placed.
    shutil.rmtree(location, ignore_errors=True)
    shutil.copytree(FIXTURE, location)

    # UC's own column types are irrelevant to the check -- enforcement reads the Delta schema,
    # and UC drops the width anyway -- but keep them honest.
    uctl("drop", SCHEMA, TABLE, check=False)
    uctl("create", SCHEMA, TABLE, "id INT, code STRING")

    env = {
        **os.environ,
        "UC_TEST_CATALOG": "duck",
        "UC_TEST_SCHEMA": SCHEMA,
        "UC_TEST_TABLE": TABLE,
    }
    try:
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, TABLE, check=False)
        shutil.rmtree(location, ignore_errors=True)
