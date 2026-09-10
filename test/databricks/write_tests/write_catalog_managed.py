"""Driver for write_catalog_managed.test -- seed id_name into a cmt x managed cell and write.

@requires(access="rw") seeds `id_name` into an isolated `cmt__managed__<token>` cell
(catalog-managed commit protocol, UC-managed storage): the provisioner create+inserts the
fixture, injects CATALOG/SCHEMA into the body (run_paired env=...), and drops the cell
on teardown. The body exercises the staged-commit + backfill (delta.yaml v1) protocol. The
same @requires + provisioner also drive `pytest --repl`.
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name"),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_write_catalog_managed(request, resources):
    run_paired(request, env=resources.env)
