"""Driver for variant.test: VARIANT columns written and read through the UC catalog path.

The fixture is a committed Delta log rather than a TableSpec, and has to be: duckdb-delta refuses
`CREATE TABLE` with a VARIANT column, so the column can only reach the log some other way. Inserts
into a table that already declares one work, which is what the body does. We copy `data/variant`
into the table's storage location (a copy, so the INSERTs never mutate the committed tree) and
register that location with UC.

Not matrixed over the commit axis (cf. rw.py): the variant type does not depend on whether the
catalog or the filesystem owns commits.
"""

import os
import pathlib
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import plain_table_location, uctl

CATALOG = "duck"
SCHEMA = "plain"  # EXTERNAL -> uctl gives the table a location we can stage into
FIXTURE = pathlib.Path(__file__).resolve().parents[2] / "data" / "variant"


@pytest.mark.oss_local
def test_variant(request, uc_server):
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table name and its storage location.
    table = f"variant_{uuid.uuid4().hex[:8]}"
    location = plain_table_location(uc_server, table, CATALOG, SCHEMA)
    shutil.copytree(FIXTURE, location)

    uctl("create", SCHEMA, table, "id INT, data VARIANT")

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
