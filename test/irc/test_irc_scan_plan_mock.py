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
import re
import subprocess
import threading
from pathlib import Path

import pytest

from uc import build_dir, load_statements

REPO = Path(__file__).resolve().parents[2]  # test/irc/ -> repo root

CATALOG = "mock_cat"
SCHEMA = "mock_sch"
TABLE = "days"

DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri")
ROWS_PER_FILE = 10


def _duckdb_bin():
    binary = build_dir() / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set BUILD_DIR)")
    return str(binary)


def _run_sql(sql, timeout=60, cli_args=()):
    return subprocess.run(
        [_duckdb_bin(), "-unsigned", *cli_args, "-noheader", "-list", "-c", sql],
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
        sql = f"COPY (SELECT id, '{day}' AS day FROM range({lo}, {hi}) t(id)) TO '{path}' (FORMAT parquet);"
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
        if path.startswith("/mock-bucket/"):
            return self._blob(method)
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

    def _blob(self, method):
        """Object storage at `/mock-bucket/<key>` (path style), for reads through a secret's credentials: serves
        `server.blobs[key]` with Range support and records how each request was signed."""
        key = self.path.split("?", 1)[0][len("/mock-bucket/") :]
        auth = self.headers.get("Authorization", "")
        if auth.startswith("AWS4-HMAC-SHA256 "):
            signed = "hmac"
        elif auth.startswith("Bearer "):
            signed = "bearer"
        else:
            signed = "unsigned" if not auth else "other"
        self.server.blob_requests.append((method, key, signed))
        data = self.server.blobs.get(key)
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        status, body = 200, data
        rng = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if rng:
            lo = int(rng.group(1))
            hi = int(rng.group(2)) if rng.group(2) else len(data) - 1
            status, body = 206, data[lo : hi + 1]
            self.send_response(status)
            self.send_header("Content-Range", f"bytes {lo}-{lo + len(body) - 1}/{len(data)}")
        else:
            self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Last-Modified", "Mon, 09 Nov 2026 00:00:00 GMT")
        self.send_header("ETag", '"mock"')
        self.end_headers()
        if method != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self._dispatch("HEAD")

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
        self._httpd.blobs = {}
        self._httpd.blob_requests = []
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

    @property
    def address(self):
        return self._httpd.server_address

    @property
    def blob_requests(self):
        return self._httpd.blob_requests

    def serve_blobs(self, blobs):
        """Serve `blobs` ({key: bytes}) as objects under /mock-bucket/."""
        self._httpd.blobs.update(blobs)

    def set_plan_script(self, plan_script):
        self._httpd.plan_script = plan_script

    def set_storage_location(self, location):
        """The table's storage_location in the /tables response (before the first query)."""
        self._httpd.table_info["storage_location"] = location

    def serve_delta_table(self, location, columns):
        """Point the served table at a real Delta table (before the first query)."""
        self._httpd.table_info["storage_location"] = f"file://{location}"
        self._httpd.table_info["columns"] = columns

    def rename_column(self, idx, name):
        """Rename a column in the served /tables response (before the first query)."""
        self._httpd.table_info["columns"][idx]["name"] = name

    def attach_sql(self, alias="unity", scan_plan="true"):
        """scan_plan=None omits the option entirely, exercising the default."""
        opt = "" if scan_plan is None else f" use_irc_scan_plan {scan_plan},"
        return (
            f"CREATE SECRET (TYPE unity_catalog, TOKEN 'tok', ENDPOINT '{self.endpoint}', AWS_REGION 'us-east-1');"
            f"ATTACH '{CATALOG}' AS {alias} (TYPE unity_catalog, READ_ONLY, DEFAULT_SCHEMA '{SCHEMA}',"
            f"{opt} API_IRC_ENDPOINT_OVERRIDE '{self.endpoint}');"
        )

    def query(self, sql, alias="unity", scan_plan="true", cli_args=()):
        return _run_sql(
            f"{load_statements(build_dir())} {self.attach_sql(alias, scan_plan)} USE {alias}; {sql}", cli_args=cli_args
        )

    def plan_requests(self):
        return [x for x in self.requests if x["path"].endswith("/plan")]


# -----------------------------------------------------------------------------
# A GCS credential vended by /plan becomes a gcs secret
#
# Iceberg REST vends a GCS token as `gcs.oauth2.token`; the delta/v1 spelling `gcs.oauth-token` is
# accepted too. The data files stay local: only the secret the plan's storage credential becomes is
# under test. Read inside BEGIN so the secret is still there whatever its lifetime.
#
# `iceberg-key` is a REGRESSION case (red while the plan path read only `gcs.oauth-token`), and so is
# `no-token` (red while an empty s3 secret was created in its place); `delta-key` is CHARACTERIZATION.
#


def _vending_gcs(data_files, config):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {
                "status": "completed",
                "plan-id": "p1",
                "file-scan-tasks": [_file_scan_task(f) for f in data_files],
                "storage-credentials": [{"prefix": "gs://mock-bucket/days/", "config": config}],
            }
        return {}

    return script


def _strip_colour(text):
    # The CLI prints warnings to stdout, in colour
    return re.sub(r"\x1b\[[0-9;]*m", "", text)


@pytest.mark.parametrize(
    "config,secret_type",
    [
        ({"gcs.oauth2.token": "ya29.mock"}, "gcs"),
        ({"gcs.oauth-token": "ya29.mock"}, "gcs"),
        ({"gcs.oauth2.token-expires-at": "0"}, None),  # no token: no secret
    ],
    ids=["iceberg-key", "delta-key", "no-token"],
)
def test_gcs_credential_from_the_plan(data_files, config, secret_type):
    with MockUC(_vending_gcs(data_files, config)) as srv:
        r = srv.query(
            f"BEGIN; SELECT 'rows', count(*) FROM {TABLE}; "
            f"SELECT 'secret', type, scope[1] FROM duckdb_secrets() WHERE name LIKE '__internal_uc_scanplan__%'; COMMIT;"
        )
        assert r.returncode == 0, r.stderr
        rows = _labelled(_strip_colour(r.stdout))
        assert rows.get("rows") == "50", r.stdout
        expected = f"{secret_type}|gs://mock-bucket/days/" if secret_type else None
        assert rows.get("secret") == expected, r.stdout


# Which key wins, read from the secret itself (`-unredacted`): the Iceberg key when both are set, the
# delta/v1 key when the Iceberg one is empty, and with neither the warning and no secret.
@pytest.mark.parametrize(
    "config,token",
    [
        ({"gcs.oauth2.token": "ya29.iceberg", "gcs.oauth-token": "ya29.delta"}, "ya29.iceberg"),
        ({"gcs.oauth2.token": "", "gcs.oauth-token": "ya29.delta"}, "ya29.delta"),
        ({}, None),
    ],
    ids=["both-keys", "empty-iceberg-key", "neither"],
)
def test_gcs_token_key_precedence(data_files, config, token):
    with MockUC(_vending_gcs(data_files, config)) as srv:
        r = srv.query(
            f"BEGIN; SELECT count(*) FROM {TABLE}; "
            "SELECT 'secret', secret_string FROM duckdb_secrets(redact=false) "
            "WHERE name LIKE '__internal_uc_scanplan__%'; COMMIT;",
            cli_args=("-unredacted",),
        )
        assert r.returncode == 0, r.stderr
        out = _strip_colour(r.stdout)
        secret = [ln.strip().partition("|")[2] for ln in out.splitlines() if ln.strip().startswith("secret|")]
        if token:
            assert len(secret) == 1 and "type=gcs;" in secret[0], out
            assert f"bearer_token={token}" in secret[0], out
        else:
            assert secret == [], out
            assert "Unity Catalog vended no GCP credential" in out, out


# -----------------------------------------------------------------------------
# Which credentials a gs:// read goes out with
#
# The data files are served by the mock itself, as `gs://mock-bucket/days/<file>` with the endpoint in
# the URL (transport only: the secret supplies all the authentication; httpfs takes URL parameters only
# with `s3_enable_deprecated_url_parameters`), and the mock records how each request was signed. The user has a gcs HMAC secret for the bucket.
#
# `no-token` is a REGRESSION case: while an empty s3 secret was created for the prefix, httpfs found it
# ahead of the user's gcs secret (it looks up s3 secrets first) and the reads went out unsigned.
# `vended-token` is CHARACTERIZATION: the vended secret's longer scope beats the user's.
#


def _serving_gcs(data_files, config, host, port):
    def script(method, path, requests):
        if path.endswith("/plan"):
            transport = f"?s3_endpoint={host}:{port}&s3_use_ssl=false&s3_url_style=path"
            return {
                "status": "completed",
                "plan-id": "p1",
                "file-scan-tasks": [
                    _file_scan_task(f"gs://mock-bucket/days/{Path(f).name}{transport}") for f in data_files
                ],
                "storage-credentials": [{"prefix": "gs://mock-bucket/days/", "config": config}],
            }
        return {}

    return script


@pytest.mark.parametrize(
    "config,signed",
    [({"gcs.oauth2.token-expires-at": "0"}, "hmac"), ({"gcs.oauth2.token": "ya29.mock"}, "bearer")],
    ids=["no-token", "vended-token"],
)
def test_gs_read_signs_with_the_vended_token_else_the_users_secret(data_files, config, signed):
    with MockUC(None) as srv:
        srv.serve_blobs({f"days/{Path(f).name}": Path(f).read_bytes() for f in data_files})
        srv.set_plan_script(_serving_gcs(data_files, config, *srv.address))
        # httpfs reads the endpoint from the URL only when asked to (duckdb v2.0.1's httpfs)
        r = srv.query(
            "SET s3_enable_deprecated_url_parameters = true; "
            "CREATE SECRET user_hmac (TYPE gcs, KEY_ID 'HMACKEY', SECRET 'hmac-secret', SCOPE 'gs://mock-bucket/'); "
            f"SELECT 'rows', count(*) FROM {TABLE};"
        )
        assert r.returncode == 0, r.stderr
        assert _labelled(_strip_colour(r.stdout)).get("rows") == "50", r.stdout
        modes = {mode for _, _, mode in srv.blob_requests}
        assert srv.blob_requests and modes == {signed}, srv.blob_requests


# -----------------------------------------------------------------------------
# Each cloud's /plan credential, and credentials applied again in one transaction
#
# The data files stay local, so these check the secrets only. Azure needs the azure extension, which
# this repo does not build: it is loaded from $UC_TEST_AZURE_EXTENSION, else the case is skipped.
#

_ABFSS = "abfss://container@account.dfs.core.windows.net/days/"


def _azure_load():
    path = os.environ.get("UC_TEST_AZURE_EXTENSION")
    if not path or not Path(path).exists():
        pytest.skip("azure extension not available (set UC_TEST_AZURE_EXTENSION to its .duckdb_extension)")
    return f"LOAD '{path}';"


def _vending_one(data_files, prefix, config):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {
                "status": "completed",
                "plan-id": "p1",
                "file-scan-tasks": [_file_scan_task(f) for f in data_files],
                "storage-credentials": [{"prefix": prefix, "config": config}],
            }
        return {}

    return script


@pytest.mark.parametrize(
    "prefix,config,secret_type,expected",
    [
        (
            "s3://mock-bucket/days/",
            {"s3.access-key-id": "AKIAMOCK", "s3.secret-access-key": "mock-secret", "client.region": "eu-west-1"},
            "s3",
            ["key_id=AKIAMOCK", "region=eu-west-1"],
        ),
        ("gs://mock-bucket/days/", {"gcs.oauth2.token": "ya29.mock"}, "gcs", ["bearer_token=ya29.mock"]),
        (
            _ABFSS,
            {"adls.sas-token.account.dfs.core.windows.net": "sv=2026&sig=mock"},
            "azure",
            ["account_name=account", "AccountName=account;SharedAccessSignature=sv=2026&sig=mock"],
        ),
    ],
    ids=["aws", "gcs", "azure"],
)
def test_plan_credential_of_each_cloud(data_files, prefix, config, secret_type, expected):
    load = _azure_load() if secret_type == "azure" else ""
    with MockUC(_vending_one(data_files, prefix, config)) as srv:
        r = srv.query(
            f"{load} BEGIN; SELECT 'rows', count(*) FROM {TABLE}; "
            "SELECT 'secret', type, scope[1], secret_string FROM duckdb_secrets(redact=false) "
            f"WHERE name LIKE '{_VENDED}%'; "
            f"SELECT 'which', name FROM which_secret('{prefix}part-0.parquet', '{secret_type}'); COMMIT;",
            cli_args=("-unredacted",),
        )
        assert r.returncode == 0, r.stderr
        rows = _labelled(_strip_colour(r.stdout))
        assert rows.get("rows") == "50", r.stdout
        kind, scope, secret_string = rows["secret"].split("|", 2)
        assert (kind, scope) == (secret_type, prefix), r.stdout
        for part in expected:
            assert part in secret_string, r.stdout
        # The secret is the one found for a file under the prefix
        assert rows.get("which") == f"{_VENDED}unity__{CATALOG}__{SCHEMA}__{TABLE}__0", r.stdout


def test_a_lone_slash_prefix_is_scoped_to_the_table(data_files):
    """REGRESSION: a lone "/" prefix became the scope "/://", which matches no path, and dispatched as s3 with a
    misleading "no AWS credential" warning. It stands for the table's location now, as in duckdb-iceberg."""
    with MockUC(_vending_one(data_files, "/", {"gcs.oauth2.token": "ya29.mock"})) as srv:
        # The table's own credentials (temporary-table-credentials) come back empty from the mock.
        srv.set_storage_location("gs://mock-bucket/days")
        r = srv.query(
            f"BEGIN; SELECT 'rows', count(*) FROM {TABLE}; "
            f"SELECT 'secret', type, scope[1] FROM duckdb_secrets() WHERE name LIKE '{_VENDED}%'; COMMIT;"
        )
        assert r.returncode == 0, r.stderr
        out = _strip_colour(r.stdout)
        rows = _labelled(out)
        assert rows.get("rows") == "50", out
        assert rows.get("secret") == "gcs|gs://mock-bucket/days", out
        assert "vended no AWS credential" not in out, out


def test_azure_plan_credential_without_a_sas_makes_no_secret(data_files):
    with MockUC(_vending_one(data_files, _ABFSS, {"adls.sas-token-expires-at-ms.account": "0"})) as srv:
        r = srv.query(f"BEGIN; SELECT 'rows', count(*) FROM {TABLE}; {_secrets('inside')} COMMIT;")
        assert r.returncode == 0, r.stderr
        rows = _labelled(_strip_colour(r.stdout))
        assert rows.get("rows") == "50", r.stdout
        assert rows.get("inside") == "0|0", r.stdout
        assert "Unity Catalog vended no Azure SAS token" in _strip_colour(r.stdout), r.stdout


def test_a_plan_vending_nothing_keeps_another_attachments_secret(data_files):
    """REGRESSION: two attachments exposing the same catalog.schema.table named their /plan secrets alike, so a plan
    of the second that vends nothing dropped the first one's secret."""
    first_plan = _vending_one(data_files, "gs://first-bucket/", {"gcs.oauth2.token": "first-token"})
    with MockUC(first_plan) as first, MockUC(_vending_one(data_files, "gs://first-bucket/", {})) as second:

        def attach(srv, alias):
            return (
                f"CREATE SECRET {alias}_auth (TYPE unity_catalog, TOKEN 'tok', ENDPOINT '{srv.endpoint}'); "
                f"ATTACH '{CATALOG}' AS {alias} (TYPE unity_catalog, SECRET '{alias}_auth', READ_ONLY, "
                f"DEFAULT_SCHEMA '{SCHEMA}', use_irc_scan_plan true, API_IRC_ENDPOINT_OVERRIDE '{srv.endpoint}');"
            )

        r = _run_sql(
            f"{load_statements(build_dir())} {attach(first, 'first')} {attach(second, 'second')} BEGIN; "
            f"SELECT count(*) FROM first.{SCHEMA}.{TABLE}; SELECT count(*) FROM second.{SCHEMA}.{TABLE}; "
            f"SELECT 'secrets', string_agg(name, ',') FROM duckdb_secrets() WHERE name LIKE '{_VENDED}%'; COMMIT;"
        )
        assert r.returncode == 0, r.stderr
        rows = _labelled(_strip_colour(r.stdout))
        assert rows.get("secrets") == f"{_VENDED}first__{CATALOG}__{SCHEMA}__{TABLE}__0", r.stdout


def test_reapplied_credentials_leave_no_stale_secret(data_files):
    """REGRESSION: without the drop, a secret from an earlier scan in the transaction outlives a later plan that
    vends nothing for it (or fewer credentials), and is still found first."""
    token = {"gcs.oauth2.token": "ya29.mock"}
    plans = [
        [("gs://mock-bucket/days/", token), ("gs://mock-bucket/other/", token)],  # two secrets
        [("gs://mock-bucket/days/", token)],  # one: the second goes
        [("gs://mock-bucket/days/", {})],  # nothing vended: the first goes too
    ]

    def script(method, path, requests):
        if path.endswith("/plan"):
            creds = plans[min(len([x for x in requests if x["path"].endswith("/plan")]), len(plans)) - 1]
            return {
                "status": "completed",
                "plan-id": "p1",
                "file-scan-tasks": [_file_scan_task(f) for f in data_files],
                "storage-credentials": [{"prefix": p, "config": c} for p, c in creds],
            }
        return {}

    with MockUC(script) as srv:
        scans = " ".join(f"SELECT count(*) FROM {TABLE}; {_secrets(f'after{i}')}" for i in range(len(plans)))
        r = srv.query(f"BEGIN; {scans} COMMIT;")
        assert r.returncode == 0, r.stderr
        rows = _labelled(_strip_colour(r.stdout))
        assert [rows.get(f"after{i}") for i in range(len(plans))] == ["2|2", "1|1", "0|0"], r.stdout


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
# USE_IRC_SCAN_PLAN gates the whole path
#
# Every other test here opts in, so nothing covered the default. It has to be asserted as the
# ABSENCE of a /plan request: a broken gate would attempt scan planning, fail against a server
# that doesn't offer it, fall back to Delta and leave every suite green. The fallback that makes
# the feature safe is exactly what would hide the gate failing open.
#


@pytest.mark.parametrize("scan_plan", [None, "false"])
def test_scan_plan_not_attempted_unless_opted_in(data_files, scan_plan):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [_file_scan_task(f) for f in data_files]}
        return {}

    with MockUC(script) as srv:
        # The query itself fails -- the Delta path looks for a real Delta table at the mock's
        # storage_location, which doesn't exist. That's fine: the assertion is that we never asked
        # the IRC endpoint to plan.
        srv.query(f"SELECT count(*) FROM {TABLE};", scan_plan=scan_plan)
        assert srv.plan_requests() == [], f"scan plan attempted without opt-in ({scan_plan=})"


