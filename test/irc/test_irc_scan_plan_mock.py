"""Offline end-to-end test of the IRC scan-plan READ path (ATTACH -> scan), no creds, no catalog.

test_irc_api_retry.py mocks the IRC plan routes but drives them through
`__internal_uc_plan_table_scan`, which stops at the API boundary. This module mocks the UC catalog
routes *and* the IRC plan routes on one server and points a real ATTACH at it, so the whole read
path runs: UCTableEntry::GetScanFunction -> UCScanPlanPushdownFilter -> UCMultiFileList ->
parquet_scan. Data files are plain local parquet written into a tmpdir, and the table's
storage_location is a file:// URL so TableInformation::RefreshCredentials short-circuits -- no S3,
no httpfs auth, nothing to vend.

That covers three things a live Databricks run structurally cannot:
  - the LAZY plan-task path. Databricks returns every file inline (both live .tests assert
    plan_tasks=0), so nothing else exercises UCMultiFileList::ExpandNextPath.
  - the exact bytes of the IRC filter, read back from the recorded POST body -- the only place the
    wire form is observable at all. Two distinct concerns share that vantage point: that the term
    is JSON-escaped (a real bug), and that it names the physical column rather than a query alias
    (an invariant DuckDB already provides -- see that section's note before assuming otherwise).
  - a terminal non-completed plan status, which arrives as a 200 and so is a *value*, not an
    exception, on the scan path.

Each test says in its own docstring/section whether it is a REGRESSION test (red against the
unfixed code) or CHARACTERIZATION (green either way). That distinction was measured, not assumed;
several of these look like regression tests and are not.

Runs the BUILT duckdb CLI (its ABI matches the locally-built extension; pip `duckdb` would not).
"""

import http.server
import json
import os
import subprocess
import threading
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]  # test/irc/ -> repo root

CATALOG = "mock_cat"
SCHEMA = "mock_sch"
TABLE = "days"

DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri")
ROWS_PER_FILE = 10


def _duckdb_bin():
    build_dir = os.environ.get("BUILD_DIR") or os.environ.get("DUCKDB_BUILD_DIR", "build/debug")
    binary = REPO / build_dir / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set DUCKDB_BUILD_DIR)")
    return str(binary)


def _run_sql(sql, timeout=60):
    return subprocess.run(
        [_duckdb_bin(), "-unsigned", "-noheader", "-list", "-c", sql],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def scalar(result):
    """Last non-empty stdout line: the CLI also prints a `true` row for CREATE SECRET/ATTACH."""
    lines = [ln.strip() for ln in result.stdout.splitlines() if ln.strip()]
    assert lines, f"no output\n--- stderr ---\n{result.stderr}"
    return lines[-1]


@pytest.fixture(scope="module")
def data_files(tmp_path_factory):
    """Five one-day parquet files, ids 1..50 -- the same shape as the live scan_plan fixture."""
    d = tmp_path_factory.mktemp("scan_plan_files")
    paths = []
    for i, day in enumerate(DAYS):
        path = d / f"{day}.parquet"
        lo = i * ROWS_PER_FILE + 1
        hi = lo + ROWS_PER_FILE
        sql = f"COPY (SELECT id, '{day}' AS day FROM range({lo}, {hi}) t(id)) " f"TO '{path}' (FORMAT parquet);"
        r = _run_sql(sql)
        assert r.returncode == 0, r.stderr
        paths.append(str(path))
    return paths


def _file_scan_task(path):
    return {"data-file": {"content": "data", "file-path": path, "file-format": "parquet"}}


# -----------------------------------------------------------------------------
# Mock server: UC catalog routes + IRC scan-plan routes on one port
#


class _Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):  # keep test output quiet
        pass

    def _dispatch(self, method):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode() if length else ""
        self.server.requests.append({"method": method, "path": self.path, "body": body})

        path = self.path
        if "/api/2.1/unity-catalog/catalogs" in path:
            payload = {"catalogs": [{"name": CATALOG}]}
        elif "/api/2.1/unity-catalog/schemas" in path:
            payload = {"schemas": [{"name": SCHEMA, "catalog_name": CATALOG}]}
        elif "/api/2.1/unity-catalog/tables" in path:
            payload = {"tables": [self.server.table_info] if f"schema_name={SCHEMA}" in path else []}
        else:
            # IRC scan-plan routes -- delegated to the per-test script.
            payload = self.server.plan_script(method, path, self.server.requests)

        raw = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_DELETE(self):
        self._dispatch("DELETE")


