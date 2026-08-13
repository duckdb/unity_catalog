"""Driver for catalog_managed_path_read.test -- report C2, with the assertion corrected.

The report asks for a regression on a path-vs-catalog COUNT MISMATCH. That scenario is not
reachable: since kernel v0.21.0 a path read of a catalog-managed table hard-errors rather than
returning stale rows, so there is no mismatch to observe. Asserting one would encode the
diagnosis we are correcting. What this test pins instead is the property that actually protects
the user -- the path read FAILS, and the catalog read is the only way in.

Provisioning is hand-rolled because the storage location has to reach the body: `uctl create cmt`
makes a MANAGED (= catalog-managed, in this UC build) table, then `uctl get` reports where UC put
it, and that path is injected for the delta_scan attempt.
"""

import json
import os

import pytest

from ducktest import run_paired
from uc import uctl

TABLE = "catalog_managed_c2"
SCHEMA = "cmt"  # MANAGED -> catalog-managed commits; see scripts/oss_uc_image/uctl


def _storage_location(schema, table):
    """Where UC placed the managed table, from `uctl get`."""
    r = uctl("get", schema, table)
    out = (r.stdout or "").strip()
    try:
        return json.loads(out).get("storage_location")
    except json.JSONDecodeError:
        # Tolerate a non-JSON CLI rendering: find the first path-looking token.
        for line in out.splitlines():
            if "storage_location" in line:
                return line.split(None, 1)[-1].strip().strip('",')
    return None


@pytest.mark.oss_local
def test_catalog_managed_path_read(request, uc_server):
    uctl("drop", SCHEMA, TABLE, check=False)
    uctl("create", SCHEMA, TABLE, "id INT, name STRING")

    location = _storage_location(SCHEMA, TABLE)
    if not location:
        uctl("drop", SCHEMA, TABLE, check=False)
        pytest.skip("could not determine the managed table's storage_location from `uctl get`")

    env = {
        **os.environ,
        "UC_TEST_CATALOG": "duck",
        "UC_TEST_SCHEMA": SCHEMA,
        "UC_TEST_TABLE": TABLE,
        "UC_CMT_STORAGE_LOCATION": location,
    }
    try:
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, TABLE, check=False)
