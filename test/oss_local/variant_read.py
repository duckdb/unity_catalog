"""Driver for variant_read.test: Spark-written VARIANT columns read through the UC catalog path.

Runs once per fixture (data/variant_unshredded, data/variant_shredded); the body holds for both,
since shredding changes the files and not the values. The table is registered over REST because
`uctl` cannot state a nested type, and carries the properties Spark reports for it.
"""

import os
import shutil
import uuid

import pytest

from ducktest import run_paired
from uc import plain_table_location, register_external_table, uctl
from uc.server import ENDPOINT
from uc.variant_fixtures import COLUMNS, FIXTURES

CATALOG = "duck"
SCHEMA = "plain"


@pytest.mark.oss_local
@pytest.mark.parametrize("fixture", sorted(FIXTURES))
def test_variant_read(request, uc_server, fixture):
    source, properties = FIXTURES[fixture]
    table = f"variant_{fixture}_{uuid.uuid4().hex[:8]}"
    location = plain_table_location(uc_server, table)

    try:
        shutil.copytree(source, location)
        register_external_table(ENDPOINT, table, location, COLUMNS, properties)
        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": table,
        }
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, table, check=False)
        shutil.rmtree(location, ignore_errors=True)
