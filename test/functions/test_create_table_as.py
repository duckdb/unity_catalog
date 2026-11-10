"""CREATE TABLE ... AS SELECT through Unity Catalog, against a mock catalog.

A CTAS with a location makes an EXTERNAL table: the name is checked with UC first, delta writes version 0 with the
query's rows at the location (one commit: protocol, metadata and the add actions), and the table is registered with
UC afterwards. A registration that fails removes what the statement wrote. A CTAS without a location would make a
managed table, which is refused for now.

The mock keeps what it is told: a registered table is listed and loadable afterwards, so a later statement, or a
fresh session, reads it through the catalog.
"""

import json
import re
import shutil
from pathlib import Path

import pytest

from uc import delta_log_columns
from uc.mock import CATALOG, DROP, MockUnityCatalog, delta_table, run

REGISTER = f"/delta/v1/catalogs/{CATALOG}/schemas/plain/tables"
STAGING = f"/delta/v1/catalogs/{CATALOG}/schemas/plain/staging-tables"
VERSION_ZERO = Path("_delta_log") / "00000000000000000000.json"


def _reported_columns(location):
    return [
        {"name": name, "type_text": text, "type_name": type_name, "position": i, "nullable": True, "type_json": js}
        for i, (name, text, type_name, js) in enumerate(delta_log_columns(location))
    ]


class Catalog(MockUnityCatalog):
    """A catalog that registers what it is sent, in any schema of `schemas`. `register(body) -> (status, dict) |
    DROP | None` answers the register call before the default, which accepts it; with `register_lands`, the table
    is registered even when `register` answers otherwise (a registration whose reply was lost). `taken` names
    tables UC already has (in every schema)."""

    def __init__(self, register=None, taken=(), register_lands=False, schemas=("plain",)):
        self.tables = []
        self.registered = []
        self.taken = set(taken)
        self.calls = []

        def find(schema, name):
            return next((t for t in self.tables if (t["schema_name"], t["name"]) == (schema, name)), None)

        def register_table(schema, body):
            location = body["location"].removeprefix("file://")
            # UC lists a registered table with the properties it derived from the protocol
            entry = delta_table(body["name"], location, _reported_columns(location), schema=schema)
            self.tables.append({**entry, "properties": {}})

        def respond(method, path, body):
            path = path.split("?")[0]
            self.calls.append((method, path))
            if method == "GET" and (m := re.fullmatch(rf"/api/2.1/unity-catalog/tables/{CATALOG}\.(\w+)\.(\w+)", path)):
                schema, name = m.groups()
                if find(schema, name) or name in self.taken:
                    return 200, {"name": name}
                return 404, {"error_code": "TABLE_DOES_NOT_EXIST", "message": f"Table not found: {name}"}
            m = re.fullmatch(
                rf"/api/2.1/unity-catalog/delta/v1/catalogs/{CATALOG}/schemas/(\w+)/tables(?:/(\w+))?", path
            )
            if m and method == "GET" and m.group(2):
                table = find(*m.groups())
                if table:
                    metadata = {"etag": "e", "table-uuid": table["table_id"], "location": table["storage_location"]}
                    return 200, {"metadata": metadata, "commits": []}
                return 404, {
                    "error": {"message": f"Table not found: {m.group(2)}", "type": "NoSuchTableException", "code": 404}
                }
            if m and method == "POST" and not m.group(2):
                schema = m.group(1)
                request = json.loads(body)
                self.registered.append(request)
                answer = register(request) if register else None
                if register_lands and answer is not None:
                    register_table(schema, request)
                if answer is not None:
                    return answer
                if find(schema, request["name"]) or request["name"] in self.taken:
                    return 409, {
                        "error": {
                            "message": f"Table already exists: {request['name']}",
                            "type": "AlreadyExistsException",
                            "code": 409,
                        }
                    }
                register_table(schema, request)
                metadata = {
                    "etag": "e",
                    "table-uuid": "00000000-0000-0000-0000-000000000001",
                    "location": request["location"],
                }
                return 200, {"metadata": metadata, "commits": []}
            return None

        super().__init__(self.tables, schemas=list(schemas), respond=respond)


