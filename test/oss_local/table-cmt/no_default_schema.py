"""Driver for no_default_schema.test (same-stem pairing -> one test).

OSS UC exposes no default namespace, so a catalog attached WITHOUT DEFAULT_SCHEMA has no
default schema. A catalog-managed table still reads and writes, fully qualified: delta looks
its commit function up in the table's own schema (duckdb/duckdb-delta#421), not in `main`,
which UC maps to the missing default (#150). @requires provisions a unique empty
duck.cmt.${UC_TEST_TABLE}. Depends on uc_server (session container) AND resources.
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_no_default_schema(request, uc_server, resources):
    run_paired(request, env=resources.env)
