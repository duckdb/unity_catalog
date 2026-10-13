"""Driver for attach.test -- ATTACH/DETACH/USE mechanics for the unity_catalog extension.

Merged from the former basic.test + aliases.test (near-duplicates: same flow, differing only
in the secret TYPE spelling). @requires(access="rw") seeds `id_name` into an isolated cell
(plain/managed) so the body exercises the attach/detach/use flow AND the `TYPE UC` secret-type
alias against a real attached table; the cell is dropped on teardown.
"""

from ducktest import TableSpec, requires, run_paired


@requires(
    source=TableSpec("id_name"),
    access="rw",
    properties={"commit": "plain", "storage": "managed"},
)
def test_attach(request, resources):
    run_paired(request, env=resources.env)
