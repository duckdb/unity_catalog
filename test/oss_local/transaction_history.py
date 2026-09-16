"""Driver for transaction_history.test (same-stem pairing).

Matrixed over the commit axis: a catalog-managed commit marks the table's attached Delta catalog for
reattachment, which a plain commit does not, so the plain cell is the control for the cmt one.
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