class MockUC:
    """`plan_script(method, path, requests) -> dict` handles the IRC /plan and /tasks routes."""

    def __init__(self, plan_script, data_source_format="DELTA"):
        self._httpd = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        self._httpd.requests = []
        self._httpd.plan_script = plan_script
        self._httpd.table_info = {
            "name": TABLE,
            "catalog_name": CATALOG,
            "schema_name": SCHEMA,
            "table_type": "EXTERNAL",
            "data_source_format": data_source_format,
            # file:// keeps RefreshCredentials from trying to vend S3 credentials.
            "storage_location": "file:///tmp/uc-scan-plan-mock",
            "table_id": "00000000-0000-0000-0000-0000000000ff",
            "columns": [
                {"name": "id", "type_text": "bigint", "type_name": "LONG", "position": 0, "nullable": True},
                {"name": "day", "type_text": "string", "type_name": "STRING", "position": 1, "nullable": True},
            ],
        }
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

    def plan_bodies(self):
        return [json.loads(r["body"]) for r in self.requests if r["path"].endswith("/plan") and r["body"]]

    def rename_column(self, idx, name):
        """Rename a column in the served /tables response (before the first query)."""
        self._httpd.table_info["columns"][idx]["name"] = name

    def attach_sql(self, alias="unity"):
        return (
            f"CREATE SECRET (TYPE unity_catalog, TOKEN 'tok', ENDPOINT '{self.endpoint}', AWS_REGION 'us-east-1');"
            f"ATTACH '{CATALOG}' AS {alias} (TYPE unity_catalog, READ_ONLY, DEFAULT_SCHEMA '{SCHEMA}',"
            f" use_irc_scan_plan true, api_irc_endpoint_override '{self.endpoint}');"
        )

    def query(self, sql, alias="unity"):
        return _run_sql(f"LOAD unity_catalog; LOAD parquet; {self.attach_sql(alias)} USE {alias}; {sql}")


# -----------------------------------------------------------------------------
# Lazy plan-task expansion
#
# UCMultiFileList::ExpandNextPath must return "this call added a file", not "tokens remain":
# LazyMultiFileList::ExpandNextPathInternal latches all_files_expanded on a false return, and
# MultiFileList::Scan reads the resulting empty OpenFileInfo as end-of-list.
#
# Which of these is the REGRESSION test matters, and it is not the obvious one. Measured against
# the unfixed build, only test_inline_plus_trailing_plan_task goes red. The others pass either way
# because parquet's bind probes the list via GetExpandResult() -> GetFile(1) before the scan: with
# 0-1 inline files that probe drives the expansion to completion, so expanded_files is already
# fully populated by the time the bad `false` latches all_files_expanded, and nothing is lost. The
# bug only bites when >=2 pre-seeded inline files satisfy GetFile(1) WITHOUT expanding, deferring
# the expansion into the scan loop where the empty OpenFileInfo is actually observed. Keep the
# others as coverage of the drain loop, but don't mistake them for proof.
#


def test_all_files_inline(data_files):
    """Baseline: no plan-tasks at all. This is the only shape Databricks actually returns."""

    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [_file_scan_task(f) for f in data_files]}
        return {}

    with MockUC(script) as srv:
        r = srv.query(f"SELECT count(*) FROM {TABLE};")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "50", r.stdout


