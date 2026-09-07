"""Driver for writer_statement_shapes.test: inserts whose shape does not match the table.

`BY NAME`, an explicit column list, an omitted column and `DEFAULT VALUES` all reach the writer
through a plan the extension did not build itself. Every write test here so far inserts positionally,
so nothing covered them through the catalog -- and a shape that arrives in the wrong order lands
values in the wrong columns without failing.

Fanned out over both commit paths, since the catalog-managed one plans its insert differently.
"""

from ducktest import TableSpec, requires_matrix, run_paired


@requires_matrix(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": ["cmt", "plain"]},
    marks=["oss_local"],
)
def test_writer_statement_shapes(request, uc_server, resources):
    run_paired(request, env=resources.env)
