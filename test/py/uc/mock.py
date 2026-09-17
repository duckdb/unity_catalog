"""An in-process Unity Catalog serving fixed tables, for tests of shapes a live server will not produce.

    with MockUnityCatalog([delta_table("pairs", fixture, columns)]) as mock:
        result, stdout = run(mock, "SELECT id FROM unity.plain.pairs;")

Serves only the /catalogs, /schemas and /tables an ATTACH and a read need, and records every request
path so a missing or unexpected endpoint shows on failure. Tables live at file:// locations, which
keeps credential vending out of it.
"""

import http.server
import json
import os
import re
import subprocess
import threading
from pathlib import Path

import pytest

CATALOG = "duck"

_REPO = Path(__file__).resolve().parents[3]


def delta_table(name, location, columns, *, schema="plain"):
    """A /tables entry for an external Delta table at `location`, reporting `columns` as its ColumnInfo."""
    return {
        "name": name,
        "catalog_name": CATALOG,
        "schema_name": schema,
        "table_type": "EXTERNAL",
        "data_source_format": "DELTA",
        "storage_location": f"file://{location}",
        "table_id": "00000000-0000-0000-0000-000000000001",
        "columns": columns,
    }


class MockUnityCatalog:
    """Serves `tables`, each listed under its own schema. `schemas` defaults to the tables' schemas, in
    order; the first is the default schema `run` attaches with."""

    def __init__(self, tables, schemas=None):
        self.requests = []
        self.schemas = list(schemas or dict.fromkeys(t["schema_name"] for t in tables))
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                if "/catalogs" in self.path:
                    body = {"catalogs": [{"name": CATALOG}]}
                elif "/schemas" in self.path:
                    body = {"schemas": [{"name": s, "catalog_name": CATALOG} for s in outer.schemas]}
                elif "/tables" in self.path:
                    body = {"tables": [t for t in tables if f"schema_name={t['schema_name']}" in self.path]}
                else:
                    body = {}
                payload = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(payload)

        self._httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._httpd.shutdown()

    @property
    def endpoint(self):
        host, port = self._httpd.server_address
        return f"http://{host}:{port}"


def duckdb_bin():
    binary = _REPO / os.environ.get("DUCKDB_BUILD_DIR", "build/debug") / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set DUCKDB_BUILD_DIR)")
    return str(binary)


def run(mock, sql):
    """Attach the mock catalog as `unity`, run `sql` in list mode, and return (result, stdout with colour
    stripped). stdout holds only what `sql` prints: the attach's own output is discarded, its errors are
    not."""
    prelude = (
        # The listing fans out across the thread pool and is intermittently racy.
        "SET threads TO 1;"
        f"CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{mock.endpoint}', AWS_REGION 'us-east-2');"
        f"ATTACH '{CATALOG}' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA '{mock.schemas[0]}');"
    )
    quiet_prelude = ["-cmd", ".output /dev/null", "-cmd", prelude, "-cmd", ".output"]
    result = subprocess.run(
        [duckdb_bin(), "-unsigned", "-list", "-noheader", *quiet_prelude, "-c", sql],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    return result, re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