def test_all_files_behind_a_single_plan_task(data_files):
    """Coverage (not a regression test -- passes unfixed; see the section note): everything lazy."""

    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [], "plan-tasks": ["t1"]}
        if path.endswith("/tasks"):
            return {"file-scan-tasks": [_file_scan_task(f) for f in data_files]}
        return {}

    with MockUC(script) as srv:
        r = srv.query(f"SELECT count(*) FROM {TABLE};")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "50", f"lazily-fetched files were dropped: {r.stdout!r}\n{r.stderr}"


def test_inline_plus_trailing_plan_task(data_files):
    """THE regression test: 2 files inline (enough to satisfy the GetFile(1) probe without
    expanding), 3 behind the final token. Unfixed, this trips DuckDB's
    D_ASSERT(current_file_idx >= GetTotalFileCount()) in MultiFileList::Scan on a debug build; on a
    release build the assert is compiled out and the scan silently stops at 20 of 50 rows."""

    def script(method, path, requests):
        if path.endswith("/plan"):
            return {
                "status": "completed",
                "plan-id": "p1",
                "file-scan-tasks": [_file_scan_task(f) for f in data_files[:2]],
                "plan-tasks": ["t1"],
            }
        if path.endswith("/tasks"):
            return {"file-scan-tasks": [_file_scan_task(f) for f in data_files[2:]]}
        return {}

    with MockUC(script) as srv:
        r = srv.query(f"SELECT count(*) FROM {TABLE};")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "50", f"trailing plan-task batch was dropped: {r.stdout!r}\n{r.stderr}"


def test_chained_and_empty_plan_tasks(data_files):
    """A token may yield more tokens, and a token may yield no files at all -- keep draining."""

    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [], "plan-tasks": ["t1"]}
        if path.endswith("/tasks"):
            token = json.loads(requests[-1]["body"])["plan-task"]
            if token == "t1":
                return {"file-scan-tasks": [_file_scan_task(data_files[0])], "plan-tasks": ["t2"]}
            if token == "t2":  # a batch with no files must not end the expansion
                return {"file-scan-tasks": [], "plan-tasks": ["t3"]}
            return {"file-scan-tasks": [_file_scan_task(f) for f in data_files[1:]]}
        return {}

    with MockUC(script) as srv:
        r = srv.query(f"SELECT count(*) FROM {TABLE};")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "50", f"chained plan-tasks lost files: {r.stdout!r}\n{r.stderr}"


# -----------------------------------------------------------------------------
# Each execution re-plans
#
# The scan-plan response carries short-lived temp S3 credentials and a point-in-time file list.
# Planning at optimize time bakes both into the physical plan, so anything that reuses that plan --
# a prepared statement being the plain case -- replays a stale list on expired credentials. Planning
# in init_global re-plans per execution instead.
#
# PREPARE is what makes this observable: a plain repeated query is re-bound and re-optimized each
# time, so it would re-plan under either design and prove nothing.
#


def test_prepared_statement_replans_each_execution(data_files):
    """The mock hands back a different file set per /plan call, so a reused plan is visible as a
    stale row count."""

    def script(method, path, requests):
        if path.endswith("/plan"):
            n = sum(1 for r in requests if r["path"].endswith("/plan"))  # includes this one
            subset = data_files[:2] if n == 1 else data_files
            return {
                "status": "completed",
                "plan-id": f"p{n}",
                "file-scan-tasks": [_file_scan_task(f) for f in subset],
            }
        return {}

    with MockUC(script) as srv:
        r = srv.query(
            f"PREPARE pp AS SELECT 'R=' || count(*) FROM {TABLE};"
            "EXECUTE pp;"
            "EXECUTE pp;"
        )
        assert r.returncode == 0, r.stderr
        seen = [ln.strip() for ln in r.stdout.splitlines() if ln.strip().startswith("R=")]
        n_plans = sum(1 for x in srv.requests if x["path"].endswith("/plan"))
        # Two executions, two plans: the second must see the file set the server returned second.
        assert n_plans == 2, f"expected one /plan per execution, got {n_plans}"
        assert seen == ["R=20", "R=50"], f"second execution reused the first plan: {seen}"


