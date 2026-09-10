"""Driver for table-plain/catalog_managed.test (same-stem pairing -> one test).

The plain half of the catalog-managed PROTOCOL contrast: @requires provisions a unique
empty EXTERNAL table (duck.plain.${UC_TEST_TABLE}) and the body asserts reading does
NOT call LoadTable and writing does NOT call UpdateTable (vs table-cmt/catalog_managed,
which asserts they ARE called). The shared data round-trip is in oss_local/rw.test.
Depends on uc_server (session container) AND resources (the per-test table).
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "plain", "storage": "external"},
)
def test_catalog_managed(request, uc_server, resources):
    run_paired(request, env=resources.env)
