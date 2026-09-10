"""Driver for column_mapping_write.test: an INSERT into a column-mapped table through the UC catalog path.

A write through `ATTACH … (TYPE unity_catalog)` plans its insert down a different branch than a
plain `ATTACH … (TYPE delta)`. Column mapping is what makes that branch worth pinning: it decides
which schema the parquet copy is bound to, and a file written under logical names is unreadable in
id mode and reads back NULL in name mode.

The fixture is a committed Delta log rather than a TableSpec, and has to be: the mapping lives in
Delta field metadata (`delta.columnMapping.physicalName` / `.id`) that neither `uctl create` nor the
duckdb middleman can express. It is copied, so the INSERT never mutates the committed tree.

Both modes drive one body: every assertion is mode-independent -- what DuckDB emits is the same
either way, only the reader's resolution rule differs. The control table is created per mode too,
so the mapped and unmapped writes always come from the same ATTACH.
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import REPO_ROOT, plain_table_location, uctl

CATALOG = "duck"
SCHEMA = "plain"  # EXTERNAL -> uctl gives the table a location we can stage into
COLUMNS = "id INT, code STRING"
FIXTURES = REPO_ROOT / "data" / "column_mapping"


@pytest.mark.oss_local
@pytest.mark.parametrize("mode", ["name", "id"])
def test_column_mapping_write(request, uc_server, mode):
    # Unique per run: a container shared across sessions (--existing-service) would otherwise
    # collide on the table names and their storage locations.
    token = uuid.uuid4().hex[:8]
    mapped = f"column_mapping_{mode}_{token}"
    unmapped = f"column_mapping_none_{token}"
    mapped_location = plain_table_location(uc_server, mapped)
    unmapped_location = plain_table_location(uc_server, unmapped)

    try:
        shutil.copytree(FIXTURES / f"{mode}_mode", mapped_location)
        uctl("create", SCHEMA, mapped, COLUMNS)

        # The control gets no staged log -- an unmapped table of the same shape, written through the
        # same catalog attach, is what makes the mapped result mean anything.
        uctl("create", SCHEMA, unmapped, COLUMNS)

        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": mapped,
            "UC_TEST_TABLE_LOCATION": str(mapped_location),
            "UC_TEST_UNMAPPED_TABLE": unmapped,
            "UC_TEST_UNMAPPED_LOCATION": str(unmapped_location),
        }
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, mapped, check=False)
        uctl("drop", SCHEMA, unmapped, check=False)
        shutil.rmtree(mapped_location, ignore_errors=True)
        shutil.rmtree(unmapped_location, ignore_errors=True)
