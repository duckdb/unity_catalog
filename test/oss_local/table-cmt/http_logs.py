"""Driver for table-cmt/http_logs.test (same-stem pairing -> one test).

Declarative: @requires provisions a unique empty catalog-managed table
(duck.cmt.${UC_TEST_TABLE}) and the body exercises HTTP prefetch logging
(enable_logging('HTTP') + duckdb_logs_parsed('HTTP')). Ports the old
local_oss_unity_catalog/http_logs.test. Depends on uc_server (session container) AND
resources (the per-test table, dropped on teardown).
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_http_logs(request, uc_server, resources):
    run_paired(request, env=resources.env)