# -----------------------------------------------------------------------------
# The scan does not depend on the filter-pushdown optimizer running
#
# pushdown_complex_filter has exactly one caller in core (optimizer/pushdown/pushdown_get.cpp), so
# anything that stops FilterPushdown visiting this Get used to leave the scan bound to nothing.
# Planning now lives in init_global and pushdown only stashes the predicate, so disabling the
# optimizer costs server-side pruning and nothing else.
#


def test_scan_works_with_filter_pushdown_disabled(data_files):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [_file_scan_task(f) for f in data_files]}
        return {}

    with MockUC(script) as srv:
        r = srv.query(
            "SET disabled_optimizers='filter_pushdown';"
            f"SELECT count(*) FROM {TABLE} WHERE day = 'Mon';"
        )
        assert r.returncode == 0, r.stderr
        # Right answer: DuckDB's own Filter still applies the predicate.
        assert scalar(r) == "10", r.stdout
        # And the plan went out unfiltered, since pushdown never ran.
        bodies = srv.plan_bodies()
        assert bodies, "no /plan request was made"
        assert "filter" not in bodies[-1], f"expected an unfiltered plan request, got {bodies[-1]}"


# -----------------------------------------------------------------------------
# The IRC filter names the physical column, not the query's alias
#
# CHARACTERIZATION, not regression -- these pass against the unfixed code too, and it is worth
# recording why, because the reasoning is non-obvious enough that it was gotten wrong once.
#
# The term comes from BoundColumnRefExpression::GetName(), which returns the expression's ALIAS
# when one is set. That reads like a hazard: `SELECT day AS d` ought to put "d" on the wire, and a
# term naming the wrong column would make the server prune on it, with the dropped files never
# reaching DuckDB's own Filter (unrecoverable wrong rows). It does not happen. FilterPushdown
# substitutes a copy of the PROJECTION'S UNDERLYING expression -- the base-table column ref, whose
# alias is the physical column name. The output alias "d" lives on the projection list entry,
# which never reaches the Get. Measured across view, subquery, CTE, UNION ALL, JOIN, and a
# deliberate name-SWAP (`SELECT day AS id, id AS day ... WHERE id = 'Mon'`, where a leaked alias
# would name a real different column): every shape sends "day".
#
# So these lock in an invariant that DuckDB currently provides implicitly. If a future core change
# to filter pushdown breaks it, this is where it surfaces.
#


def _completed_all(data_files):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [_file_scan_task(f) for f in data_files]}
        return {}

    return script


def test_filter_term_is_the_physical_column_name(data_files):
    with MockUC(_completed_all(data_files)) as srv:
        r = srv.query(f"SELECT count(*) FROM {TABLE} WHERE day = 'Mon';")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "10", r.stdout
        (body,) = srv.plan_bodies()
        assert body["filter"] == {"type": "eq", "term": "day", "value": "Mon"}, body


def test_filter_term_survives_a_view_alias(data_files):
    with MockUC(_completed_all(data_files)) as srv:
        # TEMP: the attach is READ_ONLY, so the view can't live in the UC catalog.
        r = srv.query(
            f"CREATE TEMP VIEW v AS SELECT id, day AS d FROM {TABLE};" "SELECT count(*) FROM v WHERE d = 'Mon';"
        )
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "10", r.stdout
        bodies = [b for b in srv.plan_bodies() if "filter" in b]
        assert bodies, "no filter was pushed down through the view"
        assert bodies[-1]["filter"] == {
            "type": "eq",
            "term": "day",
            "value": "Mon",
        }, f"alias leaked into the IRC term: {bodies[-1]}"


