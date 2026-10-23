"""Driver for column_mapping_nested.test: column-mapped tables with nested columns, which Spark wrote, read
and appended to through the UC catalog path.

Two fixtures per mapping mode, copied and registered over REST as EXTERNAL tables:
`data/column_mapping/spark_nested_<mode>_mode` (STRUCT, LIST and MAP columns, a struct field renamed between
two writes) and `data/column_mapping/reserved_physical_name_<mode>` (a struct field whose physical name is
the one DuckDB's parquet writer reserves for field ids, which an INSERT must refuse).
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import REPO_ROOT, delta_log_columns, plain_table_location, register_external_table, uctl

CATALOG = "duck"
SCHEMA = "plain"
FIXTURES = REPO_ROOT / "data" / "column_mapping"


@pytest.mark.oss_local
@pytest.mark.parametrize("mode", ["name", "id"])
def test_column_mapping_nested(request, uc_server, mode):
    endpoint = os.environ.get("UC_ENDPOINT", uc_server.endpoint)
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table names and their storage locations.
    token = uuid.uuid4().hex[:8]
    tables = {
        f"spark_nested_{mode}_mode": f"column_mapping_nested_{mode}_{token}",
        f"reserved_physical_name_{mode}_mode": f"column_mapping_reserved_{mode}_{token}",
    }
    locations = {fixture: plain_table_location(uc_server, table) for fixture, table in tables.items()}

    try:
        for fixture, table in tables.items():
            shutil.copytree(FIXTURES / fixture, locations[fixture])
            register_external_table(endpoint, table, locations[fixture], delta_log_columns(locations[fixture]))
        nested, reserved = tables.values()
        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": nested,
            "UC_TEST_TABLE_LOCATION": str(locations[f"spark_nested_{mode}_mode"]),
            "UC_TEST_RESERVED_TABLE": reserved,
            "UC_TEST_RESERVED_LOCATION": str(locations[f"reserved_physical_name_{mode}_mode"]),
            "UC_ENDPOINT": endpoint,
        }
        run_paired(request, env=env)
    finally:
        for fixture, table in tables.items():
            uctl("drop", SCHEMA, table, check=False)
            shutil.rmtree(locations[fixture], ignore_errors=True)
