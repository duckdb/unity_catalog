"""CREATE TABLE ... AS SELECT for a managed (catalog-managed) Unity Catalog table, against a mock catalog.

A CTAS without a location makes a MANAGED table: UC stages it (a table id, a location, the protocol and properties
it requires), delta writes version 0 with the query's rows through the catalog committer at the staging location
(one commit: protocol, metadata with `io.unitycatalog.tableId`, the add actions), and the table is registered with
UC afterwards. A failed registration keeps the files: UC owns the staging location and its id is in the log.
"""

import json
import re
from pathlib import Path

import pytest

from uc import delta_log_columns
from uc.capi import run_capi
from uc.mock import CATALOG, DROP, MockUnityCatalog, run

SCHEMA = "cmt"
TABLE_ID = "00000000-0000-0000-0000-0000000000ee"
VERSION_ZERO = Path("_delta_log") / "00000000000000000000.json"
# What UC requires of a catalog-managed table, and kernel's catalog committer checks at version 0
FEATURES = ["catalogManaged", "vacuumProtocolCheck", "inCommitTimestamp"]
REQUIRED = {"delta.enableInCommitTimestamps": "true", "io.unitycatalog.tableId": TABLE_ID}


def _version_zero(location):
    return [json.loads(line) for line in (location / VERSION_ZERO).read_text().splitlines()]


def _files(path):
    return sorted(str(p.relative_to(path)) for p in path.rglob("*")) if path.exists() else []


def _child_attach(location, table="t"):
    """Attaches `location` as UC attaches a staged managed table for its version 0: a child of `unity` that commits
    through it"""
    return (
        f"ATTACH '{location}' AS c (TYPE delta, CHILD_CATALOG_MODE true, INTERNAL_TABLE_NAME '{table}', "
        f"UNITY_TABLE_ID '{TABLE_ID}', PARENT_CATALOG 'unity', PARENT_CATALOG_SCHEMA '{SCHEMA}', PARENT_COMMIT true);"
    )


def _with(properties):
    return ", ".join(f"'{k}' = '{v}'" for k, v in properties.items())


def _cmt_properties(features=FEATURES):
    return {**REQUIRED, **{f"delta.feature.{f}": "supported" for f in features}}


# -----------------------------------------------------------------------------
# Delta under parent_commit, attached directly (what UC's managed CTAS plans through)
#


def test_delta_plans_ctas_through_the_catalog_committer(tmp_path):
    location = tmp_path / "t"
    with MockUnityCatalog([], schemas=[SCHEMA]) as uc:
        result, stdout = run(
            uc,
            _child_attach(location) + f"CREATE TABLE c.t WITH ({_with(_cmt_properties())}) AS "
            "SELECT range::INTEGER AS id, 'v' || range AS name FROM range(3);",
        )
        assert result.returncode == 0, result.stderr
    # Version 0 holds the rows, the catalog-managed protocol and the table id, at the published path
    actions = _version_zero(location)
    protocol = next(a["protocol"] for a in actions if "protocol" in a)
    assert "catalogManaged" in protocol["readerFeatures"], protocol
    assert "catalogManaged" in protocol["writerFeatures"], protocol
    metadata = next(a["metaData"] for a in actions if "metaData" in a)
    assert metadata["configuration"]["io.unitycatalog.tableId"] == TABLE_ID, metadata
    adds = [a["add"] for a in actions if "add" in a]
    assert adds and all(a["dataChange"] for a in adds), actions
    assert sum(json.loads(a["stats"])["numRecords"] for a in adds) == 3, adds
    # Version 0 is not a staged commit, and UC's commit API (from version 1) is not called
    assert not list(location.glob("_delta_log/_staged_commits/*")), _files(location)
    assert not [r for r in uc.requests if "/delta/v1/" in r], uc.requests


