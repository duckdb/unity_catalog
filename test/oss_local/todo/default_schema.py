"""Driver for default_schema.test (negative test, same-stem pairing).

OSS UC exposes no default namespace, so attaching a UC catalog WITHOUT DEFAULT_SCHEMA
leaves auto-detection unable to resolve one; accessing a table through the catalog's
implicit default schema then errors. Kept until default-schema auto-detection is
supported (then flip to a positive no-DEFAULT_SCHEMA case). @requires provisions a unique
empty duck.cmt.${UC_TEST_TABLE} so the failure is unambiguously the default-schema
resolution, not a missing table. Depends on uc_server (session container) AND resources.
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_default_schema(request, uc_server, resources):
    run_paired(request, env=resources.env)