def _ok(uc, sql, **kwargs):
    result, stdout = run(uc, sql, **kwargs)
    assert result.returncode == 0, f"{sql}\n--- stderr ---\n{result.stderr}"
    return stdout.splitlines()


def _fails(uc, sql, **kwargs):
    result, _ = run(uc, sql, **kwargs)
    assert result.returncode != 0, f"{sql} succeeded"
    return result.stderr


def _version_zero(location):
    return [json.loads(line) for line in (location / VERSION_ZERO).read_text().splitlines()]


def _files(path):
    return sorted(str(p.relative_to(path)) for p in path.rglob("*")) if path.exists() else []


def _writes(uc):
    return [c for c in uc.calls if c[0] == "POST"]


# -----------------------------------------------------------------------------
# Creating it
#


def test_ctas_writes_version_zero_with_its_rows_and_registers_it(tmp_path):
    location = tmp_path / "t"
    with Catalog() as uc:
        _ok(
            uc,
            f"CREATE TABLE unity.plain.t WITH (location = '{location}') AS "
            "SELECT range AS id, 'v' || range AS name FROM range(3);",
        )
        # One commit, holding the protocol, the schema and the files with the rows
        assert sorted(p.name for p in (location / "_delta_log").glob("*.json")) == [VERSION_ZERO.name]
        actions = _version_zero(location)
        kinds = [next(iter(a)) for a in actions]
        assert {"protocol", "metaData", "add"} <= set(kinds), kinds
        adds = [a["add"] for a in actions if "add" in a]
        assert sum(json.loads(a["stats"])["numRecords"] for a in adds) == 3
        assert all(a["dataChange"] for a in adds)
        # Named first, registered after the write, never staged
        assert [c for c in uc.calls if c[1].endswith("tables/duck.plain.t")] == [
            ("GET", "/api/2.1/unity-catalog/tables/duck.plain.t")
        ]
        assert not any(c[1].endswith(STAGING) for c in uc.calls)
        [registered] = uc.registered
        assert (registered["name"], registered["table-type"]) == ("t", "EXTERNAL")
        assert registered["location"] == str(location)
        assert [f["name"] for f in registered["columns"]["fields"]] == ["id", "name"]
        assert registered["partition-columns"] == []
        assert "catalogManaged" not in registered["protocol"]["reader-features"]

        # Readable, and writable, through the catalog afterwards (each run is a new session)
        assert _ok(uc, "SELECT id, name FROM unity.plain.t ORDER BY id;") == ["0|v0", "1|v1", "2|v2"]
        assert _ok(uc, "INSERT INTO unity.plain.t VALUES (3, 'v3'); SELECT count(*) FROM unity.plain.t;") == ["4"]


def test_a_partitioned_ctas_registers_its_partition_column(tmp_path):
    location = tmp_path / "part"
    with Catalog() as uc:
        _ok(
            uc,
            f"CREATE TABLE unity.plain.part PARTITIONED BY (p) WITH (location = '{location}') AS "
            "SELECT * FROM (VALUES (1, 'a b'), (2, 'x%y'), (3, 'a b'), (4, NULL)) v(id, p);",
        )
        [registered] = uc.registered
        assert registered["partition-columns"] == ["p"]
        assert _ok(uc, "SELECT p, count(*) FROM unity.plain.part GROUP BY p ORDER BY p NULLS LAST;") == [
            "a b|2",
            "x%y|1",
            "NULL|1",
        ]


def test_an_empty_query_makes_an_empty_table(tmp_path):
    location = tmp_path / "empty"
    with Catalog() as uc:
        _ok(
            uc, f"CREATE TABLE unity.plain.empty WITH (location = '{location}') AS SELECT 1::INTEGER AS id WHERE false;"
        )
        assert _ok(uc, "SELECT count(*) FROM unity.plain.empty;") == ["0"]
        assert len(uc.registered) == 1


