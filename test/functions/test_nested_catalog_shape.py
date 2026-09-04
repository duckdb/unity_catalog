"""Catalog-described schema versus logged schema, over identical files.

The UC read path binds against the schema the Delta log carries, not the column list the catalog
reports, so a description that disagrees cannot steer a projection to another field. A live server
cannot serve a disagreeing description -- it echoes back what was registered -- hence the mock.
"""

import http.server
import json
import os
import re
import subprocess
import threading
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_CATALOG = "duck"
_SCHEMA = "plain"

_NESTED = _REPO / "data" / "nested_projection"
_PAIRS = _REPO / "data" / "nested_struct_pairs"

_INT = "\"integer\""
_STR = "\"string\""


def _struct(*fields):
    children = ",".join(
        f'{{"name":"{name}","type":{type_json},"nullable":true,"metadata":{{}}}}' for name, type_json in fields
    )
    return f'{{"type":"struct","fields":[{children}]}}'


def _array(element):
    return f'{{"type":"array","elementType":{element},"containsNull":true}}'


def _map(key, value):
    return f'{{"type":"map","keyType":{key},"valueType":{value},"valueContainsNull":true}}'


_RECORDS = ("array<struct<name:string,value:int>>", _array(_struct(("name", _STR), ("value", _INT))))
_MULTI = ("map<string,array<int>>", _map(_STR, _array(_INT)))
_ID = ("int", _INT)
_REC = ("struct<first:string,second:string>", _struct(("first", _STR), ("second", _STR)))

_NESTED_QUERY = "SELECT id, records[1].name, records[2].value, multi['a'] FROM unity.{s}.{t} ORDER BY id"
_NESTED_ROWS = ["1|x|20|[1, 2, 3]", "2|p|40|[7]"]
_PAIRS_QUERY = "SELECT id, rec.first, rec.second FROM unity.{s}.{t} ORDER BY id"
_PAIRS_ROWS = ["1|a|b", "2|c|d"]

_NESTED_COLUMNS = [("id", _ID), ("records", _RECORDS), ("multi", _MULTI)]

# fixture, table, declared columns, query, expected rows or expected error.
_CASES = {
    "truthful": (_NESTED, "nested_projection", _NESTED_COLUMNS, _NESTED_QUERY, _NESTED_ROWS),
    # Truthful `position`, shuffled array order.
    "columns_out_of_position_order": (
        _NESTED,
        "nested_projection",
        [("multi", _MULTI), ("id", _ID), ("records", _RECORDS)],
        _NESTED_QUERY,
        _NESTED_ROWS,
    ),
    "children_swapped_typed": (
        _NESTED,
        "nested_projection",
        [
            ("id", _ID),
            (
                "records",
                (
                    "array<struct<value:int,name:string>>",
                    _array(_struct(("value", _INT), ("name", _STR))),
                ),
            ),
            ("multi", _MULTI),
        ],
        _NESTED_QUERY,
        _NESTED_ROWS,
    ),
    # Same swap, both children strings: no type disagrees, so nothing stops the read.
    "children_swapped_same_type": (
        _PAIRS,
        "pairs",
        [("id", _ID), ("rec", ("struct<second:string,first:string>", _struct(("second", _STR), ("first", _STR))))],
        _PAIRS_QUERY,
        _PAIRS_ROWS,
    ),
    # DuckDB reads `a.b` as table.column once no column `a` exists, hence "table" in the message.
    "top_level_renamed": (
        _PAIRS,
        "pairs",
        [("id", _ID), ("record", _REC)],
        "SELECT id, record.first FROM unity.{s}.{t} ORDER BY id",
        'Binder Error: Referenced table "record" not found',
    ),
    "top_level_extra_column": (
        _PAIRS,
        "pairs",
        [("id", _ID), ("rec", _REC), ("added", ("string", _STR))],
        _PAIRS_QUERY,
        _PAIRS_ROWS,
    ),
    # No type_json, and text the fallback parser cannot read: the read resolves from the log.
    "text_only_unreadable": (
        _PAIRS,
        "pairs",
        [("id", ("int", None)), ("rec", ("struct<first: string, second: string>", None))],
        _PAIRS_QUERY,
        _PAIRS_ROWS,
    ),
    "json_wins_over_text": (
        _PAIRS,
        "pairs",
        [("id", ("int", _INT)), ("rec", ("bogus<not:a:type>", _struct(("first", _STR), ("second", _STR))))],
        _PAIRS_QUERY,
        _PAIRS_ROWS,
    ),
    # Time travel resolves the same way, at the version the query names.
    "at_latest_version": (
        _PAIRS,
        "pairs",
        [("id", _ID), ("rec", _REC)],
        "SELECT id, rec.first, rec.second FROM unity.{s}.{t} AT (VERSION => 1) ORDER BY id",
        _PAIRS_ROWS,
    ),
    "at_version_before_the_write": (
        _PAIRS,
        "pairs",
        [("id", _ID), ("rec", _REC)],
        "SELECT count(*), 'rows' FROM unity.{s}.{t} AT (VERSION => 0)",
        ["0|rows"],
    ),
    "at_version_past_the_end": (
        _PAIRS,
        "pairs",
        [("id", _ID), ("rec", _REC)],
        "SELECT count(*) FROM unity.{s}.{t} AT (VERSION => 9)",
        "end version 9",
    ),
}


