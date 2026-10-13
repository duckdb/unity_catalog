"""Driver for attach.test (same-stem pairing -> one test).

Declarative: @requires provisions a unique empty catalog-managed table
(duck.cmt.${UC_TEST_TABLE}); the body exercises attach/detach/USE semantics and
writes+reads a row through it. Depends on uc_server (session container) AND resources
(the per-test table, dropped on teardown).
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_attach(request, uc_server, resources):
    run_paired(request, env=resources.env)
