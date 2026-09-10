"""What a catalog says a table holds, against what its Delta log holds, over identical files.

A read binds against the schema resolved from the Delta log, not the schema Unity Catalog reports,
so a misaligned reported schema cannot steer a projection to another field. The mock serves reported
schemas a live server will not: one that misaligns with the resolved schema, since a server echoes
back what was registered, and one carrying no `type_json`, since a server that sends it leaves the
SQL-text type map unreachable.
"""

import http.server
import json
import os
import re
import subprocess
import threading
from pathlib import Path
from typing import NamedTuple

import pytest

_REPO = Path(__file__).resolve().parents[2]
_CATALOG = "duck"
_SCHEMA = "plain"

_NESTED = _REPO / "data" / "nested_projection"
_PAIRS = _REPO / "data" / "nested_struct_pairs"
_TIMESTAMPS = _REPO / "data" / "timestamps"


# -----------------------------------------------------------------------------
# Type DSL
#
# One expression per type, carrying both spellings a ColumnInfo needs: UC's `type_text` and the
# Delta StructField `type` JSON. Nesting the builders keeps the two in step -- the drift a
# hand-written pair invites is what `timestamp_ntz` fell into.


class _Type(NamedTuple):
    text: str  # UC `type_text`
    json: str = None  # Delta StructField `type` fragment; None sends `type_text` alone

    def as_text(self, spelling):
        """The same type with an incorrect `type_text` -- to prove `type_json` wins."""
        return _Type(spelling, self.json)


_INT = _Type("int", '"integer"')
_STR = _Type("string", '"string"')


def _text(spelling):
    """A `type_text` with no `type_json`: the SQL-text fallback path."""
    return _Type(spelling)


def _struct(**fields):
    body = ",".join(f'{{"name":"{n}","type":{t.json},"nullable":true,"metadata":{{}}}}' for n, t in fields.items())
    return _Type(
        "struct<" + ",".join(f"{n}:{t.text}" for n, t in fields.items()) + ">", f'{{"type":"struct","fields":[{body}]}}'
    )


def _array(element):
    return _Type(f"array<{element.text}>", f'{{"type":"array","elementType":{element.json},"containsNull":true}}')


def _map(key, value):
    return _Type(
        f"map<{key.text},{value.text}>",
        f'{{"type":"map","keyType":{key.json},"valueType":{value.json},"valueContainsNull":true}}',
    )


def _col(name, typ):
    return (name, typ)


_REC = _struct(first=_STR, second=_STR)
_RECORDS = _array(_struct(name=_STR, value=_INT))
_MULTI = _map(_STR, _array(_INT))


# -----------------------------------------------------------------------------
# Cases
#
# Two schemas per case, and every test turns on the gap between them. Throughout this file:
#   reported -- the schema Unity Catalog reports, set per case by `reported_schema` (the /tables ColumnInfo)
#   resolved -- the schema resolved from the Delta log, which a read binds against
# Each case gives a `reported` that diverges from `resolved`, then states what the read must do.

_NESTED_QUERY = "SELECT id, records[1].name, records[2].value, multi['a'] FROM unity.{s}.{t} ORDER BY id"
_NESTED_ROWS = ["1|x|20|[1, 2, 3]", "2|p|40|[7]"]
_PAIRS_QUERY = "SELECT id, rec.first, rec.second FROM unity.{s}.{t} ORDER BY id"
_PAIRS_ROWS = ["1|a|b", "2|c|d"]
_TIMESTAMP_QUERY = "SELECT id, event_time FROM unity.{s}.{t} ORDER BY id"
_TIMESTAMP_ROWS = ["1|2026-01-15 10:30:00", "2|2026-06-20 23:59:59", "3|NULL"]

# The fixture is duckdb-delta's own output, which spells the two types the way Databricks does.
_TIMESTAMP_COLUMNS = [
    _col("id", _text("int")),
    _col("event_time", _text("timestamp_ntz")),
    _col("event_tz", _text("timestamp")),
]


class _Rows(NamedTuple):
    rows: list  # the read succeeds and returns exactly these


class _Fails(NamedTuple):
    message: str  # the read is refused, and its stderr contains this


class _Case(NamedTuple):
    why: str  # the divergence, and what must hold in spite of it
    fixture: Path
    table: str
    reported_schema: list  # the UC ColumnInfo report; the log carries the resolved schema
    query: str
    expect: object  # _Rows(...) | _Fails(...)