def test_filter_term_with_a_swapped_alias(data_files):
    """The adversarial shape: `day` aliased to `id` and vice versa. A leaked alias would put "id"
    on the wire -- a real, different column -- so the server would prune on the wrong one."""
    with MockUC(_completed_all(data_files)) as srv:
        r = srv.query(f"SELECT count(*) FROM (SELECT day AS id, id AS day FROM {TABLE}) WHERE id = 'Mon';")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "10", r.stdout
        bodies = [b for b in srv.plan_bodies() if "filter" in b]
        assert bodies, "no filter was pushed down"
        assert bodies[-1]["filter"]["term"] == "day", f"alias leaked into the IRC term: {bodies[-1]}"


# -----------------------------------------------------------------------------
# The term must be JSON-escaped
#
# Regression (this one IS red unfixed): values go through the string escaper but the term was
# concatenated in raw, so a column name containing a quote produces a body that is not valid JSON
# at all. A real server rejects it -> the /plan failure marks the whole ATTACH UNAVAILABLE for 15
# minutes, silently demoting every table to the Delta path.
#


def test_term_with_a_quote_is_escaped(data_files):
    with MockUC(_completed_all(data_files)) as srv:
        srv.rename_column(1, 'we"ird')
        r = srv.query(f'SELECT count(*) FROM {TABLE} WHERE "we""ird" = \'Mon\';')
        assert r.returncode == 0, r.stderr
        raw = [x["body"] for x in srv.requests if x["path"].endswith("/plan") and x["body"]]
        assert raw, "no /plan request was made"
        for body in raw:
            try:
                parsed = json.loads(body)
            except json.JSONDecodeError as e:
                raise AssertionError(f"/plan body is not valid JSON ({e}): {body}") from None
            if "filter" in parsed:
                assert parsed["filter"]["term"] == 'we"ird', parsed


def test_filter_term_survives_a_subquery_alias(data_files):
    with MockUC(_completed_all(data_files)) as srv:
        r = srv.query(f"SELECT count(*) FROM (SELECT day AS d FROM {TABLE}) WHERE d = 'Mon';")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "10", r.stdout
        bodies = [b for b in srv.plan_bodies() if "filter" in b]
        assert bodies, "no filter was pushed down through the subquery"
        assert bodies[-1]["filter"]["term"] == "day", f"alias leaked into the IRC term: {bodies[-1]}"


# -----------------------------------------------------------------------------
# Terminal non-completed plan status
#
# Regression: `{"status":"failed"}` is a 200, so it reached the pushdown callback as a value, not an
# exception. The callback only handled COMPLETED, so it returned with the scan unbound -- D_ASSERT
# in debug, a null init_global call in release. It must be an error (or a fallback), never a crash.
#


# `reported` is what the error names, which is not always what the server sent: an unrecognized
# status parses to UCScanPlanStatus::UNKNOWN, and the enum deliberately doesn't round-trip the raw
# string back out.
@pytest.mark.parametrize(
    "sent,reported",
    [("failed", "failed"), ("cancelled", "cancelled"), ("bogus-status", "unknown")],
)
def test_non_completed_plan_status_is_an_error_not_a_crash(data_files, sent, reported):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": sent, "plan-id": "p1", "error": {"message": "boom", "type": "SomeError"}}
        return {}

    # data_source_format != DELTA so there is no fallback to mask the failure -- the error must
    # surface, and name the status.
    with MockUC(script, data_source_format="PARQUET") as srv:
        r = srv.query(f"SELECT count(*) FROM {TABLE};")
        assert r.returncode != 0, f"expected an error, got: {r.stdout!r}"
        combined = r.stdout + r.stderr
        # The load-bearing assertion. A nonzero exit is NOT enough: unfixed, this path aborts on
        # D_ASSERT(bd.scan_plan_done) in UCScanPlanInitGlobal, which also exits nonzero and also
        # prints the word "Error". What distinguishes a handled failure from a crash is that the
        # message names the offending status and is not an assertion failure.
        assert "assertion failure" not in combined.lower(), f"crashed instead of erroring:\n{combined}"
        assert f"status '{reported}'" in combined, f"error did not name the plan status {reported!r}:\n{combined}"
