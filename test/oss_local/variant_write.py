"""Driver for variant_write.test: duckdb-delta writes VARIANT rows into Spark-created tables.

Registers a copy of the top-level, unshredded and shredded fixtures (the INSERTs never touch the committed tree) over REST,
because `uctl` cannot state a nested type. `CREATE TABLE` is not how these start: this extension
cannot create a table through the catalog, and duckdb-delta refuses a VARIANT column in one.
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import plain_table_location, register_external_table, uctl
from uc.server import ENDPOINT
from uc.variant_fixtures import COLUMNS, FIXTURES, TOPLEVEL

CATALOG = "duck"
SCHEMA = "plain"


@pytest.mark.oss_local
def test_variant_write(request, uc_server):
    token = uuid.uuid4().hex[:8]
    specs = {"toplevel": TOPLEVEL, **{name: (source, COLUMNS, props) for name, (source, props) in FIXTURES.items()}}
    tables = {name: f"variant_w_{name}_{token}" for name in specs}
    locations = {name: plain_table_location(uc_server, table) for name, table in tables.items()}

    try:
        for name, (source, columns, properties) in specs.items():
            shutil.copytree(source, locations[name])
            register_external_table(ENDPOINT, tables[name], locations[name], columns, properties)
        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": tables["toplevel"],
            "UC_TEST_TABLE_LOCATION": str(locations["toplevel"]),
            "UC_TEST_TABLE_NESTED": tables["unshredded"],
            "UC_TEST_TABLE_SHREDDED": tables["shredded"],
        }
        run_paired(request, env=env)
    finally:
        for name, table in tables.items():
            uctl("drop", SCHEMA, table, check=False)
            shutil.rmtree(locations[name], ignore_errors=True)