# -----------------------------------------------------------------------------
# Time travel
#
# The /plan request carries no version, so a read with an AT clause must take the Delta path.
# REGRESSION: unfixed, /plan answers with the latest files while the read binds the requested version's
# schema. The served table is a small Delta fixture whose v0 holds 10 rows; the plan's files hold 50, so
# the two paths cannot be confused.
#

_ICT_COLUMNS = [{"name": "i", "type_text": "bigint", "type_name": "LONG", "position": 0, "nullable": True}]


@pytest.mark.parametrize(
    "at_clause", ["VERSION => 0", "TIMESTAMP => TIMESTAMPTZ '2023-11-14 22:13:20+00'"], ids=["version", "timestamp"]
)
def test_time_travel_reads_the_delta_log_not_the_plan(data_files, at_clause):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {"status": "completed", "plan-id": "p1", "file-scan-tasks": [_file_scan_task(f) for f in data_files]}
        return {}

    with MockUC(script) as srv:
        srv.serve_delta_table(REPO / "data" / "ict_timetravel", _ICT_COLUMNS)
        r = srv.query(f"SELECT count(*) FROM {TABLE} AT ({at_clause});")
        assert r.returncode == 0, r.stderr
        assert scalar(r) == "10", r.stdout
        assert srv.plan_requests() == [], "a time-travel read was planned through /plan"


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
        r = srv.query(f"PREPARE pp AS SELECT 'R=' || count(*) FROM {TABLE};EXECUTE pp;EXECUTE pp;")
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
        r = srv.query(f"SET disabled_optimizers='filter_pushdown';SELECT count(*) FROM {TABLE} WHERE day = 'Mon';")
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
        r = srv.query(f"CREATE TEMP VIEW v AS SELECT id, day AS d FROM {TABLE};SELECT count(*) FROM v WHERE d = 'Mon';")
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


