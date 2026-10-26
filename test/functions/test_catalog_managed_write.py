"""A write into a catalog-managed table commits through Unity Catalog.

UC attaches each table's Delta catalog itself and hands it the options that make it a child of UC:
the table's name, and for a catalog-managed table the parent catalog that commits for it. A Delta
catalog that missed them would look for the wrong table, or commit straight into `_delta_log`
behind UC's back. So the commit has to arrive at UC, and the log must not hold it yet.

The table is made by a managed CREATE against a mocked staging and registration, then served as
the catalog-managed table UC would list.
"""

import json

from uc.mock import MockUnityCatalog, run

TABLE_ID = "00000000-0000-0000-0000-0000000000dd"
TABLES = "/delta/v1/catalogs/duck/schemas/cmt/tables"
TABLE = f"{TABLES}/managed"
STAGING = "/delta/v1/catalogs/duck/schemas/cmt/staging-tables"
FEATURES = ["catalogManaged", "v2Checkpoint", "vacuumProtocolCheck", "deletionVectors"]


def _staged(location):
    return {
        "table-id": TABLE_ID,
        "table-type": "MANAGED",
        "location": f"file://{location}",
        "required-protocol": {
            "min-reader-version": 3,
            "min-writer-version": 7,
            "reader-features": FEATURES,
            "writer-features": FEATURES + ["inCommitTimestamp"],
        },
        "required-properties": {
            "delta.enableInCommitTimestamps": "true",
            "delta.checkpointPolicy": "v2",
            "delta.enableDeletionVectors": "true",
            "io.unitycatalog.tableId": TABLE_ID,
        },
        "suggested-properties": {},
    }


def _listed(location):
    return {
        "name": "managed",
        "catalog_name": "duck",
        "schema_name": "cmt",
        "table_type": "MANAGED",
        "data_source_format": "DELTA",
        "storage_location": f"file://{location}",
        "table_id": TABLE_ID,
        "properties": {f"delta.feature.{f}": "supported" for f in FEATURES},
        "columns": [
            {
                "name": "id",
                "type_text": "int",
                "type_name": "INT",
                "position": 0,
                "nullable": True,
                "type_json": '{"name":"id","type":"integer","nullable":true,"metadata":{}}',
            }
        ],
    }


def test_an_insert_into_a_catalog_managed_table_commits_through_the_catalog(tmp_path):
    location = tmp_path / "managed"

    def create(method, path, body):
        if method == "POST" and path.endswith(STAGING):
            return 200, _staged(location)
        if method == "POST" and path.endswith(TABLES):
            return 200, {}
        return None

    with MockUnityCatalog([], schemas=["cmt"], respond=create) as uc:
        result, _ = run(uc, "CREATE TABLE unity.cmt.managed (id INTEGER);")
    assert result.returncode == 0, result.stderr

    commits = []

    def write(method, path, body):
        if not path.endswith(TABLE):
            return None
        if method == "GET":
            return 200, {"metadata": {"etag": "e1", "table-uuid": TABLE_ID}, "commits": [], "latest-table-version": 0}
        commits.append(json.loads(body))
        return 200, {"metadata": {"etag": "e2"}}

    with MockUnityCatalog([_listed(location)], respond=write) as uc:
        result, _ = run(uc, "INSERT INTO unity.cmt.managed VALUES (1);")
    assert result.returncode == 0, result.stderr

    (commit,) = commits
    assert [u["commit"]["version"] for u in commit["updates"] if u["action"] == "add-commit"] == [1], commit
    assert {"type": "assert-table-uuid", "uuid": TABLE_ID} in commit["requirements"], commit
    # Staged for the catalog, not published: UC backfills version 1 itself
    assert list(location.glob("_delta_log/_staged_commits/00000000000000000001.*.json")), list(location.rglob("*"))
    assert not (location / "_delta_log" / "00000000000000000001.json").exists()
