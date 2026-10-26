"""Per-extension (uc) pytest helpers.

Real, per-extension package (NOT symlinked) -- see test/py/driver/README.md "Layout".
Holds shared paths + the `uctl` table-ops wrapper; resource declarations/fixtures live
in sibling modules (e.g. server.py: the OSS UC server resource).

Imported by drivers as `from uc import uctl` / `from uc.server import uc_server`.
"""

import json
import os
import subprocess
import urllib.request
from pathlib import Path

from ducktest import ProvisionFailed

# test/py/uc/__init__.py -> repo root is 3 dirs up (uc -> py -> test -> root).
# uc/ is a real dir (only test/py/driver is a symlink), so resolve() is safe here.
REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS_DIR = REPO_ROOT / "scripts" / "oss_uc_image"
UCTL = os.environ.get("UC_UCTL", str(SCRIPTS_DIR / "uctl"))

# Locally-built extensions a CLI test LOADs by full path under `-unsigned`. A bare `LOAD name` resolves
# through the build's local extension repository, which the CI test job does not receive (it gets the
# loadables only), and the shell has no extension linked in.
EXTENSIONS = ("parquet", "httpfs", "delta", "unity_catalog")


def build_dir():
    """The build tree CLI tests run against: $BUILD_DIR, else $DUCKDB_BUILD_DIR, else build/debug."""
    return REPO_ROOT / os.environ.get("BUILD_DIR", os.environ.get("DUCKDB_BUILD_DIR", "build/debug"))


def load_statements(build_dir, extensions=EXTENSIONS):
    """`LOAD '<build_dir>/extension/<name>/<name>.duckdb_extension';` for each extension, one per line."""
    return "\n".join(
        f"LOAD '{Path(build_dir) / 'extension' / name / f'{name}.duckdb_extension'}';" for name in extensions
    )


def uctl(*args, check=True):
    """Run the image kit's `uctl <args>` (wraps `docker exec <container> bin/uc ...`).

    Container name via DUCKTEST_UC_CONTAINER (uctl reads it); the `uc_server` fixture
    starts that container. Surfaces stdout/stderr on failure. Examples:

        uctl("create", "managed", "id_name", "id INT, name STRING")
        uctl("drop", "managed", "id_name", check=False)   # ignore "doesn't exist"
    """
    r = subprocess.run([UCTL, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        # failed uctl IS provisioning failure -- raise with the output so it surfaces clean log line
        out = (r.stdout + r.stderr).strip()
        raise ProvisionFailed(f"uctl {' '.join(map(str, args))} failed (exit {r.returncode}): {out}")
    return r


def plain_table_location(uc_server, table, catalog="duck", schema="plain"):
    """Where uctl points an EXTERNAL table: `<data dir>/<catalog>/<schema>/<table>`.

    The data dir is bind-mounted at the same path host and container, so a test body reads back the
    same tree the write went to. Drivers that stage a hand-written `_delta_log` need this before the
    table exists, which is why it is computed rather than asked for.
    """
    if not uc_server.data_dir:
        raise ProvisionFailed(
            "uc_server exposes no data_dir, so an EXTERNAL table's storage location cannot be "
            "staged into -- the container was attached rather than started here"
        )
    return Path(uc_server.data_dir) / catalog / schema / table


def register_external_table(endpoint, table, location, columns, properties=None, catalog="duck", schema="plain"):
    """Register an EXTERNAL Delta table over REST, for column types `uctl` cannot state.

    `columns` are (name, type_text, type_name, type_json) tuples, in position order.
    """
    body = {
        "name": table,
        "catalog_name": catalog,
        "schema_name": schema,
        "table_type": "EXTERNAL",
        "data_source_format": "DELTA",
        "storage_location": str(location),
        "properties": properties or {},
        "columns": [
            {
                "name": name,
                "type_text": type_text,
                "type_name": type_name,
                "type_json": type_json,
                "position": position,
                "nullable": True,
            }
            for position, (name, type_text, type_name, type_json) in enumerate(columns)
        ],
    }
    request = urllib.request.Request(
        f"{endpoint}/api/2.1/unity-catalog/tables",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def _type_text(delta_type):
    """UC's `type_text` for a Delta schema type, in Spark's simple-string spelling."""
    if isinstance(delta_type, str):
        return {"integer": "int", "long": "bigint"}.get(delta_type, delta_type)
    if delta_type["type"] == "struct":
        return "struct<" + ",".join(f"{f['name']}:{_type_text(f['type'])}" for f in delta_type["fields"]) + ">"
    if delta_type["type"] == "array":
        return f"array<{_type_text(delta_type['elementType'])}>"
    return f"map<{_type_text(delta_type['keyType'])},{_type_text(delta_type['valueType'])}>"


def delta_log_columns(location):
    """The (name, type_text, type_name, type_json) columns UC reports for the Delta table at `location`: its
    latest schema, as `register_external_table` takes them."""
    schema = None
    for commit in sorted(Path(location, "_delta_log").glob("*.json")):
        for line in commit.read_text().splitlines():
            action = json.loads(line)
            if "metaData" in action:
                schema = json.loads(action["metaData"]["schemaString"])
    columns = []
    for field in schema["fields"]:
        type_text = _type_text(field["type"])
        type_name = {"bigint": "LONG"}.get(type_text, type_text.split("<")[0].upper())
        columns.append((field["name"], type_text, type_name, json.dumps(field)))
    return columns