def test_delta_plans_a_partitioned_ctas_through_the_catalog_committer(tmp_path):
    location = tmp_path / "t"
    properties = {**_cmt_properties(), "delta.columnMapping.mode": "name"}
    with MockUnityCatalog([], schemas=[SCHEMA]) as uc:
        result, _ = run(
            uc,
            _child_attach(location) + f"CREATE TABLE c.t PARTITIONED BY (p) WITH ({_with(properties)}) AS "
            "SELECT range::INTEGER AS id, (range % 2)::INTEGER AS p, {'a': range} AS s FROM range(4);",
        )
        assert result.returncode == 0, result.stderr
    actions = _version_zero(location)
    metadata = next(a["metaData"] for a in actions if "metaData" in a)
    assert metadata["partitionColumns"] == ["p"], metadata
    assert metadata["configuration"]["delta.columnMapping.mode"] == "name", metadata
    adds = [a["add"] for a in actions if "add" in a]
    assert len(adds) == 2, adds
    assert sum(json.loads(a["stats"])["numRecords"] for a in adds) == 4, adds


def test_a_ctas_the_catalog_committer_refuses_leaves_no_files(tmp_path):
    # Without catalogManaged kernel refuses the commit through a catalog committer, after the query ran; delta then
    # removes the files the statement wrote
    location = tmp_path / "t"
    with MockUnityCatalog([], schemas=[SCHEMA]) as uc:
        result, _ = run(
            uc,
            _child_attach(location) + f"CREATE TABLE c.t WITH ({_with(REQUIRED)}) AS SELECT range AS id FROM range(3);",
        )
    assert result.returncode != 0
    assert "path-based and cannot be committed to with a catalog committer" in result.stderr, result.stderr
    assert not list(location.rglob("*.parquet")), _files(location)
    assert not (location / VERSION_ZERO).exists(), _files(location)


# -----------------------------------------------------------------------------
# Managed CTAS through UC
#

UC_FEATURES = ["catalogManaged", "v2Checkpoint", "vacuumProtocolCheck", "deletionVectors"]
REGISTER = f"/api/2.1/unity-catalog/delta/v1/catalogs/{CATALOG}/schemas/{SCHEMA}/tables"
STAGING = f"/api/2.1/unity-catalog/delta/v1/catalogs/{CATALOG}/schemas/{SCHEMA}/staging-tables"
REGISTER_FAILED = {"error": {"message": "registration blew up", "type": "InternalError", "code": 500}}
REGISTER_500 = "HTTP 500 for HTTP POST to"


def _conflict(name):
    return 409, {"error": {"message": f"Table already exists: {name}", "type": "AlreadyExistsException", "code": 409}}


