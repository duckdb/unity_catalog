"""Driver for nested_projection.test: a nested read through the UC catalog path.

Reads the same committed fixture twice -- through the catalog, and through `delta_scan()` on its
storage location, which is the control. Registered over REST rather than with `uctl`, because
`uc table create` splits `--columns` on every comma and so cannot state a nested type.
"""

import json
import os
import shutil
import urllib.request
import uuid

import pytest

from ducktest import run_paired
from uc import REPO_ROOT, plain_table_location, uctl
from uc.server import ENDPOINT

CATALOG = "duck"
SCHEMA = "plain"  # EXTERNAL -> the table gets a location we can stage the fixture into
FIXTURE = REPO_ROOT / "data" / "nested_projection"

# Mirrors the Delta log's schemaString.
COLUMNS = [
    ("id", "int", "INT", '{"name":"id","type":"integer","nullable":true,"metadata":{}}'),
    (
        "records",
        "array<struct<name:string,value:int>>",
        "ARRAY",
        '{"name":"records","type":{"type":"array","elementType":{"type":"struct","fields":'
        '[{"name":"name","type":"string","nullable":true,"metadata":{}},'
        '{"name":"value","type":"integer","nullable":true,"metadata":{}}]},'
        '"containsNull":true},"nullable":true,"metadata":{}}',
    ),
    (
        "multi",
        "map<string,array<int>>",
        "MAP",
        '{"name":"multi","type":{"type":"map","keyType":"string","valueType":'
        '{"type":"array","elementType":"integer","containsNull":true},'
        '"valueContainsNull":true},"nullable":true,"metadata":{}}',
    ),
]


def _register(table, location):
    body = {
        "name": table,
        "catalog_name": CATALOG,
        "schema_name": SCHEMA,
        "table_type": "EXTERNAL",
        "data_source_format": "DELTA",
        "storage_location": str(location),
        "columns": [
            {
                "name": name,
                "type_text": type_text,
                "type_name": type_name,
                "type_json": type_json,
                "position": position,
                "nullable": True,
            }
            for position, (name, type_text, type_name, type_json) in enumerate(COLUMNS)
        ],
    }
    request = urllib.request.Request(
        f"{ENDPOINT}/api/2.1/unity-catalog/tables",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


@pytest.mark.oss_local
def test_nested_projection(request, uc_server):
    # Unique per run: a shared container (--existing-service) would collide on name and location.
    table = f"nested_projection_{uuid.uuid4().hex[:8]}"
    location = plain_table_location(uc_server, table)

    try:
        shutil.copytree(FIXTURE, location)
        _register(table, location)

        env = {
            **os.environ,
            "UC_TEST_CATALOG": CATALOG,
            "UC_TEST_SCHEMA": SCHEMA,
            "UC_TEST_TABLE": table,
            "UC_TEST_TABLE_LOCATION": str(location),
        }
        run_paired(request, env=env)
    finally:
        uctl("drop", SCHEMA, table, check=False)
        shutil.rmtree(location, ignore_errors=True)
