"""Driver for catalog_managed_path_read.test: reading a catalog-managed table off storage.

Such a read cannot return a stale-but-plausible count -- since kernel v0.21.0 it hard-errors
instead of answering from the log on disk. So the property worth pinning is that the path read
FAILS and the catalog is the only way in, not that two counts differ.

Provisioning is hand-rolled because the storage location has to reach the body: `uctl create cmt`
makes a MANAGED (= catalog-managed, in this UC build) table, then UC reports where it put it, and
that path is injected for the delta_scan attempt.

TODO: once the provisioner can surface a table's storage location, express this as `@requires`.
That supplies the per-run token and teardown, and the uuid below goes away with it.
"""

import json
import os
import uuid

import pytest

from ducktest import run_paired
from uc import uctl

CATALOG = "duck"
SCHEMA = "cmt"  # MANAGED -> catalog-managed commits; see scripts/oss_uc_image/uctl


def _storage_location(catalog, schema, table):
    """Where UC placed the managed table.

    Goes through uctl's raw escape hatch to pass `--output json`: the CLI otherwise renders an
    ASCII table, which is for humans and would have to be scraped.
    """
    r = uctl("uc", "table", "get", "--full_name", f"{catalog}.{schema}.{table}", "--output", "json")
    return json.loads((r.stdout or "").strip()).get("storage_location")


@pytest.mark.oss_local
def test_catalog_managed_path_read(request, uc_server):
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table name.
    table = f"catalog_managed_c2_{uuid.uuid4().hex[:8]}"
    uctl("create", SCHEMA, table, "id INT, name STRING")

    try:
        location = _storage_location(CATALOG, SCHEMA, table)
        if not location:
            # Not a skip: a MANAGED table always has one, so its absence is a real defect and
            # skipping would bury the only thing this test needs.
            raise AssertionError(f"UC reported no storage_location for {CATALOG}.{SCHEMA}.{table}")

        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": table,
            "UC_CMT_STORAGE_LOCATION": location,
        }
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, table, check=False)