def test_inside_a_transaction(tmp_path):
    # Statement-scoped as plain CREATE is: the table exists once the statement ends, and ROLLBACK leaves it
    location = tmp_path / "txn"
    with Catalog() as uc:
        _ok(uc, f"BEGIN; CREATE TABLE unity.plain.txn WITH (location = '{location}') AS SELECT 42 AS id; ROLLBACK;")
        assert len(uc.registered) == 1
        assert _ok(uc, "SELECT id FROM unity.plain.txn;") == ["42"]


# -----------------------------------------------------------------------------
# A name UC already has
#


def test_a_taken_name_is_refused_before_anything_is_written(tmp_path):
    location = tmp_path / "taken"
    with Catalog(taken={"taken"}) as uc:
        err = _fails(uc, f"CREATE TABLE unity.plain.taken WITH (location = '{location}') AS SELECT 1 AS id;")
        assert "already exists" in err, err
        assert not location.exists()
        assert uc.registered == []


def test_if_not_exists_on_a_taken_name_skips_the_query(tmp_path):
    location = tmp_path / "taken"
    with Catalog(taken={"taken"}) as uc:
        # The query would fail if it ran
        _ok(
            uc,
            f"CREATE TABLE IF NOT EXISTS unity.plain.taken WITH (location = '{location}') AS "
            "SELECT error('the query ran') AS id;",
        )
        assert not location.exists()
        assert uc.registered == []


def test_if_not_exists_on_a_listed_table_skips_the_query(tmp_path):
    location = tmp_path / "t"
    with Catalog() as uc:
        _ok(uc, f"CREATE TABLE unity.plain.t WITH (location = '{location}') AS SELECT 1 AS id;")
        _ok(
            uc,
            f"CREATE TABLE IF NOT EXISTS unity.plain.t WITH (location = '{tmp_path / 'again'}') AS "
            "SELECT error('the query ran') AS id;",
        )
        err = _fails(uc, f"CREATE TABLE unity.plain.t WITH (location = '{tmp_path / 'again'}') AS SELECT 1 AS id;")
        assert "already exists" in err, err
        assert not (tmp_path / "again").exists()
        assert len(uc.registered) == 1


# -----------------------------------------------------------------------------
# Registration refused
#
# What the statement committed is removed only when UC refused the name outright (taken meanwhile) and no table in
# the catalog is registered at the location. Whenever that cannot be established -- a refusal of the location, a
# lost or unclear reply, another table at the location -- the files stay.
#

ALREADY_EXISTS = (
    409,
    {"error": {"message": "Table already exists: raced", "type": "AlreadyExistsException", "code": 409}},
)
REFUSED = (
    400,
    {
        "error": {
            "message": "Input path overlaps with managed storage.",
            "type": "InvalidParameterValueException",
            "code": 400,
        }
    },
)
FAILED = (500, {"error": {"message": "registration blew up", "type": "InternalError", "code": 500}})


@pytest.mark.parametrize("existing_directory", [False, True], ids=["new-directory", "existing-directory"])
def test_a_name_taken_at_registration_removes_what_was_written(tmp_path, existing_directory):
    location = tmp_path / "raced"
    if existing_directory:
        location.mkdir()
        (location / "keep.txt").write_text("not ours")
    with Catalog(register=lambda body: ALREADY_EXISTS) as uc:
        err = _fails(
            uc,
            f"CREATE TABLE unity.plain.raced PARTITIONED BY (p) WITH (location = '{location}') AS "
            "SELECT range AS id, ['a b', 'x%y', 'q/r', NULL][range % 4 + 1] AS p FROM range(10);",
        )
        assert "already exists" in err, err
        assert len(uc.registered) == 1
        # The log, the data files, the partition directories (whose names the log escapes once more than the
        # filesystem); the directory itself only if the statement made it
        assert _files(location) == (["keep.txt"] if existing_directory else [])
        assert location.exists() == existing_directory
        # IF NOT EXISTS: someone else's table has the name, so this statement created nothing
        _ok(uc, f"CREATE TABLE IF NOT EXISTS unity.plain.raced WITH (location = '{location}') AS SELECT 1 AS id;")
        assert _files(location) == (["keep.txt"] if existing_directory else [])
    # So the location takes a create again
    with Catalog() as uc:
        _ok(uc, f"CREATE TABLE unity.plain.raced WITH (location = '{location}') AS SELECT 1 AS id;")


