"""In-process mock Unity Catalog server for tests that drive the extension's read path only.

A mock (not the OSS container) is the right tool when the thing under test is how the extension
parses or enumerates a *response shape*: no registration, no read-your-writes timing, no cold start
-- and response shapes `uctl` never produces (e.g. null type_precision) become expressible.

Serves the three GETs `ATTACH` + `SHOW ALL TABLES` need (/catalogs, /schemas, /tables) and records
every request path, so a missing or unexpected endpoint is visible in the failure output.
"""

import http.server
import json
import threading

CATALOG = "duck"


class MockUnityCatalog:
    """`tables` maps schema name -> list of table dicts; its keys are the served schema list."""

    def __init__(self, tables):
        self.tables = tables
        self.requests = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                p = self.path
                if "/catalogs" in p:
                    body = {"catalogs": [{"name": CATALOG}]}
                elif "/schemas" in p:
                    body = {"schemas": [{"name": s, "catalog_name": CATALOG} for s in outer.tables]}
                elif "/tables" in p:
                    schema = next((s for s in outer.tables if f"schema_name={s}" in p), None)
                    body = {"tables": outer.tables.get(schema, [])}
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

    def attach_sql(self, default_schema):
        return (
            f"CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{self.endpoint}', AWS_REGION 'us-east-2');"
            f"ATTACH '{CATALOG}' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA '{default_schema}');"
        )


def table(name, schema, columns):
    return {
        "name": name,
        "catalog_name": CATALOG,
        "schema_name": schema,
        "table_type": "EXTERNAL",
        "data_source_format": "DELTA",
        "storage_location": f"file:///tmp/uc-mock/{schema}/{name}",
        "table_id": f"00000000-0000-0000-0000-{abs(hash((schema, name))) % 10**12:012d}",
        "columns": columns,
    }
