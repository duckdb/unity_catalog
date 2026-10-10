"""VARIANT columns as Unity Catalog reports them, against the Spark-written fixtures in data/variant_*.

The mock serves what a live OSS server will not: type text with no `type_json`, the Databricks
spelling of the shredding feature, and a scan plan. oss_local/variant_read.test covers the read
paths an OSS server does reach.
"""

import json
from pathlib import Path

import pytest

from uc.mock import MockUnityCatalog, delta_table, run

_REPO = Path(__file__).resolve().parents[2]
_FIXTURES = {"unshredded": _REPO / "data" / "variant_unshredded", "shredded": _REPO / "data" / "variant_shredded"}


def _field(name, typ):
    return {"name": name, "type": typ, "nullable": True, "metadata": {}}


def _column(position, name, type_text, typ, with_json=True):
    column = {"name": name, "type_text": type_text, "type_name": type_text.split("<")[0].upper(), "position": position}
    if with_json:
        column["type_json"] = json.dumps(_field(name, typ))
    return column


def _columns(with_json=True):
    """The fixtures' schema, plus a map the fixtures do not hold; listing never opens the files."""
    return [
        _column(0, "id", "int", "integer", with_json),
        _column(1, "v", "variant", "variant", with_json),
        _column(
            2,
            "s",
            "struct<tag:string,v:variant>",
            {"type": "struct", "fields": [_field("tag", "string"), _field("v", "variant")]},
            with_json,
        ),
        _column(3, "a", "array<variant>", {"type": "array", "elementType": "variant", "containsNull": True}, with_json),
    ]


def _table(name, location, columns, properties=None):
    table = delta_table(name, location, columns)
    table["properties"] = properties or {}
    return table


def _warnings(stdout):
    return [line for line in stdout.splitlines() if "schema." in line]


LISTED = "SELECT column_types FROM (SHOW ALL TABLES) WHERE name = 'variants';"
LISTED_ROW = "[INTEGER, VARIANT, 'STRUCT(tag VARCHAR, v VARIANT)', 'VARIANT[]']"

READ = "SELECT id, v.a::INTEGER, s.v.b::VARCHAR, a[1] FROM unity.plain.variants WHERE id IN (1, 4, 11, 12) ORDER BY id;"
READ_ROWS = [
    "1|1|x|{'a': 1, 'b': x}",
    "4|NULL|NULL|42",
    "11|NULL|NULL|{'o': {'deep': [1, 2, {'k': true}]}}",
    "12|NULL|NULL|NULL",
]


def _rows(stdout):
    return [line for line in stdout.splitlines() if "|" in line]


# -----------------------------------------------------------------------------
# Listing: both spellings of the type
#


@pytest.mark.parametrize("with_json", [True, False], ids=["type_json", "type_text"])
def test_variant_lists_as_variant(with_json, tmp_path):
    with MockUnityCatalog([_table("variants", tmp_path, _columns(with_json))]) as mock:
        result, stdout = run(mock, LISTED)

    assert result.returncode == 0, stdout + result.stderr
    assert LISTED_ROW in stdout, stdout
    assert not _warnings(stdout), stdout


def test_variant_map_value_lists_as_variant(tmp_path):
    column = _column(0, "m", "map<string,variant>", {"type": "map", "keyType": "string", "valueType": "variant"})
    with MockUnityCatalog([_table("variants", tmp_path, [column])]) as mock:
        result, stdout = run(mock, LISTED)

    assert result.returncode == 0, stdout + result.stderr
    assert "['MAP(VARCHAR, VARIANT)']" in stdout, stdout


# -----------------------------------------------------------------------------
# Delta log read: every fixture, both spellings, with and without the shredding feature
#
# Spark reports `variantShredding-preview`; the GA name is `variantShredding`. Either one only permits
# shredding, and the unshredded fixture under it stands for a table whose files never shredded.
#

_SHREDDING = {
    "no_feature": {},
    "preview": {"delta.feature.variantShredding-preview": "supported"},
    "ga": {"delta.feature.variantShredding": "supported"},
}


@pytest.mark.parametrize("feature", sorted(_SHREDDING))
@pytest.mark.parametrize("fixture", sorted(_FIXTURES))
def test_variant_read_through_the_log(fixture, feature):
    properties = {"delta.feature.variantType": "supported", **_SHREDDING[feature]}
    with MockUnityCatalog([_table("variants", _FIXTURES[fixture], _columns(), properties)]) as mock:
        result, stdout = run(mock, LISTED + READ)

    assert result.returncode == 0, stdout + result.stderr
    assert LISTED_ROW in stdout, stdout
    assert _rows(stdout) == READ_ROWS, stdout + result.stderr
    assert not _warnings(stdout), stdout


def test_variant_time_travel_through_the_log():
    with MockUnityCatalog([_table("variants", _FIXTURES["shredded"], _columns())]) as mock:
        result, stdout = run(mock, "SELECT list(id ORDER BY id) FROM unity.plain.variants AT (VERSION => 2);")

    assert result.returncode == 0, stdout + result.stderr
    assert "[1, 2, 3, 4, 5]" in stdout, stdout


def test_variant_text_only_agrees_with_the_log():
    """A server that sends no type_json leaves the text map as the only source of the reported type."""
    with MockUnityCatalog([_table("variants", _FIXTURES["unshredded"], _columns(with_json=False))]) as mock:
        result, stdout = run(mock, LISTED + READ)

    assert result.returncode == 0, stdout + result.stderr
    assert LISTED_ROW in stdout, stdout
    assert _rows(stdout) == READ_ROWS, stdout + result.stderr
    assert not _warnings(stdout), stdout


# -----------------------------------------------------------------------------
# No Delta log: the reported schema stands in
#


def test_variant_without_a_log_binds_to_the_report(tmp_path):
    """Registered but never written: the read binds against the report and then finds nothing to
    read. It must not be refused for the column type."""
    with MockUnityCatalog([_table("variants", tmp_path, _columns())]) as mock:
        result, stdout = run(mock, "SELECT v FROM unity.plain.variants;")

    assert result.returncode != 0, stdout
    assert "no Delta table was found there" in result.stderr, result.stderr
    assert "cannot read" not in result.stderr, result.stderr


# -----------------------------------------------------------------------------
# Scan plan: the reported schema binds, and parquet reads the planned files
#


def _plan_for(location):
    files = sorted(str(p) for p in location.glob("*.parquet"))

    def plan(path):
        tasks = [{"data-file": {"content": "data", "file-path": f, "file-format": "parquet"}} for f in files]
        return {"status": "completed", "plan-id": "p1", "file-scan-tasks": tasks}

    return plan


@pytest.mark.parametrize("fixture", sorted(_FIXTURES))
def test_variant_read_through_a_scan_plan(fixture):
    location = _FIXTURES[fixture]
    with MockUnityCatalog([_table("variants", location, _columns())], plan=_plan_for(location)) as mock:
        options = f", USE_IRC_SCAN_PLAN true, API_IRC_ENDPOINT_OVERRIDE '{mock.endpoint}'"
        result, stdout = run(mock, READ, attach_options=options)
        planned = [p for p in mock.requests if p.endswith("/plan")]

    assert planned, mock.requests
    assert result.returncode == 0, stdout + result.stderr
    assert _rows(stdout) == READ_ROWS, stdout + result.stderr
