"""Driver for column_mapping_partitioned.test: a partitioned table with column mapping, which Spark wrote,
read and appended to through the UC catalog path.

The fixtures are Spark's (`data/column_mapping/spark_partitioned_{name,id}_mode`): files with physical
column names and field ids, and partition values keyed by the partition column's physical name. Each run
copies one and registers it over REST as an EXTERNAL table, so the INSERTs never touch the committed
tree, and the log, not `uctl`, says what the table is.
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import REPO_ROOT, plain_table_location, register_external_table, uctl

CATALOG = "duck"
SCHEMA = "plain"
FIXTURES = REPO_ROOT / "data" / "column_mapping"
COLUMNS = [
    ("id", "int", "INT", '{"name":"id","type":"integer","nullable":true,"metadata":{}}'),
    ("p", "string", "STRING", '{"name":"p","type":"string","nullable":true,"metadata":{}}'),
    ("v", "int", "INT", '{"name":"v","type":"integer","nullable":true,"metadata":{}}'),
]


@pytest.mark.oss_local
@pytest.mark.parametrize("mode", ["name", "id"])
def test_column_mapping_partitioned(request, uc_server, mode):
    endpoint = os.environ.get("UC_ENDPOINT", uc_server.endpoint)
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table names and their storage locations.
    table = f"column_mapping_partitioned_{mode}_{uuid.uuid4().hex[:8]}"
    location = plain_table_location(uc_server, table)

    try:
        shutil.copytree(FIXTURES / f"spark_partitioned_{mode}_mode", location)
        register_external_table(endpoint, table, location, COLUMNS)
        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": table,
            "UC_TEST_TABLE_LOCATION": str(location),
            "UC_ENDPOINT": endpoint,
        }
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, table, check=False)
        shutil.rmtree(location, ignore_errors=True)
