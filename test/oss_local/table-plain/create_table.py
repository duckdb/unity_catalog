"""Driver for table-plain/create_table.test (same-stem pairing -> one test).

The body creates its tables itself, under a location root this driver makes fresh per run. Afterwards
the driver checks what UC registered -- table type, location, protocol, partition columns -- which no
query through DuckDB can see, and deletes the tables over REST: DROP TABLE is not supported yet.
DELETE removes an external table's registration only, so its files go with the temp dir.
"""

import json
import os
import urllib.error
import urllib.request
import uuid

from ducktest import run_paired


def _get_table(endpoint, full_name):
    with urllib.request.urlopen(f"{endpoint}/api/2.1/unity-catalog/tables/{full_name}", timeout=10) as response:
        return json.load(response)


def _delete_table(endpoint, full_name):
    """Best effort: a table left behind is untidy, not a test failure."""
    request = urllib.request.Request(f"{endpoint}/api/2.1/unity-catalog/tables/{full_name}", method="DELETE")
    try:
        urllib.request.urlopen(request, timeout=10).read()
    except urllib.error.URLError:
        pass


def test_create_external_table(request, uc_server, tmp_path):
    endpoint = os.environ.get("UC_ENDPOINT", uc_server.endpoint)
    catalog = os.environ.get("UC_TEST_CATALOG", "duck")
    table = f"created_by_duckdb_ext_{uuid.uuid4().hex[:8]}"
    created = {
        f"{catalog}.plain.{table}": tmp_path / table,
        f"{catalog}.plain.{table}_part": tmp_path / f"{table}_part",
        f"{catalog}.cmt.{table}_ext": tmp_path / f"{table}_ext",
    }
    # Its location holds a uuid only the body knows
    unchecked = [f"{catalog}.plain.{table}_uuid"]
    try:
        run_paired(
            request,
            env={
                "UC_TEST_CATALOG": catalog,
                "UC_ENDPOINT": endpoint,
                "UC_TEST_NEW_TABLE": table,
                "UC_TEST_LOCATION": str(tmp_path),
            },
        )
        for full_name, location in created.items():
            info = _get_table(endpoint, full_name)
            assert (info["table_type"], info["data_source_format"]) == ("EXTERNAL", "DELTA"), full_name
            assert info["storage_location"] == f"file://{location}", full_name
            # A table whose catalog entry claims catalogManaged would be read as one, and fail to load
            assert "delta.feature.catalogManaged" not in info.get("properties", {}), full_name
        partitions = {
            c["name"]: c.get("partition_index")
            for c in _get_table(endpoint, f"{catalog}.plain.{table}_part")["columns"]
        }
        assert partitions == {"id": None, "p": 0}
    finally:
        for full_name in [*created, *unchecked]:
            _delete_table(endpoint, full_name)
