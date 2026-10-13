"""Driver for catalog_managed.test (same-stem pairing -> one test).

Declarative, mirroring the Databricks path: @requires(TableSpec("id_name").Seed(None))
asks the generic `resources` fixture -> OssProvisioner to instantiate a unique per-test
EMPTY table `id_name_rw_<token>` in the cmt (catalog-managed) schema, and injects its
name into the body as ${UC_TEST_TABLE}. `.Seed(None)` because the body does its own
inserts. Depends on uc_server (the session container); the per-test table is dropped on
teardown.
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_catalog_managed(request, uc_server, resources):
    run_paired(request, env=resources.env)
