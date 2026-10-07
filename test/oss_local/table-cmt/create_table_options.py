"""Driver for create_table_options.test (same-stem pairing -> one test).

Every case is refused during bind or before the first REST call, so nothing is created and
nothing needs provisioning -- but an attached catalog is still a precondition, hence uc_server.

`UC_ENDPOINT` is where the server answers. It defaults to the address the other bodies hardcode;
a run from somewhere that reaches the container by another address sets it instead. Both it and the
catalog are passed in explicitly: only a test that provisions resources exports them, so a body
relying on that would skip whenever it runs before one.
"""

import os

from ducktest import run_paired

DEFAULT_ENDPOINT = "http://127.0.0.1:8080"
DEFAULT_CATALOG = "duck"


def test_create_table_options(request, uc_server):
    run_paired(
        request,
        env={
            "UC_TEST_CATALOG": os.environ.get("UC_TEST_CATALOG", DEFAULT_CATALOG),
            "UC_ENDPOINT": os.environ.get("UC_ENDPOINT", DEFAULT_ENDPOINT),
        },
    )
