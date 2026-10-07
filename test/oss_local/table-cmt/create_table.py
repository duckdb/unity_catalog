"""Driver for create_table.test (same-stem pairing -> one test).

Nothing is provisioned for the body: it CREATEs the table itself, so `@requires` would be
the wrong tool -- that instantiates a table. The driver picks a name no earlier run used,
because DROP TABLE is not supported yet and a fixed name would collide with the run before,
and deletes the tables afterwards through UC's REST API so a long-lived server does not fill
up with them.

`UC_ENDPOINT` is where the server answers. It defaults to the address the other bodies
hardcode; a run from somewhere that reaches the container by another address (a dev
container talking to a server on its host, say) sets it and both the body and the cleanup
below follow.
"""

import os
import urllib.error
import urllib.request
import uuid

from ducktest import run_paired

DEFAULT_ENDPOINT = "http://127.0.0.1:8080"


def _delete_table(endpoint, full_name):
    """Best effort: a table left behind is untidy, not a test failure."""
    request = urllib.request.Request(f"{endpoint}/api/2.1/unity-catalog/tables/{full_name}", method="DELETE")
    try:
        urllib.request.urlopen(request, timeout=10).read()
    except urllib.error.URLError:
        pass


def test_create_table(request, uc_server):
    endpoint = os.environ.get("UC_ENDPOINT", DEFAULT_ENDPOINT)
    catalog = os.environ.get("UC_TEST_CATALOG", "duck")
    table = f"created_by_duckdb_{uuid.uuid4().hex[:8]}"
    nested = f"created_by_duckdb_nested_{uuid.uuid4().hex[:8]}"
    try:
        run_paired(
            request,
            env={
                "UC_TEST_NEW_TABLE": table,
                "UC_TEST_NEW_NESTED_TABLE": nested,
                "UC_TEST_CATALOG": catalog,
                "UC_ENDPOINT": endpoint,
            },
        )
    finally:
        for name in (table, nested):
            _delete_table(endpoint, f"{catalog}.cmt.{name}")
