"""Driver for table-cmt/checkpoint.test (same-stem pairing -> one test).

Declarative CMT counterpart of table-plain/checkpoint.py: @requires provisions a unique
empty catalog-managed table (duck.cmt.${UC_TEST_TABLE}) and the body exercises
unity_catalog_checkpoint_table's name-resolution forms against it, advancing a version
between each. MANAGED tables nest their log under
<data_dir>/duck/cmt/__unitystorage/<uuid>/_delta_log/..., so the body's final assertion
scopes the glob to duck/cmt/ and COUNTs (it cannot filter on the table name --
parse_path(file)[-3] is a UUID). Injects the container's host bind-mount dir as
${UC_TEST_DATA} so the body's glob resolves; depends on uc_server (session container)
AND resources (the per-test table, dropped on teardown).
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_checkpoint(request, uc_server, resources):
    run_paired(request, env={**resources.env, "UC_TEST_DATA": uc_server.data_dir})