def test_a_cleanup_keeps_a_file_another_writer_put_in_the_log(tmp_path):
    # Directories go only if empty, and only them: never with what is in them
    location = tmp_path / "shared_log"

    def another_writer_then_taken(body):
        (location / "_delta_log" / "other_writer.txt").write_text("not ours")
        return ALREADY_EXISTS

    with Catalog(register=another_writer_then_taken) as uc:
        _fails(uc, f"CREATE TABLE unity.plain.shared_log WITH (location = '{location}') AS SELECT 1 AS id;")
    assert _files(location) == ["_delta_log", "_delta_log/other_writer.txt"]


def test_a_cleanup_removes_nested_partition_directories(tmp_path):
    location = tmp_path / "nested"
    with Catalog(register=lambda body: ALREADY_EXISTS) as uc:
        _fails(
            uc,
            f"CREATE TABLE unity.plain.nested PARTITIONED BY (p, q) WITH (location = '{location}') AS "
            "SELECT range AS id, (range % 2)::VARCHAR AS p, ['a b', NULL][range % 2 + 1] AS q FROM range(8);",
        )
        assert _files(location) == []
        assert not location.exists()


@pytest.mark.parametrize(
    "answer, message",
    [(REFUSED, "overlaps with managed storage"), (FAILED, "HTTP 500"), (DROP, "")],
    ids=["location-refused", "server-error", "reply-lost"],
)
@pytest.mark.parametrize("ctas", [True, False], ids=["ctas", "create"])
def test_an_unclear_or_location_refusal_keeps_the_files(tmp_path, answer, message, ctas):
    location = tmp_path / "kept"
    with Catalog(register=lambda body: answer) as uc:
        sql = (
            f"CREATE TABLE unity.plain.kept WITH (location = '{location}') AS SELECT 7 AS id;"
            if ctas
            else f"CREATE TABLE unity.plain.kept (id INTEGER) WITH (location = '{location}');"
        )
        err = _fails(uc, "CALL enable_logging(level = 'WARNING'); " + sql)
        assert message in err, err
        assert (location / VERSION_ZERO).exists()
        if ctas:
            adds = [a["add"]["path"] for a in _version_zero(location) if "add" in a]
            assert adds and all((location / a).exists() for a in adds)


def test_a_cleanup_keeps_files_another_table_is_registered_at(tmp_path):
    # Another client registers the just-committed location under its own name; then UC refuses ours
    location = tmp_path / "adopted"

    def adopt_then_taken(body):
        entry = delta_table("adopted", location, _reported_columns(location))
        uc.tables.append({**entry, "properties": {}})
        return ALREADY_EXISTS

    for ctas in [True, False]:
        if location.exists():
            shutil.rmtree(location)
        with Catalog(register=adopt_then_taken) as uc:
            sql = (
                f"CREATE TABLE unity.plain.original WITH (location = '{location}') AS SELECT 42 AS id;"
                if ctas
                else f"CREATE TABLE unity.plain.original (id INTEGER) WITH (location = '{location}');"
            )
            _fails(uc, sql)
            assert (location / VERSION_ZERO).exists()
            assert _ok(uc, "SELECT count(*) FROM unity.plain.adopted;") == (["1"] if ctas else ["0"])


def test_a_registration_that_went_through_counts_as_created(tmp_path):
    # The register call fails as seen from here, but UC has the table at this location: the files stay
    location = tmp_path / "landed"
    with Catalog(register=lambda body: DROP, register_lands=True) as uc:
        result, _ = run(uc, f"CREATE TABLE unity.plain.landed WITH (location = '{location}') AS SELECT 7 AS id;")
        assert result.returncode == 0, result.stderr
        assert (location / VERSION_ZERO).exists()
        assert _ok(uc, "SELECT id FROM unity.plain.landed;") == ["7"]


