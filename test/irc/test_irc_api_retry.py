"""Offline test of the IRC scan-plan request/poll/cancel loop (UCAPI::PlanTableScan).

No live catalog, no creds: a threaded Python mock HTTP server serves the three scan-plan routes
(POST /plan, GET /plan/{id}, DELETE /plan/{id}) with scripted responses and records every request.
The internal `__internal_uc_plan_table_scan` table function drives PlanTableScan directly against
the mock, so the poll/backoff/Retry-After/cancel behavior is exercised end-to-end and asserted
from the server's request log -- which is why this is a .py (server lifecycle + request
introspection) rather than a sqllogictest .test.

Runs the BUILT duckdb CLI (its ABI matches the locally-built extension; pip `duckdb` would not).
"""

import http.server
import json
import os
import subprocess
import threading
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]  # test/irc/ -> repo root


def _duckdb_bin():
    build_dir = os.environ.get("DUCKDB_BUILD_DIR", "build/debug")
    binary = REPO / build_dir / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set DUCKDB_BUILD_DIR)")
    return str(binary)


class _Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):  # keep the test output quiet
        pass

    def _dispatch(self, method):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode() if length else ""
        self.server.requests.append({"method": method, "path": self.path, "body": body, "t": time.monotonic()})
        status, headers, payload = self.server.script(method, self.path, self.server.requests)
        self.send_response(status)
        for key, value in headers.items():
            self.send_header(key, value)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        if payload is not None:
            self.wfile.write(payload.encode())

    def do_POST(self):
        self._dispatch("POST")

    def do_GET(self):
        self._dispatch("GET")

    def do_DELETE(self):
        self._dispatch("DELETE")


class MockPlanServer:
    """`script(method, path, requests) -> (status, headers_dict, json_str_or_None)`."""

    def __init__(self, script):
        self._httpd = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        self._httpd.requests = []
        self._httpd.script = script
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

    @property
    def requests(self):
        return self._httpd.requests

    def count(self, method):
        return sum(1 for r in self.requests if r["method"] == method)


def _run_plan_scan(endpoint, *, filter_json=None, pre="", timeout=30):
    args = f"'{endpoint}', 'cat', 'sch', 'tbl', 'tok'"
    if filter_json is not None:
        args += f", filter => '{filter_json}'"
    sql = f"LOAD unity_catalog; LOAD httpfs; {pre} SELECT * FROM __internal_uc_plan_table_scan({args});"
    return subprocess.run([_duckdb_bin(), "-unsigned", "-c", sql], capture_output=True, text=True, timeout=timeout)


def _resp(status_str, plan_id="p1", retry_after=None):
    headers = {"Retry-After": str(retry_after)} if retry_after is not None else {}
    return 200, headers, json.dumps({"status": status_str, "plan-id": plan_id})


# -----------------------------------------------------------------------------
# Poll behavior
#


def test_poll_submitted_then_completed():
    def script(method, path, requests):
        if method == "POST" and path.endswith("/plan"):
            return _resp("submitted")
        if method == "GET":
            # current GET is already logged; complete on the 3rd (after 2 'submitted')
            n = sum(1 for r in requests if r["method"] == "GET")
            return _resp("completed") if n >= 3 else _resp("submitted")
        return 200, {}, "{}"

    with MockPlanServer(script) as srv:
        result = _run_plan_scan(srv.endpoint)
        assert result.returncode == 0, result.stderr
        assert "completed" in result.stdout
        assert srv.count("POST") == 1
        assert srv.count("GET") == 3  # 2 submitted + 1 completed


def test_immediate_completed_does_not_poll():
    def script(method, path, requests):
        if method == "POST" and path.endswith("/plan"):
            return _resp("completed")
        return 200, {}, "{}"

    with MockPlanServer(script) as srv:
        result = _run_plan_scan(srv.endpoint)
        assert result.returncode == 0, result.stderr
        assert "completed" in result.stdout
        assert srv.count("GET") == 0  # no polling when the POST already completes


def test_failed_status_surfaces():
    def script(method, path, requests):
        if method == "POST" and path.endswith("/plan"):
            return 200, {}, json.dumps({"status": "failed", "error": {"message": "boom", "type": "X"}})
        return 200, {}, "{}"

    with MockPlanServer(script) as srv:
        result = _run_plan_scan(srv.endpoint)
        assert result.returncode == 0, result.stderr
        assert "failed" in result.stdout
        assert srv.count("GET") == 0


# -----------------------------------------------------------------------------
# Retry-After pacing
#


def test_retry_after_is_honored():
    def script(method, path, requests):
        if method == "POST" and path.endswith("/plan"):
            return _resp("submitted", retry_after=1)  # seed a 1s wait before the first poll
        if method == "GET":
            return _resp("completed")
        return 200, {}, "{}"

    with MockPlanServer(script) as srv:
        result = _run_plan_scan(srv.endpoint, timeout=30)
        assert result.returncode == 0, result.stderr
        post = next(r for r in srv.requests if r["method"] == "POST")
        first_get = next(r for r in srv.requests if r["method"] == "GET")
        # Retry-After: 1 => the first poll must wait ~1s, not the 100ms backoff floor.
        assert first_get["t"] - post["t"] >= 0.9


# -----------------------------------------------------------------------------
# Cancellation -> best-effort plan DELETE
#


def test_timeout_cancel_issues_plan_delete():
    def script(method, path, requests):
        if method == "POST" and path.endswith("/plan"):
            return _resp("submitted")
        if method == "GET":
            return _resp("submitted")  # never completes -> statement_timeout must fire
        if method == "DELETE":
            return 200, {}, "{}"
        return 200, {}, "{}"

    with MockPlanServer(script) as srv:
        result = _run_plan_scan(srv.endpoint, pre="SET max_execution_time=1000;", timeout=30)
        assert result.returncode != 0  # query aborted by the timeout
        assert srv.count("POST") >= 1, [r["method"] for r in srv.requests]
        assert srv.count("DELETE") >= 1, [r["method"] for r in srv.requests]


# -----------------------------------------------------------------------------
# Request body: the filter JSON is wrapped into the plan request
#


def test_filter_is_sent_in_plan_body():
    def script(method, path, requests):
        if method == "POST" and path.endswith("/plan"):
            return _resp("completed")
        return 200, {}, "{}"

    filter_json = '{"type":"eq","term":"day","value":"Mon"}'
    with MockPlanServer(script) as srv:
        result = _run_plan_scan(srv.endpoint, filter_json=filter_json)
        assert result.returncode == 0, result.stderr
        post = next(r for r in srv.requests if r["method"] == "POST")
        body = json.loads(post["body"])
        assert body.get("case-sensitive") is False
        assert body.get("filter") == json.loads(filter_json)