# -----------------------------------------------------------------------------
# The secret a /plan response vends lives for one transaction
#
# The planned scan turns each storage-credentials entry into a secret. A TEMPORARY one lives in a
# transactional catalog set, so two transactions that planned the same table conflicted on its name;
# a TRANSACTION secret is private to its transaction and gone at its end. The data files stay local:
# only the secret's lifetime is under test, not its use. One connection, so the conflict itself is
# not reproduced here.
#

_VENDED = "__internal_uc_scanplan__"


def _vending(data_files):
    def script(method, path, requests):
        if path.endswith("/plan"):
            return {
                "status": "completed",
                "plan-id": "p1",
                "file-scan-tasks": [_file_scan_task(f) for f in data_files],
                "storage-credentials": [
                    {
                        "prefix": "s3://mock-bucket/days/",
                        "config": {
                            "s3.access-key-id": "AKIAMOCK",
                            "s3.secret-access-key": "mock-secret",
                            "s3.session-token": "mock-token",
                            "client.region": "us-east-1",
                        },
                    }
                ],
            }
        return {}

    return script


def _secrets(label):
    """One `label|<vended secrets>|<of them in transaction storage>` line."""
    return (
        f"SELECT '{label}', count(*), count(*) FILTER (WHERE storage = 'transaction') FROM duckdb_secrets() "
        f"WHERE name LIKE '{_VENDED}%';"
    )


