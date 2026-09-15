"""Driver for variant.test (same-stem pairing -> one test).

@requires provisions a unique empty duck.cmt.{UC_TEST_TABLE} from the `id_variant`
fixture (id INTEGER, data VARIANT); the body asserts the column type and round-trips every
variant value shape through the Delta write/read path.

Not matrixed over the commit axis (cf. rw.py): the variant type does not depend on whether the
catalog or the filesystem owns commits.
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_variant").Seed(None),
    access="rw",
    properties={"commit": "cmt", "storage": "managed"},
)
def test_variant(request, uc_server, resources):
    run_paired(request, env=resources.env)