def test_a_registration_that_went_through_is_recognized_in_another_spelling(tmp_path):
    # The reply is lost, and UC spells the location its own way: the same place, so the same table, and the files
    # stay
    root = tmp_path / "root"
    root.mkdir()
    canonical = root / "landed"

    def lands_then_lost(body):
        assert body["location"] == str(canonical)
        entry = delta_table("landed", f"{root}/./landed/", _reported_columns(canonical))
        uc.tables.append({**entry, "properties": {}})
        return DROP

    with Catalog(register=lands_then_lost) as uc:
        _ok(uc, f"CREATE TABLE unity.plain.landed WITH (location = 'file://{root}//x/../landed/') AS SELECT 7 AS id;")
        assert (canonical / VERSION_ZERO).exists()
        assert _ok(uc, "SELECT id FROM unity.plain.landed;") == ["7"]


# -----------------------------------------------------------------------------
# Refused before anything is written
#


@pytest.mark.parametrize(
    "sql, message",
    [
        (
            "CREATE TABLE unity.plain.m AS SELECT 1 AS id",
            "CREATE TABLE AS for a managed Unity Catalog table is not supported yet",
        ),
        (
            "CREATE OR REPLACE TABLE unity.plain.m WITH (location = '{loc}') AS SELECT 1 AS id",
            "CREATE OR REPLACE TABLE ... AS is not supported",
        ),
        (
            "CREATE TABLE unity.plain.m WITH (location = '{loc}', owner = 'me') AS SELECT 1 AS id",
            'Unrecognized option "owner"',
        ),
        (
            "CREATE TABLE unity.plain.m SORTED BY (id) WITH (location = '{loc}') AS SELECT 1 AS id",
            "SORTED BY is not supported",
        ),
        (
            "CREATE TABLE unity.plain.m WITH (location = 'relative/m') AS SELECT 1 AS id",
            "must be an absolute path or a URI",
        ),
        (
            "CREATE TABLE unity.plain.m WITH (location = '{loc}') AS SELECT 1::UHUGEINT AS id",
            "does not support column 'id' of type UHUGEINT",
        ),
    ],
    ids=["managed", "or-replace", "unknown-option", "sorted-by", "relative-location", "unstorable-type"],
)
def test_refused_before_any_write(tmp_path, sql, message):
    location = tmp_path / "m"
    with Catalog() as uc:
        err = _fails(uc, sql.format(loc=location) + ";")
        assert message in err, err
        assert not location.exists()
        assert uc.registered == []
        assert not any(c[1].endswith(STAGING) for c in uc.calls), uc.calls


def test_a_read_only_catalog_refuses_it(tmp_path):
    location = tmp_path / "ro"
    with Catalog() as uc:
        err = _fails(
            uc,
            f"CREATE TABLE unity.plain.ro WITH (location = '{location}') AS SELECT 1 AS id;",
            attach_options=", READ_ONLY",
        )
        assert "read only" in err, err
        assert not location.exists()


def test_a_query_that_fails_midway_leaves_nothing(tmp_path):
    # Delta undoes the write; UC never hears of the table. One thread, so files are written before the error
    location = tmp_path / "boom"
    with Catalog() as uc:
        err = _fails(
            uc,
            "SET threads = 1; SET preserve_insertion_order = true; "
            f"CREATE TABLE unity.plain.boom WITH (location = '{location}') AS "
            "SELECT CASE WHEN range < 300000 THEN range ELSE error('boom at the end') END AS id "
            "FROM range(400000);",
        )
        assert "boom at the end" in err, err
        assert uc.registered == []
        assert _files(location) == []


def test_explain_writes_and_registers_nothing(tmp_path):
    location = tmp_path / "explained"
    with Catalog() as uc:
        plan = "\n".join(
            _ok(uc, f"EXPLAIN CREATE TABLE unity.plain.explained WITH (location = '{location}') AS SELECT 1 AS id;")
        )
        assert "Uc Create Table As" in plan and "Delta Create Table As" in plan, plan
        assert uc.registered == []
        assert _files(location) == []


# -----------------------------------------------------------------------------
# A name or a location someone else has
#


