"""Driver for table-cmt/create_table_as.test (same-stem pairing -> one test).

The body creates its managed tables itself, under names no earlier run used (DROP TABLE is not supported
yet). Afterwards the driver checks what UC registered -- MANAGED, the partition column, a version 0 at the
staged location that holds the rows and the catalog-managed protocol -- which no query through DuckDB
shows, and deletes the tables over REST.
"""

import json
import os
import urllib.error
import urllib.request
import uuid
from pathlib import Path

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


def _version_zero(storage_location):
    log = Path(storage_location.removeprefix("file://")) / "_delta_log" / "00000000000000000000.json"
    return [json.loads(line) for line in log.read_text().splitlines()]


def test_create_managed_table_as(request, uc_server):
    endpoint = os.environ.get("UC_ENDPOINT", uc_server.endpoint)
    catalog = os.environ.get("UC_TEST_CATALOG", "duck")
    table = f"ctas_by_duckdb_{uuid.uuid4().hex[:8]}"
    names = [table, f"{table}_pn", f"{table}_empty", f"{table}_type", f"{table}_replace"]
    try:
        run_paired(request, env={"UC_TEST_CATALOG": catalog, "UC_ENDPOINT": endpoint, "UC_TEST_NEW_TABLE": table})
        for name in (table, f"{table}_pn", f"{table}_empty"):
            info = _get_table(endpoint, f"{catalog}.cmt.{name}")
            assert (info["table_type"], info["data_source_format"]) == ("MANAGED", "DELTA"), info
            actions = _version_zero(info["storage_location"])
            protocol = next(a["protocol"] for a in actions if "protocol" in a)
            assert "catalogManaged" in protocol["readerFeatures"], protocol
            metadata = next(a["metaData"] for a in actions if "metaData" in a)
            assert metadata["configuration"]["io.unitycatalog.tableId"] == info["table_id"], metadata
        actions = _version_zero(_get_table(endpoint, f"{catalog}.cmt.{table}")["storage_location"])
        rows = sum(json.loads(a["add"]["stats"])["numRecords"] for a in actions if "add" in a)
        assert rows == 3, actions
        partitions = {
            c["name"]: c.get("partition_index") for c in _get_table(endpoint, f"{catalog}.cmt.{table}_pn")["columns"]
        }
        assert partitions == {"id": None, "p": 0, "s": None}, partitions
    finally:
        for name in names:
            _delete_table(endpoint, f"{catalog}.cmt.{name}")
