"""Shared read/write driver for rw.test (same-stem pairing).

The data behavior (write -> read-back -> metadata) is IDENTICAL for catalog-managed
(managed storage -> duck.cmt) and plain (external storage -> duck.plain) tables -- that's
the invariant. So ONE body (rw.test, keyed on ${UC_TEST_SCHEMA}/${UC_TEST_TABLE}) covers
both, fanned out declaratively by @requires_matrix over the commit axis instead of an
imperative parametrize + manual `uctl` seeding. The table-type-specific protocol
assertions (does reading call LoadTable / writing call UpdateTable?) stay isolated in
table-cmt/ and table-plain/.

Each cell is an independent pytest item (test_rw[cmt] / test_rw[plain]) carrying
its own @requires; the OSS provisioner instantiates a unique empty `id_name_rw_<token>`
in the cell's schema (cmt / plain) and injects
UC_TEST_CATALOG/SCHEMA/TABLE into resources.env. Server-only resource (uc_server fixture,
from oss_local/conftest.py); the `oss_local` mark tags the cells for `-m oss_local`.
"""

from ducktest import TableSpec, requires_matrix, run_paired


@requires_matrix(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": ["cmt", "plain"]},
    marks=["oss_local"],
)
def test_rw(request, uc_server, resources):
    run_paired(request, env=resources.env)
