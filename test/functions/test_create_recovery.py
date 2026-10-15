"""A CREATE whose registration reports failure, though it may have gone through.

UC's managed-tables protocol settles an unclear outcome by loading the name and comparing its
table-uuid with the id the table was staged under. These cases make the catalog fail the register
call in each way that leaves the outcome unclear -- a 5xx after the write, a connection closed
before the reply -- and check that a matching uuid counts as created while anything else surfaces
the error the register call raised.
"""

import json

import pytest

from uc.mock import DROP, MockUnityCatalog, run

STAGED_ID = "00000000-0000-0000-0000-0000000000aa"
TABLE = "recovered"
STAGING = "/delta/v1/catalogs/duck/schemas/cmt/staging-tables"
REGISTER = "/delta/v1/catalogs/duck/schemas/cmt/tables"
LOAD = f"/delta/v1/catalogs/duck/schemas/cmt/tables/{TABLE}"
REGISTER_FAILED = {"error": {"message": "registration blew up", "type": "InternalError", "code": 500}}


def _staged(location):
    # What OSS UC answers for a managed table, minus the suggestions: a catalog-managed protocol and the
    # properties that go with it.
    features = ["catalogManaged", "v2Checkpoint", "vacuumProtocolCheck", "deletionVectors"]
    return {
        "table-id": STAGED_ID,
        "table-type": "MANAGED",
        "location": f"file://{location}",
        "required-protocol": {
            "min-reader-version": 3,
            "min-writer-version": 7,
            "reader-features": features,
            "writer-features": features + ["inCommitTimestamp"],
        },
        "required-properties": {
            "delta.enableInCommitTimestamps": "true",
            "delta.checkpointPolicy": "v2",
            "delta.enableDeletionVectors": "true",
            "io.unitycatalog.tableId": STAGED_ID,
        },
        "suggested-properties": {},
    }


def _loaded(table_uuid):
    return {"metadata": {"etag": "e1", "table-uuid": table_uuid}, "commits": [], "latest-table-version": 0}


def _catalog(location, register, load):
    def respond(method, path, body):
        if method == "POST" and path.endswith(STAGING):
            return 200, _staged(location)
        if method == "POST" and path.endswith(REGISTER):
            # The register call must describe the table its version 0 committed
            assert json.loads(body)["location"] == f"file://{location}"
            return register
        if method == "GET" and path.endswith(LOAD):
            return load
        return None

    return MockUnityCatalog([], schemas=["cmt"], respond=respond)


@pytest.mark.parametrize(
    "register",
    [(500, REGISTER_FAILED), DROP],
    ids=["server-error-after-register", "connection-closed-after-register"],
)
def test_a_registered_table_counts_as_created(tmp_path, register):
    with _catalog(tmp_path / TABLE, register, (200, _loaded(STAGED_ID))) as uc:
        result, _ = run(uc, f"CREATE TABLE unity.cmt.{TABLE} (id INTEGER);")
        assert result.returncode == 0, result.stderr
        assert any(p.endswith(LOAD) for p in uc.requests), uc.requests


@pytest.mark.parametrize(
    "load",
    [
        (404, {"error": {"message": f"Table not found: {TABLE}", "type": "NoSuchTableException", "code": 404}}),
        (200, _loaded("00000000-0000-0000-0000-0000000000bb")),
    ],
    ids=["not-registered", "registered-under-another-id"],
)
def test_otherwise_the_register_error_is_reported(tmp_path, load):
    with _catalog(tmp_path / TABLE, (500, REGISTER_FAILED), load) as uc:
        result, _ = run(uc, f"CREATE TABLE unity.cmt.{TABLE} (id INTEGER);")
        assert result.returncode != 0
        # The register call's own error, not the lookup made after it
        assert f"POST to '{uc.endpoint}/api/2.1/unity-catalog{REGISTER}'" in result.stderr, result.stderr
        assert "not found" not in result.stderr.lower(), result.stderr
