"""Driver for writer_statement_mapped.test: an insert that omits a column added after creation.

The column added in the log's second commit is the one an insert is most likely to leave out, and
the one whose physical name a writer reading the creation schema would miss. Column mapping is what
makes that expensive: a file written under logical names reads back NULL in name mode.

The fixture is a committed Delta log rather than a TableSpec -- the mapping lives in field metadata
that neither `uctl create` nor the duckdb middleman can express -- and is copied, so the INSERT never
mutates the committed tree.
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import REPO_ROOT, plain_table_location, uctl

CATALOG = "duck"
SCHEMA = "plain"  # EXTERNAL -> uctl gives the table a location we can stage into
COLUMNS = "id INT, name STRING, age INT"
FIXTURE = REPO_ROOT / "data" / "writer_statements_mapped"


@pytest.mark.oss_local
def test_writer_statement_mapped(request, uc_server):
    token = uuid.uuid4().hex[:8]
    table = f"writer_statements_mapped_{token}"
    location = plain_table_location(uc_server, table)

    try:
        shutil.copytree(FIXTURE, location)
        uctl("create", SCHEMA, table, COLUMNS)

        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": table,
            "UC_TEST_TABLE_LOCATION": str(location),
        }
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, table, check=False)
        shutil.rmtree(location, ignore_errors=True)
