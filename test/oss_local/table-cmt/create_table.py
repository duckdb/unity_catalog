"""Driver for create_table.test (same-stem pairing -> one test).

Nothing is provisioned for the body: it CREATEs the table itself, so `@requires` would be
the wrong tool -- that instantiates a table. The driver picks a name no earlier run used,
because DROP TABLE is not supported yet and a fixed name would collide with the run before,
and deletes the tables afterwards through UC's REST API so a long-lived server does not fill
up with them.

`UC_ENDPOINT` is where the server answers. It defaults to the endpoint of the server the
fixture provisioned or attached to (--existing-service); a run that reaches that server by another
address (a dev container talking to a server on its host, say) sets it, and both the body and the
cleanup below follow.
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


def _partition_indexes(endpoint, full_name):
    with urllib.request.urlopen(f"{endpoint}/api/2.1/unity-catalog/tables/{full_name}", timeout=10) as response:
        return {c["name"]: c.get("partition_index") for c in json.load(response)["columns"]}


def test_create_table(request, uc_server):
    endpoint = os.environ.get("UC_ENDPOINT", uc_server.endpoint)
    catalog = os.environ.get("UC_TEST_CATALOG", "duck")
    table = f"created_by_duckdb_{uuid.uuid4().hex[:8]}"
    nested = f"created_by_duckdb_nested_{uuid.uuid4().hex[:8]}"
    partitioned = f"created_by_duckdb_partitioned_{uuid.uuid4().hex[:8]}"
    try:
        run_paired(
            request,
            env={
                "UC_TEST_NEW_TABLE": table,
                "UC_TEST_NEW_NESTED_TABLE": nested,
                "UC_TEST_NEW_PARTITIONED_TABLE": partitioned,
                "UC_TEST_CATALOG": catalog,
                "UC_ENDPOINT": endpoint,
            },
        )
        # The partition column is registered as one, which no query through DuckDB shows
        assert _partition_indexes(endpoint, f"{catalog}.cmt.{partitioned}") == {"id": None, "p": 0}
    finally:
        for name in (table, nested, partitioned, f"{table}_rolledback"):
            _delete_table(endpoint, f"{catalog}.cmt.{name}")