def test_if_not_exists_on_a_name_taken_at_registration_lists_the_winner(tmp_path):
    location = tmp_path / "loser"
    winner = tmp_path / "winner"

    with Catalog() as setup:
        _ok(setup, f"CREATE TABLE unity.plain.winner WITH (location = '{winner}') AS SELECT 7 AS id;")

    def won_meanwhile(body):
        uc.tables.append({**delta_table("raced", winner, _reported_columns(winner)), "properties": {}})
        return ALREADY_EXISTS

    with Catalog(register=won_meanwhile) as uc:
        assert _ok(
            uc,
            f"CREATE TABLE IF NOT EXISTS unity.plain.raced WITH (location = '{location}') AS SELECT 42 AS id; "
            "SELECT id FROM unity.plain.raced;",
        ) == ["7"]
        assert _files(location) == []


def test_if_not_exists_on_a_name_unlisted_at_bind_lists_it(tmp_path):
    winner = tmp_path / "winner"
    with Catalog() as setup:
        _ok(setup, f"CREATE TABLE unity.plain.t WITH (location = '{winner}') AS SELECT 7 AS id;")
        entry = setup.tables[0]
    tables = []

    def respond(method, path, body):
        # UC has the table, but this session listed the schema before it appeared
        if method == "GET" and path == f"/api/2.1/unity-catalog/tables/{CATALOG}.plain.t":
            tables.append(entry)
            return 200, {"name": "t"}
        return None

    with MockUnityCatalog(tables, schemas=["plain"], respond=respond) as uc:
        assert _ok(
            uc,
            f"CREATE TABLE IF NOT EXISTS unity.plain.t WITH (location = '{tmp_path / 'unused'}') "
            "AS SELECT error('the query ran') AS id; SELECT id FROM unity.plain.t;",
        ) == ["7"]
        assert not (tmp_path / "unused").exists()


def _existing_table(tmp_path, name="theirs"):
    """A Delta table at a location UC does not have under any name."""
    location = tmp_path / name
    with Catalog() as setup:
        _ok(setup, f"CREATE TABLE unity.plain.mine WITH (location = '{location}') AS SELECT 1 AS id;")
    return location


@pytest.mark.parametrize(
    "sql",
    [
        "CREATE TABLE unity.plain.t (id INTEGER) WITH (location = '{loc}')",
        "CREATE TABLE IF NOT EXISTS unity.plain.t (id INTEGER) WITH (location = '{loc}')",
        "CREATE TABLE unity.plain.t WITH (location = '{loc}') AS SELECT 2 AS id",
        "CREATE TABLE IF NOT EXISTS unity.plain.t WITH (location = '{loc}') AS SELECT 2 AS id",
    ],
    ids=["create", "create-if-not-exists", "ctas", "ctas-if-not-exists"],
)
def test_a_location_that_holds_a_delta_table_is_refused(tmp_path, sql):
    location = _existing_table(tmp_path)
    before = _files(location)
    with Catalog() as uc:
        err = _fails(uc, sql.format(loc=location) + ";")
        assert f"'{location}' already holds a Delta table (its _delta_log has files); choose another location" in err
        # Refused before anything is written or registered: the table there is left as it was
        assert uc.registered == []
    assert _files(location) == before


def test_a_log_holding_only_staged_commits_counts_as_a_table(tmp_path):
    location = tmp_path / "staged"
    (location / "_delta_log" / "_commits").mkdir(parents=True)
    (location / "_delta_log" / "_commits" / "00000000000000000001.uuid.json").write_text("{}")
    with Catalog() as uc:
        err = _fails(uc, f"CREATE TABLE unity.plain.t (id INTEGER) WITH (location = '{location}');")
        assert "already holds a Delta table" in err, err