_CASES = {
    "aligned": _Case(
        why="reported aligns with resolved; the baseline before the misaligned cases",
        fixture=_NESTED,
        table="nested_projection",
        reported_schema=[_col("id", _INT), _col("records", _RECORDS), _col("multi", _MULTI)],
        query=_NESTED_QUERY,
        expect=_Rows(_NESTED_ROWS),
    ),
    "columns_out_of_position_order": _Case(
        why="reported lists its columns out of order but `position` is correct; the read follows position, not list order",
        fixture=_NESTED,
        table="nested_projection",
        reported_schema=[_col("multi", _MULTI), _col("id", _INT), _col("records", _RECORDS)],
        query=_NESTED_QUERY,
        expect=_Rows(_NESTED_ROWS),
    ),
    "children_swapped_typed": _Case(
        why="reported orders a struct's children differently, typed; the read still follows resolved",
        fixture=_NESTED,
        table="nested_projection",
        reported_schema=[
            _col("id", _INT),
            _col("records", _array(_struct(value=_INT, name=_STR))),
            _col("multi", _MULTI),
        ],
        query=_NESTED_QUERY,
        expect=_Rows(_NESTED_ROWS),
    ),
    "children_swapped_same_type": _Case(
        why="children reordered but both strings, so reported and resolved agree on type and nothing stops the read",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("rec", _struct(second=_STR, first=_STR))],
        query=_PAIRS_QUERY,
        expect=_Rows(_PAIRS_ROWS),
    ),
    "top_level_renamed": _Case(
        why="reported renames a top-level column; DuckDB reads `record.first` as table.column once no column `record` exists",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("record", _REC)],
        query="SELECT id, record.first FROM unity.{s}.{t} ORDER BY id",
        expect=_Fails('Binder Error: Referenced table "record" not found'),
    ),
    "top_level_extra_column": _Case(
        why="reported lists a column resolved does not have; the read ignores it",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("rec", _REC), _col("added", _STR)],
        query=_PAIRS_QUERY,
        expect=_Rows(_PAIRS_ROWS),
    ),
    "text_only_unreadable": _Case(
        why="reported has no type_json and text the fallback parser cannot read; the read falls back to resolved",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _text("int")), _col("rec", _text("struct<first: string, second: string>"))],
        query=_PAIRS_QUERY,
        expect=_Rows(_PAIRS_ROWS),
    ),
    "json_wins_over_text": _Case(
        why="reported gives unreadable type_text but correct type_json; the JSON map wins",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("rec", _struct(first=_STR, second=_STR).as_text("bogus<not:a:type>"))],
        query=_PAIRS_QUERY,
        expect=_Rows(_PAIRS_ROWS),
    ),
    "at_latest_version": _Case(
        why="time travel to the current version resolves the same as a plain read",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("rec", _REC)],
        query="SELECT id, rec.first, rec.second FROM unity.{s}.{t} AT (VERSION => 1) ORDER BY id",
        expect=_Rows(_PAIRS_ROWS),
    ),
    "at_version_before_the_write": _Case(
        why="time travel to before the write sees no rows",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("rec", _REC)],
        query="SELECT count(*), 'rows' FROM unity.{s}.{t} AT (VERSION => 0)",
        expect=_Rows(["0|rows"]),
    ),
    "at_version_past_the_end": _Case(
        why="time travel past the last version is refused, naming the version",
        fixture=_PAIRS,
        table="pairs",
        reported_schema=[_col("id", _INT), _col("rec", _REC)],
        query="SELECT count(*) FROM unity.{s}.{t} AT (VERSION => 9)",
        expect=_Fails("end version 9"),
    ),
}


def _columns(spec):
    """A ColumnInfo list whose `position` is always correct, so list order alone is never a
    misalignment. Numeric type_precision/type_scale: null is its own bug (test_type_precision.py)."""
    columns = []
    for position, (name, typ) in enumerate(spec):
        column = {
            "name": name,
            "type_text": typ.text,
            "type_name": typ.text.split("<")[0].upper(),
            "type_precision": 0,
            "type_scale": 0,
            "position": position,
            "nullable": True,
        }
        if typ.json is not None:
            column["type_json"] = f'{{"name":"{name}","type":{typ.json},"nullable":true,"metadata":{{}}}}'
        columns.append(column)
    return columns


class _MockUnityCatalog:
    """Serves the /catalogs + /schemas + /tables an ATTACH and a read need."""

    def __init__(self, columns, fixture, table_name):
        outer = self
        self.requests = []

        table = {
            "name": table_name,
            "catalog_name": _CATALOG,
            "schema_name": _SCHEMA,
            "table_type": "EXTERNAL",
            "data_source_format": "DELTA",
            # file:// keeps RefreshCredentials out of it.
            "storage_location": f"file://{fixture}",
            "table_id": "00000000-0000-0000-0000-000000000001",
            "columns": columns,
        }

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                if "/catalogs" in self.path:
                    body = {"catalogs": [{"name": _CATALOG}]}
                elif "/schemas" in self.path:
                    body = {"schemas": [{"name": _SCHEMA, "catalog_name": _CATALOG}]}
                elif "/tables" in self.path:
                    body = {"tables": [table]}
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


def _run(mock, sql):
    """Attach the mock catalog, run `sql`, return (result, stdout with colour stripped)."""
    prelude = (
        # The listing fans out across the thread pool and is intermittently racy.
        "SET threads TO 1;"
        f"CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{mock.endpoint}', AWS_REGION 'us-east-2');"
        f"ATTACH '{_CATALOG}' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA '{_SCHEMA}');"
    )
    result = subprocess.run(
        [_duckdb_bin(), "-unsigned", "-list", "-noheader", "-c", prelude + sql],
        capture_output=True,
        text=True,
        timeout=120,
    )
    return result, re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)


