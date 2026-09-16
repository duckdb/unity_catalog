"""Driver for transaction_history.test (same-stem pairing).

The same body runs against both table types. Writing to a catalog-managed table makes every reader
re-open that table's Delta catalog, and re-opening it while a transaction is already reading the
table is what failed; writing to a plain table does not, so that cell is the control.
"""

from ducktest import TableSpec, requires_matrix, run_paired


@requires_matrix(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": ["cmt", "plain"]},
    marks=["oss_local"],
)
def test_transaction_history(request, uc_server, resources):
    run_paired(request, env=resources.env)