def _columns(spec):
    """A ColumnInfo list whose `position` always tells the truth, so a shuffled array order is the
    reader's problem. Numeric type_precision/type_scale: null is its own bug (type_precision.py)."""
    columns = []
    for position, (name, (type_text, type_json)) in enumerate(spec):
        column = {
            "name": name,
            "type_text": type_text,
            "type_name": type_text.split("<")[0].upper(),
            "type_precision": 0,
            "type_scale": 0,
            "position": position,
            "nullable": True,
        }
        if type_json is not None:
            column["type_json"] = f'{{"name":"{name}","type":{type_json},"nullable":true,"metadata":{{}}}}'
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
    """Attach the mock catalog, run `sql`, return (returncode, stdout with colour stripped)."""
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


def test_divergence_is_reported_once():
    spec = [("id", _ID), ("rec", ("struct<second:string,first:string>", _struct(("second", _STR), ("first", _STR))))]
    with _MockUnityCatalog(_columns(spec), _PAIRS, "pairs") as mock:
        result, stdout = _run(mock, "SELECT id FROM unity.plain.pairs;SELECT id FROM unity.plain.pairs;")

    assert result.returncode == 0, stdout + result.stderr
    warnings = [line for line in stdout.splitlines() if "schema.Resolve" in line]
    assert len(warnings) == 1, f"expected one divergence warning, got {len(warnings)}:\n{stdout}"
    assert 'STRUCT("second" VARCHAR, "first" VARCHAR)' in warnings[0], warnings[0]
    assert 'STRUCT("first" VARCHAR, "second" VARCHAR)' in warnings[0], warnings[0]


def test_reported_columns_follow_position():
    """SHOW ALL TABLES answers from the catalog's report, which is ordered by `position` -- not by
    the order the response happened to list the columns in."""
    spec = [("rec", _REC), ("id", _ID)]
    columns = _columns(spec)
    columns[0]["position"], columns[1]["position"] = 1, 0
    with _MockUnityCatalog(columns, _PAIRS, "pairs") as mock:
        result, stdout = _run(mock, "SELECT column_names FROM (SHOW ALL TABLES) WHERE name = 'pairs';")

    assert result.returncode == 0, stdout + result.stderr
    assert "[id, rec]" in stdout, stdout


def test_table_without_a_log_falls_back_to_the_report(tmp_path):
    """A table registered but never written to has no schema to resolve, so the catalog's report
    stands in for the lookup. The read itself still has nothing to read, and says so."""
    with _MockUnityCatalog(_columns([("id", _ID), ("rec", _REC)]), tmp_path, "pairs") as mock:
        result, stdout = _run(mock, "SELECT id FROM unity.plain.pairs;")

    combined = stdout + result.stderr
    assert "no schema in the Delta log" in stdout, combined
    assert "no Delta table was found there" in result.stderr, combined
    assert "Assertion" not in combined, combined


@pytest.mark.parametrize("case", sorted(_CASES))
def test_nested_catalog_shape(case):
    fixture, table, spec, query, expected = _CASES[case]
    expects_error = isinstance(expected, str)

    with _MockUnityCatalog(_columns(spec), fixture, table) as mock:
        result, stdout = _run(mock, query.format(s=_SCHEMA, t=table) + ";")

    detail = f"\n--- stdout ---\n{stdout}\n--- stderr ---\n{result.stderr}"

    if expects_error:
        assert result.returncode != 0, f"{case}: expected the read to be refused" + detail
        assert expected in result.stderr, f"{case}: refused, but not with the expected message" + detail
        return

    rows = [line for line in stdout.splitlines() if "|" in line]
    assert result.returncode == 0, f"{case}: the read failed" + detail
    assert rows == expected, f"{case}: the read returned values the file does not hold" + detail