def _labelled(result):
    """`label|rest` lines of a CLI result (or of its stdout) as {label: rest}."""
    rows = {}
    for line in (result if isinstance(result, str) else result.stdout).splitlines():
        label, _, rest = line.strip().partition("|")
        if rest:
            rows[label] = rest
    return rows


@pytest.mark.parametrize("end", ["COMMIT", "ROLLBACK"])
def test_vended_secret_lives_for_its_transaction(data_files, end):
    with MockUC(_vending(data_files)) as srv:
        r = srv.query(f"BEGIN; SELECT 'rows', count(*) FROM {TABLE}; {_secrets('inside')} {end}; {_secrets('after')}")
        assert r.returncode == 0, r.stderr
        rows = _labelled(r)
        assert rows.get("rows") == "50", r.stdout
        assert rows.get("inside") == "1|1", f"no transaction-scoped vended secret inside BEGIN:\n{r.stdout}"
        assert rows.get("after") == "0|0", f"the vended secret outlived its transaction ({end}):\n{r.stdout}"


def test_vended_secret_of_an_autocommit_scan_is_gone_after_it(data_files):
    with MockUC(_vending(data_files)) as srv:
        r = srv.query(f"SELECT 'rows', count(*) FROM {TABLE}; {_secrets('after')}")
        assert r.returncode == 0, r.stderr
        rows = _labelled(r)
        assert rows.get("rows") == "50", r.stdout
        assert rows.get("after") == "0|0", f"the vended secret outlived its statement:\n{r.stdout}"
