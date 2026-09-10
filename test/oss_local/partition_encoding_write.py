"""Driver for partition_encoding_write.test: partitioned INSERTs through the UC catalog path.

A write through `ATTACH … (TYPE unity_catalog)` plans its insert down a different branch than a
plain `ATTACH … (TYPE delta)`, so what duckdb-delta's own tests prove about partition-value encoding
does not carry over on its own. Two properties have to survive that branch: a NULL partition value
that stays distinct from the empty string, and an `add.path` escaped so it can be decoded back to
the file it names.

The fixture is a committed Delta log rather than a TableSpec, and has to be: `partitionColumns`
lives in the log's metaData, which neither `uctl create` nor the duckdb middleman can express. It is
copied, so the INSERTs never mutate the committed tree.

The two cases get a table each, which keeps the commit numbering the body's assertions index into
independent of one another.
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import REPO_ROOT, plain_table_location, uctl

CATALOG = "duck"
SCHEMA = "plain"  # EXTERNAL -> uctl gives the table a location we can stage into
COLUMNS = "id INT, code STRING, p STRING"
FIXTURE = REPO_ROOT / "data" / "partition_encoding"


@pytest.mark.oss_local
def test_partition_encoding_write(request, uc_server):
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table names and their storage locations.
    token = uuid.uuid4().hex[:8]
    tables = {"null": f"partition_null_{token}", "enc": f"partition_enc_{token}"}
    locations = {k: plain_table_location(uc_server, t) for k, t in tables.items()}

    try:
        for key, table in tables.items():
            shutil.copytree(FIXTURE, locations[key])
            uctl("create", SCHEMA, table, COLUMNS)

        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_NULL_TABLE": tables["null"],
            "UC_TEST_NULL_LOCATION": str(locations["null"]),
            "UC_TEST_ENC_TABLE": tables["enc"],
            "UC_TEST_ENC_LOCATION": str(locations["enc"]),
        }
        run_paired(request, env=env)
    finally:
        for key, table in tables.items():
            uctl("drop", SCHEMA, table, check=False)
            shutil.rmtree(locations[key], ignore_errors=True)