def test_divergence_is_reported():
    """The warning fires on every lookup, so a catalog that has not caught up with a commit stops
    warning by itself while a stale one keeps saying so. Only the count is unassertable here: the
    CLI displays the first warning of a session and swallows the rest."""
    spec = [_col("id", _INT), _col("rec", _struct(second=_STR, first=_STR))]
    with _MockUnityCatalog(_columns(spec), _PAIRS, "pairs") as mock:
        result, stdout = _run(mock, "SELECT id FROM unity.plain.pairs;SELECT id FROM unity.plain.pairs;")

    assert result.returncode == 0, stdout + result.stderr
    warnings = [line for line in stdout.splitlines() if "schema.Resolve" in line]
    assert warnings, f"no divergence warning:\n{stdout}"
    assert 'STRUCT("second" VARCHAR, "first" VARCHAR)' in warnings[0], warnings[0]
    assert 'STRUCT("first" VARCHAR, "second" VARCHAR)' in warnings[0], warnings[0]


def test_reported_columns_follow_position():
    """SHOW ALL TABLES answers from the reported schema, which is ordered by `position` -- not by
    the order the response happened to list the columns in."""
    spec = [_col("rec", _REC), _col("id", _INT)]
    columns = _columns(spec)
    columns[0]["position"], columns[1]["position"] = 1, 0
    with _MockUnityCatalog(columns, _PAIRS, "pairs") as mock:
        result, stdout = _run(mock, "SELECT column_names FROM (SHOW ALL TABLES) WHERE name = 'pairs';")

    assert result.returncode == 0, stdout + result.stderr
    assert "[id, rec]" in stdout, stdout


def test_table_without_a_log_falls_back_to_the_report(tmp_path):
    """A table registered but never written to has no schema to resolve, so the reported schema
    stands in for the lookup. The read itself still has nothing to read, and says so."""
    with _MockUnityCatalog(_columns([_col("id", _INT), _col("rec", _REC)]), tmp_path, "pairs") as mock:
        result, stdout = _run(mock, "SELECT id FROM unity.plain.pairs;")

    combined = stdout + result.stderr
    assert "no schema in the Delta log" in stdout, combined
    assert "no Delta table was found there" in result.stderr, combined
    assert "Assertion" not in combined, combined


def test_unreadable_column_without_a_log_is_refused(tmp_path):
    """The reported schema stands in only where every column it names resolves to a type. A column
    whose type neither the JSON nor the text parser could read binds to nothing, so the read is refused
    naming the column and the type UC gave it -- not with the binder's "parameter types could not be resolved"."""
    spec = [_col("id", _INT), _col("rec", _text("struct<first: string, second: string>"))]
    with _MockUnityCatalog(_columns(spec), tmp_path, "pairs") as mock:
        listed, listed_out = _run(mock, "SELECT column_names FROM (SHOW ALL TABLES) WHERE name = 'pairs';")
        read, read_out = _run(mock, "SELECT id FROM unity.plain.pairs;")

    assert listed.returncode == 0, listed_out + listed.stderr
    assert "[id, rec]" in listed_out, listed_out

    combined = read_out + read.stderr
    assert read.returncode != 0, combined
    assert "rec" in read.stderr and "struct<first: string, second: string>" in read.stderr, combined
    assert "parameter" not in read.stderr.lower(), combined


def test_timestamp_text_types_agree_with_the_log():
    """Both timestamps reported in text alone: reported types them as resolved does, so the listing
    is accurate and no read reports a divergence. Values are asserted here rather than as a case
    above, where resolved would carry them whatever the text map made of the reported type."""
    with _MockUnityCatalog(_columns(_TIMESTAMP_COLUMNS), _TIMESTAMPS, "timestamps") as mock:
        result, stdout = _run(
            mock,
            "SELECT column_types FROM (SHOW ALL TABLES) WHERE name = 'timestamps';"
            + _TIMESTAMP_QUERY.format(s=_SCHEMA, t="timestamps")
            + ";",
        )

    combined = stdout + result.stderr
    assert result.returncode == 0, combined
    assert "[INTEGER, TIMESTAMP, TIMESTAMP WITH TIME ZONE]" in stdout, combined
    assert "schema.Resolve" not in stdout, combined
    assert [line for line in stdout.splitlines() if line[:1].isdigit()] == _TIMESTAMP_ROWS, combined


@pytest.mark.parametrize("name", sorted(_CASES))
def test_described_schema(name):
    case = _CASES[name]
    with _MockUnityCatalog(_columns(case.reported_schema), case.fixture, case.table) as mock:
        result, stdout = _run(mock, case.query.format(s=_SCHEMA, t=case.table) + ";")

    detail = f"\n[{case.why}]\n--- stdout ---\n{stdout}\n--- stderr ---\n{result.stderr}"

    if isinstance(case.expect, _Fails):
        assert result.returncode != 0, "expected the read to be refused" + detail
        assert case.expect.message in result.stderr, "refused, but with a different message" + detail
        return

    rows = [line for line in stdout.splitlines() if "|" in line]
    assert result.returncode == 0, "the read failed" + detail
    assert rows == case.expect.rows, "the read returned values the file does not hold" + detail
