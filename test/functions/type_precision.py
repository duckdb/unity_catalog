"""Regression: a UC /tables ColumnInfo with null type_precision must parse (coerce to 0), not throw.

Delta-Spark-created tables report type_precision/type_scale as JSON null in the UC /tables response;
`ParseColumnDefinition` (src/uc_api.cpp) once parsed them with fail_on_missing=true and threw
`IO Error: Invalid field found while parsing field: type_precision` on ANY listing/read -- before
touching Delta data. `uctl`/`bin/uc` always write 0, so a live OSS server can't reproduce the shape.

This drives the extension's read path (CREATE SECRET -> ATTACH -> SHOW ALL TABLES) against a small
in-process **mock** Unity Catalog server whose /tables response carries the exact null-precision
ColumnInfo. That makes it a deterministic test of the *parser* -- no live UC container, no
registration, no read-your-writes/cold-start timing (an earlier live version was flaky on CI for
exactly those reasons; see git history and scripts/uc_read_after_write_repro.py).
"""

import http.server
import json
import os
import subprocess
import threading
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_CATALOG = "duck"
_SCHEMAS = ("cmt", "plain")

# The Delta-Spark shape `uctl` never produces: columns whose type_precision/type_scale are JSON null.
# This is the exact ColumnInfo pre-fix ParseColumnDefinition threw on.
_SPARK_LIKE_TABLE = {
    "name": "spark_like",
    "catalog_name": _CATALOG,
    "schema_name": "cmt",
    "table_type": "EXTERNAL",
    "data_source_format": "DELTA",
    "storage_location": "file:///tmp/uc-type-precision-repro",
    "table_id": "00000000-0000-0000-0000-000000000001",
    "columns": [
        {
            "name": "id",
            "type_text": "bigint",
            "type_name": "LONG",
            "type_precision": None,
            "type_scale": None,
            "position": 0,
            "nullable": True,
        },
        {"name": "name", "type_text": "string", "type_name": "STRING", "position": 1, "nullable": True},
    ],
}


class _MockUnityCatalog:
    """Serves the /catalogs + /schemas + /tables the ATTACH + SHOW ALL TABLES read path needs.

    Records every request path so a missing/unexpected endpoint is visible on failure.
    """

    def __init__(self):
        self.requests = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                p = self.path
                if "/catalogs" in p:
                    body = {"catalogs": [{"name": _CATALOG}]}
                elif "/schemas" in p:
                    body = {"schemas": [{"name": s, "catalog_name": _CATALOG} for s in _SCHEMAS]}
                elif "/tables" in p:
                    body = {"tables": [_SPARK_LIKE_TABLE] if "schema_name=cmt" in p else []}
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


def _duckdb_bin():
    binary = _REPO / os.environ.get("DUCKDB_BUILD_DIR", "build/debug") / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set DUCKDB_BUILD_DIR)")
    return str(binary)


def test_type_precision_null():
    """SHOW ALL TABLES over a null-type_precision ColumnInfo must succeed (not throw) and list the table."""
    with _MockUnityCatalog() as mock:
        sql = (
            # threads=1: SHOW ALL TABLES fans out its per-schema table fetches across DuckDB's
            # thread pool, and that concurrent listing is intermittently racy (a table goes
            # missing). This test pins the null-type_precision PARSE fix, not concurrency, so
            # serialize to keep it deterministic.
            "SET threads TO 1;"
            f"CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{mock.endpoint}', AWS_REGION 'us-east-2');"
            f"ATTACH '{_CATALOG}' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA 'cmt');"
            "SELECT 'spark_like_count=' || count(*) FROM (SHOW ALL TABLES) t WHERE t.name = 'spark_like';"
        )
        result = subprocess.run([_duckdb_bin(), "-unsigned", "-c", sql], capture_output=True, text=True, timeout=60)

    detail = f"\n--- stderr ---\n{result.stderr}\n--- mock requests ---\n" + "\n".join(mock.requests)
    # Core regression: the null type_precision must not raise while listing.
    assert result.returncode == 0, "SHOW ALL TABLES errored on a null-type_precision ColumnInfo" + detail
    # And the table must actually list -- proving the value was coerced (null -> 0), not dropped.
    assert "spark_like_count=1" in result.stdout, "spark_like was not listed" + detail