class ManagedCatalog(MockUnityCatalog):
    """A catalog that stages managed tables under `root`, registers what it is sent, lists and loads what it
    registered, and keeps the commits UC ratifies (`add-commit`). `register(body) -> (status, dict) | DROP | None`
    answers the register call before the default; with `register_lands` the table is registered even then (a reply
    that was lost). `taken` names tables UC already has. `load_uuid` overrides the table id a load reports."""

    def __init__(self, root, register=None, taken=(), register_lands=False, load_uuid=None, suggested=None):
        self.tables = []
        self.staged = {}
        self.stagings = []
        self.registered = []
        self.commits = {}
        self.calls = []
        taken = set(taken)

        def find(name):
            return next((t for t in self.tables if t["name"] == name), None)

        def staged(name):
            # A new table id and location for every staging, as UC hands them out
            self.stagings.append(name)
            id_ = f"00000000-0000-0000-0000-{len(self.stagings):012d}"
            location = root / f"{name}-{id_}"
            self.staged[name] = (id_, location)
            return {
                "table-id": id_,
                "table-type": "MANAGED",
                "location": f"file://{location}",
                "required-protocol": {
                    "min-reader-version": 3,
                    "min-writer-version": 7,
                    "reader-features": UC_FEATURES,
                    "writer-features": UC_FEATURES + ["inCommitTimestamp"],
                },
                "required-properties": {
                    "delta.enableInCommitTimestamps": "true",
                    "delta.checkpointPolicy": "v2",
                    "delta.enableDeletionVectors": "true",
                    "io.unitycatalog.tableId": id_,
                },
                "suggested-properties": suggested if suggested is not None else {"delta.columnMapping.mode": "name"},
            }

        def register_table(body):
            id_, location = self.staged[body["name"]]
            columns = [
                {"name": n, "type_text": t, "type_name": tn, "position": i, "nullable": True, "type_json": js}
                for i, (n, t, tn, js) in enumerate(delta_log_columns(location))
            ]
            self.tables.append(
                {
                    "name": body["name"],
                    "catalog_name": CATALOG,
                    "schema_name": SCHEMA,
                    "table_type": "MANAGED",
                    "data_source_format": "DELTA",
                    "storage_location": f"file://{location}",
                    "table_id": id_,
                    "properties": {f"delta.feature.{f}": "supported" for f in UC_FEATURES},
                    "columns": columns,
                }
            )
            self.commits[body["name"]] = []

        def respond(method, path, body):
            path = path.split("?")[0]
            self.calls.append((method, path))
            if method == "POST" and path == STAGING:
                name = json.loads(body)["name"]
                if name in taken or find(name):
                    return _conflict(name)
                return 200, staged(name)
            if method == "POST" and path == REGISTER:
                request = json.loads(body)
                self.registered.append(request)
                answer = register(request) if register else None
                if register_lands and answer is not None:
                    register_table(request)
                if answer is not None:
                    return answer
                if find(request["name"]) or request["name"] in taken:
                    return _conflict(request["name"])
                register_table(request)
                return 200, {"metadata": {"etag": "e0", "table-uuid": self.staged[request["name"]][0]}, "commits": []}
            m = re.fullmatch(re.escape(REGISTER) + r"/(\w+)", path)
            if m:
                table = find(m.group(1))
                if table is None:
                    return 404, {
                        "error": {
                            "message": f"Table not found: {m.group(1)}",
                            "type": "NoSuchTableException",
                            "code": 404,
                        }
                    }
                commits = self.commits[table["name"]]
                version = commits[-1]["version"] if commits else 0
                if method == "GET":
                    return 200, {
                        "metadata": {
                            "etag": f"e{version}",
                            "table-uuid": load_uuid or table["table_id"],
                            "location": table["storage_location"],
                        },
                        "commits": commits,
                        "latest-table-version": version,
                    }
                update = json.loads(body)
                commits += [u["commit"] for u in update["updates"] if u["action"] == "add-commit"]
                return 200, {"metadata": {"etag": f"e{commits[-1]['version']}"}}
            return None

        super().__init__(self.tables, schemas=[SCHEMA], respond=respond)

    def location(self, name):
        return self.staged[name][1]


def _ok(uc, sql, **kwargs):
    result, stdout = run(uc, sql, **kwargs)
    assert result.returncode == 0, f"{sql}\n--- stderr ---\n{result.stderr}"
    return stdout.splitlines()


def _fails(uc, sql, **kwargs):
    result, _ = run(uc, sql, **kwargs)
    assert result.returncode != 0, f"{sql} succeeded"
    return result.stderr


def _parquet(location):
    return sorted(location.rglob("*.parquet"))


