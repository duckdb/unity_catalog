"""Driver for table-plain/checkpoint.test (same-stem pairing -> one test).

Ports the legacy oss_local/todo/checkpoint.test to the OSS UC "ducktest" container,
retargeted at duck.plain (plain Delta). @requires provisions a unique empty EXTERNAL
table (duck.plain.${UC_TEST_TABLE}); EXTERNAL tables store their log at
<data_dir>/duck/plain/<table>/_delta_log/..., so the body's final glob assertion
(parse_path(file)[-3] == ${UC_TEST_TABLE}) holds; MANAGED tables nest under
__unitystorage/<uuid>/ and would break that. Injects the container's host bind-mount
dir as ${UC_TEST_DATA} so the body's glob resolves; depends on uc_server (session
container) AND resources (the per-test table, dropped on teardown).
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "plain", "storage": "external"},
)
def test_checkpoint(request, uc_server, resources):
    run_paired(request, env={**resources.env, "UC_TEST_DATA": uc_server.data_dir})
