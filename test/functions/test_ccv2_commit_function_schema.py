"""A catalog-managed table reads and writes without a usable default schema (unity_catalog#150, #111).

Each table's Delta catalog commits through `__internal_delta_ccv2_commit_staged`, a table function
UC serves in every schema of its catalog. Delta looks it up in `parent_catalog_schema`, which UC sets
to the table's own schema (duckdb/duckdb-delta#421). A lookup in `main` would fail here: UC maps `main`
to the attach's default schema, and this catalog has none (no DEFAULT_SCHEMA and no default-namespace
setting, #150), or a DEFAULT_SCHEMA that names no schema (#111).

The catalog has a single schema, `cmt`, and no `default`. The settings endpoint answers 404, as on a
workspace without the setting. The table is made by a managed CREATE against a mocked staging and
registration (with a valid DEFAULT_SCHEMA; the CREATE is set-up), then served as the catalog-managed
table UC would list. The mock keeps the commits UC registers, so a read after the INSERT learns of
the staged commit only through UC.
"""

import json

import pytest
from uc.mock import NO_DEFAULT_SCHEMA, MockUnityCatalog, run

TABLE_ID = "00000000-0000-0000-0000-0000000000cc"
SCHEMA = "cmt"
TABLES = f"/delta/v1/catalogs/duck/schemas/{SCHEMA}/tables"
TABLE = f"{TABLES}/managed"
STAGING = f"/delta/v1/catalogs/duck/schemas/{SCHEMA}/staging-tables"
SETTINGS = "/api/2.0/settings/types/default_namespace_ws/names/default"
FEATURES = ["catalogManaged", "v2Checkpoint", "vacuumProtocolCheck", "deletionVectors"]
NO_SETTING = (404, {"error_code": "RESOURCE_DOES_NOT_EXIST", "message": "setting default_namespace_ws not found"})


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
        "schema_name": SCHEMA,
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


def _create(location):
    def respond(method, path, body):
        if method == "POST" and path.endswith(STAGING):
            return 200, _staged(location)
        if method == "POST" and path.endswith(TABLES):
            return 200, {}
        return None

    with MockUnityCatalog([], schemas=[SCHEMA], respond=respond) as uc:
        result, _ = run(uc, f"CREATE TABLE unity.{SCHEMA}.managed (id INTEGER);")
    assert result.returncode == 0, result.stderr


class CatalogManagedTable:
    """Answers the table's Delta API like UC: GET lists the commits registered so far, POST registers one."""

    def __init__(self):
        self.commits = []

    def __call__(self, method, path, body):
        if path.endswith(SETTINGS):
            return NO_SETTING
        if not path.endswith(TABLE):
            return None
        if method == "GET":
            version = self.commits[-1]["version"] if self.commits else 0
            return 200, {
                "metadata": {"etag": f"e{version}", "table-uuid": TABLE_ID},
                "commits": self.commits,
                "latest-table-version": version,
            }
        update = json.loads(body)
        self.commits += [u["commit"] for u in update["updates"] if u["action"] == "add-commit"]
        return 200, {"metadata": {"etag": f"e{self.commits[-1]['version']}"}}


@pytest.mark.parametrize(
    "default_schema",
    [
        # The baseline: works with or without the fix, so a failure of the two below is the lookup
        pytest.param(SCHEMA, id="valid-default-schema"),
        pytest.param(NO_DEFAULT_SCHEMA, id="no-default-schema-uc150"),
        pytest.param("does_not_exist", id="wrong-default-schema-uc111"),
    ],
)
def test_a_catalog_managed_table_reads_and_writes_without_a_usable_default_schema(tmp_path, default_schema):
    location = tmp_path / "managed"
    _create(location)

    table = CatalogManagedTable()
    with MockUnityCatalog([_listed(location)], schemas=[SCHEMA], respond=table) as uc:
        before, before_out = run(uc, f"SELECT count(*) FROM unity.{SCHEMA}.managed;", default_schema=default_schema)
        assert before.returncode == 0, before.stderr
        assert before_out.strip() == "0", before_out

        insert, _ = run(uc, f"INSERT INTO unity.{SCHEMA}.managed VALUES (1), (2);", default_schema=default_schema)
        assert insert.returncode == 0, insert.stderr
        assert [c["version"] for c in table.commits] == [1], table.commits
        # Staged for the catalog, not published: only UC's commits know version 1
        assert list(location.glob("_delta_log/_staged_commits/00000000000000000001.*.json")), list(location.rglob("*"))
        assert not (location / "_delta_log" / "00000000000000000001.json").exists()

        # The next attach of the table learns version 1 from UC (and backfills it)
        after, after_out = run(
            uc, f"SELECT count(*), sum(id) FROM unity.{SCHEMA}.managed;", default_schema=default_schema
        )
        assert after.returncode == 0, after.stderr
        assert after_out.strip() == "2|3", after_out

    if default_schema == NO_DEFAULT_SCHEMA:
        # The attach did ask for a default and got none
        assert any(r.endswith(SETTINGS) for r in uc.requests), uc.requests