def test_managed_ctas_commits_version_zero_with_its_rows_and_registers_it(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        _ok(uc, f"CREATE TABLE unity.{SCHEMA}.m AS SELECT range::INTEGER AS id, 'v' || range AS name FROM range(3);")
        location = uc.location("m")
        # One commit at the published path, holding the protocol UC requires, its table id, and the rows
        assert sorted(p.name for p in (location / "_delta_log").glob("*.json")) == [VERSION_ZERO.name]
        actions = _version_zero(location)
        protocol = next(a["protocol"] for a in actions if "protocol" in a)
        for feature in ["catalogManaged", "vacuumProtocolCheck"]:
            assert feature in protocol["readerFeatures"] and feature in protocol["writerFeatures"], protocol
        assert "inCommitTimestamp" in protocol["writerFeatures"], protocol
        metadata = next(a["metaData"] for a in actions if "metaData" in a)
        assert metadata["configuration"]["io.unitycatalog.tableId"] == uc.staged["m"][0], metadata
        # UC's suggestion is taken: the files name the columns by physical name
        assert metadata["configuration"]["delta.columnMapping.mode"] == "name", metadata
        adds = [a["add"] for a in actions if "add" in a]
        assert sum(json.loads(a["stats"])["numRecords"] for a in adds) == 3, adds
        # Staged once, registered MANAGED at the staging location, after the write
        assert [c for c in uc.calls if c[0] == "POST"] == [("POST", STAGING), ("POST", REGISTER)], uc.calls
        [registered] = uc.registered
        assert (registered["name"], registered["table-type"]) == ("m", "MANAGED")
        assert registered["location"].removeprefix("file://") == str(location), registered
        assert "catalogManaged" in registered["protocol"]["reader-features"], registered

        # Read through the catalog, and an INSERT commits through it (each run is a new session)
        assert _ok(uc, f"SELECT id, name FROM unity.{SCHEMA}.m ORDER BY id;") == ["0|v0", "1|v1", "2|v2"]
        _ok(uc, f"INSERT INTO unity.{SCHEMA}.m VALUES (3, 'v3');")
        assert [c["version"] for c in uc.commits["m"]] == [1], uc.commits
        assert not (location / "_delta_log" / "00000000000000000001.json").exists()
        assert _ok(uc, f"SELECT count(*), sum(id) FROM unity.{SCHEMA}.m;") == ["4|6"]


def test_a_partitioned_nested_managed_ctas(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        _ok(
            uc,
            f"CREATE TABLE unity.{SCHEMA}.pn PARTITIONED BY (p) AS SELECT range::INTEGER AS id, "
            "(range % 2)::INTEGER AS p, {'a': range::INTEGER, 'b': ['x' || range]} AS s FROM range(4);",
        )
        [registered] = uc.registered
        assert registered["partition-columns"] == ["p"], registered
        assert _ok(uc, f"SELECT p, count(*), sum(s.a) FROM unity.{SCHEMA}.pn GROUP BY p ORDER BY p;") == [
            "0|2|2",
            "1|2|4",
        ]
        assert _ok(uc, f"SELECT s.b[1] FROM unity.{SCHEMA}.pn WHERE id = 3;") == ["x3"]


def test_without_a_suggested_mapping_the_files_use_logical_names(tmp_path):
    with ManagedCatalog(tmp_path, suggested={}) as uc:
        _ok(uc, f"CREATE TABLE unity.{SCHEMA}.plain AS SELECT 1::INTEGER AS id;")
        metadata = next(a["metaData"] for a in _version_zero(uc.location("plain")) if "metaData" in a)
        assert "delta.columnMapping.mode" not in metadata["configuration"], metadata
        assert _ok(uc, f"SELECT id FROM unity.{SCHEMA}.plain;") == ["1"]


def test_an_empty_query_makes_an_empty_managed_table(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        _ok(uc, f"CREATE TABLE unity.{SCHEMA}.e AS SELECT 1::INTEGER AS id WHERE false;")
        assert _ok(uc, f"SELECT count(*) FROM unity.{SCHEMA}.e;") == ["0"]


def test_a_name_uc_has_is_refused_at_staging_before_the_query_runs(tmp_path):
    with ManagedCatalog(tmp_path, taken={"t"}) as uc:
        err = _fails(uc, f"CREATE TABLE unity.{SCHEMA}.t AS SELECT error('the query ran') AS id;")
        assert "already exists" in err, err
        assert "the query ran" not in err, err
        assert uc.registered == []
        assert list(tmp_path.iterdir()) == []


def test_if_not_exists_on_a_name_uc_has_skips_the_query(tmp_path):
    with ManagedCatalog(tmp_path, taken={"t"}) as uc:
        _ok(uc, f"CREATE TABLE IF NOT EXISTS unity.{SCHEMA}.t AS SELECT error('the query ran') AS id;")
        assert uc.registered == []
        assert list(tmp_path.iterdir()) == []


def test_if_not_exists_on_a_listed_managed_table_skips_the_query(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        _ok(uc, f"CREATE TABLE unity.{SCHEMA}.t AS SELECT 1 AS id;")
        _ok(uc, f"CREATE TABLE IF NOT EXISTS unity.{SCHEMA}.t AS SELECT error('the query ran') AS id;")
        err = _fails(uc, f"CREATE TABLE unity.{SCHEMA}.t AS SELECT 2 AS id;")
        assert "already exists" in err, err
        assert len(uc.registered) == 1
        assert [c for c in uc.calls if c == ("POST", STAGING)] == [("POST", STAGING)], uc.calls


@pytest.mark.parametrize("if_not_exists", [False, True], ids=["plain", "if-not-exists"])
def test_a_refused_registration_keeps_the_files(tmp_path, if_not_exists):
    # UC owns the staging location and its id is in the log: nothing there is removed
    clause = "IF NOT EXISTS " if if_not_exists else ""
    with ManagedCatalog(tmp_path, register=lambda body: _conflict(body["name"])) as uc:
        sql = f"CREATE TABLE {clause}unity.{SCHEMA}.r AS SELECT range AS id FROM range(3);"
        if if_not_exists:
            _ok(uc, sql)
        else:
            assert "already exists" in _fails(uc, sql)
        location = uc.location("r")
        assert (location / VERSION_ZERO).exists()
        assert len(_parquet(location)) == 1, _files(location)


def test_a_failed_registration_keeps_the_files(tmp_path):
    with ManagedCatalog(tmp_path, register=lambda body: (500, REGISTER_FAILED)) as uc:
        err = _fails(uc, f"CREATE TABLE unity.{SCHEMA}.r AS SELECT range AS id FROM range(3);")
        assert REGISTER_500 in err and REGISTER in err, err
        location = uc.location("r")
        assert (location / VERSION_ZERO).exists()
        assert len(_parquet(location)) == 1, _files(location)


@pytest.mark.parametrize("answer", [(500, REGISTER_FAILED), DROP], ids=["server-error", "connection-closed"])
def test_a_registration_that_went_through_counts_as_created(tmp_path, answer):
    with ManagedCatalog(tmp_path, register=lambda body: answer, register_lands=True) as uc:
        _ok(uc, f"CREATE TABLE unity.{SCHEMA}.lost AS SELECT 7 AS id;")
        assert _ok(uc, f"SELECT id FROM unity.{SCHEMA}.lost;") == ["7"]


def test_a_registration_under_another_table_id_is_an_error(tmp_path):
    other = "00000000-0000-0000-0000-0000000000bb"
    with ManagedCatalog(
        tmp_path, register=lambda body: (500, REGISTER_FAILED), register_lands=True, load_uuid=other
    ) as uc:
        err = _fails(uc, f"CREATE TABLE unity.{SCHEMA}.lost AS SELECT 7 AS id;")
        assert REGISTER_500 in err and REGISTER in err, err
        assert (uc.location("lost") / VERSION_ZERO).exists()


def test_a_query_that_fails_midway_leaves_no_files_and_registers_nothing(tmp_path):
    # Delta removes what the uncommitted write wrote; the staged table is left to UC
    with ManagedCatalog(tmp_path) as uc:
        err = _fails(
            uc,
            "SET threads = 1; SET preserve_insertion_order = true; "
            f"CREATE TABLE unity.{SCHEMA}.boom AS "
            "SELECT CASE WHEN range < 300000 THEN range ELSE error('boom at the end') END AS id FROM range(400000);",
        )
        assert "boom at the end" in err, err
        assert uc.registered == []
        location = uc.location("boom")
        assert _parquet(location) == [], _files(location)
        assert not (location / VERSION_ZERO).exists()


def test_a_type_delta_cannot_store_is_refused_and_registers_nothing(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        err = _fails(uc, f"CREATE TABLE unity.{SCHEMA}.u AS SELECT 1::UHUGEINT AS id;")
        assert "does not support column 'id' of type UHUGEINT" in err, err
        assert uc.registered == []
        # Delta refuses it while planning, after UC staged the table (staging is at plan time)
        assert ("POST", STAGING) in uc.calls, uc.calls


def test_or_replace_is_refused_before_staging(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        err = _fails(uc, f"CREATE OR REPLACE TABLE unity.{SCHEMA}.o AS SELECT 1 AS id;")
        assert "CREATE OR REPLACE TABLE ... AS is not supported" in err, err
        assert not [c for c in uc.calls if c[0] == "POST"], uc.calls


def test_explain_stages_but_writes_and_registers_nothing(tmp_path):
    # Staging happens at plan time (delta needs the location to build the create transaction), so EXPLAIN leaves a
    # staged table in UC (a documented limit)
    with ManagedCatalog(tmp_path) as uc:
        plan = "\n".join(_ok(uc, f"EXPLAIN CREATE TABLE unity.{SCHEMA}.x AS SELECT 1 AS id;"))
        assert "Uc Create Table As" in plan and "Delta Create Table As" in plan, plan
        assert uc.registered == []
        assert ("POST", STAGING) in uc.calls
        assert not (uc.location("x") / VERSION_ZERO).exists()


# -----------------------------------------------------------------------------
# API clients: prepare, then execute (a second plan)
#

CTAS_M = f"CREATE TABLE unity.{SCHEMA}.m AS SELECT range::INTEGER AS id FROM range(3)"


def test_an_api_client_that_prepares_then_executes_stages_once(tmp_path):
    # Execute plans the statement again, in the next transaction; it reuses the staging the prepare made
    with ManagedCatalog(tmp_path) as uc:
        results = run_capi(uc, [("prepare", CTAS_M), ("execute",), ("query", f"SELECT count(*) FROM unity.{SCHEMA}.m")])
        assert results[0] == "prepared" and not isinstance(results[1], dict), results
        assert results[2] == [["3"]], results
        assert uc.stagings == ["m"], uc.calls
        [registered] = uc.registered
        assert registered["location"].removeprefix("file://") == str(uc.location("m"))


def test_a_prepared_ctas_that_never_runs_stages_once_and_registers_nothing(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        assert run_capi(uc, [("prepare", CTAS_M)]) == ["prepared"]
        assert uc.stagings == ["m"]
        assert uc.registered == []


def test_another_statement_for_the_name_reuses_an_unrun_staging(tmp_path):
    # A staging is bound to the name only: a statement that never ran leaves it to the next CTAS of that table
    with ManagedCatalog(tmp_path) as uc:
        other = f"CREATE TABLE unity.{SCHEMA}.m AS SELECT 42::INTEGER AS id"
        results = run_capi(
            uc, [("prepare", CTAS_M), ("prepare", other), ("execute",), ("query", f"SELECT id FROM unity.{SCHEMA}.m")]
        )
        assert results[3] == [["42"]], results
        assert uc.stagings == ["m"], uc.calls
        assert len(uc.registered) == 1


def test_a_reuse_keeps_the_staging_for_the_next_transaction(tmp_path):
    # Each reuse starts its lifetime again: prepare twice, then execute, still stages once
    with ManagedCatalog(tmp_path) as uc:
        results = run_capi(uc, [("prepare", CTAS_M), ("prepare", CTAS_M), ("execute",)])
        assert not isinstance(results[2], dict), results
        assert uc.stagings == ["m"], uc.calls


def test_a_rolled_back_transaction_ages_a_staging_like_a_commit(tmp_path):
    with ManagedCatalog(tmp_path) as uc:
        results = run_capi(uc, [("query", "BEGIN"), ("prepare", CTAS_M), ("query", "ROLLBACK"), ("execute",)])
        assert not isinstance(results[3], dict), results
        assert uc.stagings == ["m"], uc.calls
        assert len(uc.registered) == 1


def test_an_execute_after_other_statements_stages_again(tmp_path):
    # A staging lives into the transaction after its prepare, no further (a documented limit)
    with ManagedCatalog(tmp_path) as uc:
        results = run_capi(uc, [("prepare", CTAS_M), ("query", "SELECT 1"), ("execute",)])
        assert not isinstance(results[2], dict), results
        assert uc.stagings == ["m", "m"], uc.calls
        [registered] = uc.registered
        assert registered["location"].removeprefix("file://") == str(uc.location("m"))


def test_a_catalog_attached_again_under_the_same_name_stages_afresh(tmp_path):
    # Another connection re-attaches `unity` to another endpoint between the prepare and the execute: the staging
    # made in the first catalog is not that catalog's to use
    with ManagedCatalog(tmp_path / "a") as first, ManagedCatalog(tmp_path / "b") as second:
        results = run_capi(
            first,
            [
                (
                    "query",
                    f"CREATE SECRET second (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{second.endpoint}', "
                    "AWS_REGION 'us-east-2')",
                ),
                ("prepare", CTAS_M),
                ("connection", 1),
                ("query", "DETACH unity"),
                ("query", "ATTACH 'duck' AS unity (TYPE unity_catalog, SECRET second, DEFAULT_SCHEMA 'cmt')"),
                ("connection", 0),
                ("execute",),
            ],
        )
        assert not isinstance(results[6], dict), results
        assert first.stagings == ["m"] and first.registered == [], first.calls
        assert second.stagings == ["m"], second.calls
        [registered] = second.registered
        assert registered["location"].removeprefix("file://") == str(second.location("m"))


def test_executing_a_prepared_ctas_again_finds_the_table(tmp_path):
    # The second execute plans again; the session now lists the name, so it is taken, and nothing is staged or
    # registered again
    with ManagedCatalog(tmp_path) as uc:
        results = run_capi(uc, [("prepare", CTAS_M), ("execute",), ("execute",)])
        assert "already exists" in results[2]["error"], results
        assert uc.stagings == ["m"], uc.calls
        assert len(uc.registered) == 1


# -----------------------------------------------------------------------------
# The warning when a registration cannot be confirmed
#

WARNINGS = "SELECT message FROM duckdb_logs() WHERE log_level = 'WARNING' AND message LIKE 'CreateTable %'"


@pytest.mark.parametrize(
    "sql",
    [f"CREATE TABLE unity.{SCHEMA}.w (id INTEGER)", f"CREATE TABLE unity.{SCHEMA}.w AS SELECT 1::INTEGER AS id"],
    ids=["create", "ctas"],
)
def test_an_unconfirmed_registration_warns_with_the_staged_location_and_id(tmp_path, sql):
    with ManagedCatalog(tmp_path, register=lambda body: (500, REGISTER_FAILED)) as uc:
        results = run_capi(
            uc,
            [
                ("query", "CALL enable_logging(level = 'WARNING', storage = 'memory')"),
                ("query", sql),
                ("query", WARNINGS),
            ],
        )
        assert REGISTER_500 in results[1]["error"], results
        id_, location = uc.staged["w"]
        [[message]] = results[2]
        assert message.startswith(
            f"CreateTable {CATALOG}.{SCHEMA}.w could not confirm registration; version 0 kept at file://{location} "
            f"(staged table id {id_}): "
        ), message
        # The error's message, not its JSON form
        assert REGISTER_500 in message and not message.rstrip().endswith("}"), message
