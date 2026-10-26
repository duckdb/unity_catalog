"""Driver for column_mapping.test: catalog-managed tables with column mapping, which a managed CREATE
makes by taking the mapping mode UC suggests.

The names are per run, because DROP TABLE is not supported yet, and the tables are deleted through
UC's REST API afterwards. `UC_ENDPOINT` is where the server answers, as in create_table.py.
"""

import json
import os
import urllib.error
import urllib.request
import uuid

from ducktest import run_paired


def _delete_table(endpoint, full_name):
    """Best effort: a table left behind is untidy, not a test failure."""
    request = urllib.request.Request(f"{endpoint}/api/2.1/unity-catalog/tables/{full_name}", method="DELETE")
    try:
        urllib.request.urlopen(request, timeout=10).read()
    except urllib.error.URLError:
        pass


def _properties(endpoint, full_name):
    with urllib.request.urlopen(f"{endpoint}/api/2.1/unity-catalog/tables/{full_name}", timeout=10) as response:
        return json.load(response).get("properties") or {}


def test_column_mapping(request, uc_server):
    endpoint = os.environ.get("UC_ENDPOINT", uc_server.endpoint)
    catalog = os.environ.get("UC_TEST_CATALOG", "duck")
    token = uuid.uuid4().hex[:8]
    tables = {
        "UC_TEST_MAPPED_TABLE": f"mapped_{token}",
        "UC_TEST_PARTITIONED_TABLE": f"mapped_partitioned_{token}",
        "UC_TEST_NESTED_TABLE": f"mapped_nested_{token}",
    }
    try:
        run_paired(request, env={**tables, "UC_TEST_CATALOG": catalog, "UC_ENDPOINT": endpoint})
        # UC registers the mode the log committed, which no query through DuckDB shows
        modes = {
            name: _properties(endpoint, f"{catalog}.cmt.{name}").get("delta.columnMapping.mode")
            for name in tables.values()
        }
        assert modes == {
            tables["UC_TEST_MAPPED_TABLE"]: "name",
            tables["UC_TEST_PARTITIONED_TABLE"]: "name",
            tables["UC_TEST_NESTED_TABLE"]: "name",
        }
    finally:
        for name in tables.values():
            _delete_table(endpoint, f"{catalog}.cmt.{name}")