@pytest.mark.parametrize("if_not_exists", [False, True], ids=["create", "create-if-not-exists"])
def test_an_empty_delta_log_directory_takes_a_create(tmp_path, if_not_exists):
    location = tmp_path / "empty_log"
    (location / "_delta_log").mkdir(parents=True)
    with Catalog() as uc:
        _ok(
            uc,
            f"CREATE TABLE {'IF NOT EXISTS ' if if_not_exists else ''}unity.plain.t (id INTEGER) "
            f"WITH (location = '{location}'); INSERT INTO unity.plain.t VALUES (1);",
        )
        assert [r["name"] for r in uc.registered] == ["t"]
        assert _ok(uc, "SELECT id FROM unity.plain.t;") == ["1"]


def test_an_empty_delta_log_directory_is_refused_by_delta_for_ctas(tmp_path):
    # Delta's CTAS judges a local location by the directory, not its content, so it refuses where plain CREATE
    # creates: delta's to align
    location = tmp_path / "empty_log"
    (location / "_delta_log").mkdir(parents=True)
    with Catalog() as uc:
        err = _fails(uc, f"CREATE TABLE unity.plain.t WITH (location = '{location}') AS SELECT 1 AS id;")
        assert "already holds one: choose another path" in err, err
        assert uc.registered == []


def _stale_listing_catalog(tmp_path):
    """A catalog whose listing has `stale`, which its by-name lookup no longer finds (dropped elsewhere)."""
    listed = delta_table("stale", tmp_path / "gone", [])
    registered = []

    def respond(method, path, body):
        if method == "GET" and path == f"/api/2.1/unity-catalog/tables/{CATALOG}.plain.stale":
            return 404, {"error_code": "TABLE_DOES_NOT_EXIST", "message": "Table not found: stale"}
        if method == "POST" and path.endswith(REGISTER):
            registered.append(json.loads(body))
            return 200, {"metadata": {"etag": "e", "table-uuid": "u", "location": json.loads(body)["location"]}}
        return None

    uc = MockUnityCatalog([{**listed, "properties": {}}], schemas=["plain"], respond=respond)
    uc.registered = registered
    return uc


@pytest.mark.parametrize(
    "sql",
    [
        "CREATE TABLE unity.plain.stale WITH (location = '{loc}') AS SELECT 42 AS id",
        "CREATE TABLE unity.plain.stale (id INTEGER) WITH (location = '{loc}')",
    ],
    ids=["ctas", "create"],
)
def test_a_name_the_session_listed_is_taken_even_when_uc_no_longer_has_it(tmp_path, sql):
    # Core plans a plain CREATE, dropping a CTAS's query, for a name the session lists; registering then would make
    # an empty table, so the listed name is the conflict
    location = tmp_path / "stale"
    with _stale_listing_catalog(tmp_path) as uc:
        err = _fails(uc, sql.format(loc=location) + ";")
        assert (
            'Table with name "stale" already exists! If it was dropped outside this session, DETACH and ATTACH the catalog again'
            in err
        ), err
        assert uc.registered == []
        assert not location.exists()


def test_if_not_exists_on_a_name_the_session_listed_skips(tmp_path):
    location = tmp_path / "stale"
    with _stale_listing_catalog(tmp_path) as uc:
        _ok(
            uc,
            f"CREATE TABLE IF NOT EXISTS unity.plain.stale WITH (location = '{location}') AS SELECT error('ran') AS id;",
        )
        assert uc.registered == []
        assert not location.exists()


def test_plain_create_if_not_exists_on_a_name_unlisted_at_bind_lists_it(tmp_path):
    winner = tmp_path / "winner"
    with Catalog() as setup:
        _ok(setup, f"CREATE TABLE unity.plain.t WITH (location = '{winner}') AS SELECT 7 AS id;")
        entry = setup.tables[0]
    tables = []

    def respond(method, path, body):
        if method == "GET" and path == f"/api/2.1/unity-catalog/tables/{CATALOG}.plain.t":
            tables.append(entry)
            return 200, {"name": "t"}
        return None

    with MockUnityCatalog(tables, schemas=["plain"], respond=respond) as uc:
        assert _ok(
            uc,
            f"CREATE TABLE IF NOT EXISTS unity.plain.t (id INTEGER) WITH (location = '{tmp_path / 'unused'}'); "
            "SELECT id FROM unity.plain.t;",
        ) == ["7"]
        assert not (tmp_path / "unused").exists()
