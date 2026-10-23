"""Column-mapped tables through Unity Catalog, on tables Spark wrote in name and id mode.

A read through the catalog binds against the Delta log, and a write plans through the catalog's own
insert branch, so what duckdb-delta does for a mapped table has to survive both. The fixtures are
Spark's, copied from duckdb-delta's `data/inlined/column_mapping`, so the files carry physical column
names and field ids, and partition values are keyed by the partition column's physical name:

- `spark_partitioned_<mode>_mode`: (id, p, v) partitioned on p, with a NULL partition.
- `spark_renamed_<mode>_mode`: (id, s), one row written, s renamed to who, one row written.
- `spark_nested_<mode>_mode`: (id, s STRUCT(x, y NOT NULL), l INT[], ls STRUCT(a)[],
  m MAP(VARCHAR, STRUCT(b))), one row written, s.x renamed to s.x2, one row written.
- `reserved_physical_name_<mode>`: s STRUCT(x), with x's physical name the one DuckDB's parquet
  writer reserves for field ids.

Each test copies its fixture, so a write never touches the committed tree.
"""

import json
import shutil
from pathlib import Path

import pytest

from uc import delta_log_columns
from uc.mock import MockUnityCatalog, delta_table, run

_DATA = Path(__file__).resolve().parents[2] / "data" / "column_mapping"
MODES = ["name", "id"]


def _column(position, name, type_name, type_text, delta_type):
    return {
        "name": name,
        "type_text": type_text,
        "type_name": type_name,
        "position": position,
        "nullable": True,
        "type_json": f'{{"name":"{name}","type":"{delta_type}","nullable":true,"metadata":{{}}}}',
    }


PARTITIONED_COLUMNS = [
    _column(0, "id", "INT", "int", "integer"),
    {**_column(1, "p", "STRING", "string", "string"), "partition_index": 0},
    _column(2, "v", "INT", "int", "integer"),
]
# As the catalog would report them if it did not know p for a partition column
UNPARTITIONED_COLUMNS = [{k: v for k, v in c.items() if k != "partition_index"} for c in PARTITIONED_COLUMNS]
RENAMED_COLUMNS = [_column(0, "id", "INT", "int", "integer"), _column(1, "who", "STRING", "string", "string")]


def _reported_columns(location):
    """The columns UC reports for a table, read off its latest Delta schema."""
    return [
        {"name": name, "type_text": text, "type_name": type_name, "position": i, "nullable": True, "type_json": js}
        for i, (name, text, type_name, js) in enumerate(delta_log_columns(location))
    ]


def _copy(fixture, tmp_path):
    location = tmp_path / fixture
    shutil.copytree(_DATA / fixture, location)
    return location


def _rows(mock, sql, **kwargs):
    result, stdout = run(mock, sql, **kwargs)
    assert result.returncode == 0, f"{sql}\n--- stderr ---\n{result.stderr}"
    return stdout.splitlines()


# -----------------------------------------------------------------------------
# Partition values
#


@pytest.mark.parametrize("mode", MODES)
def test_partition_values_read_through_the_catalog(tmp_path, mode):
    location = _copy(f"spark_partitioned_{mode}_mode", tmp_path)
    with MockUnityCatalog([delta_table("part", location, PARTITIONED_COLUMNS)]) as uc:
        assert _rows(uc, "SELECT id, p, v FROM unity.plain.part ORDER BY id;") == [
            "1|a|10",
            "2|a|20",
            "3|b|30",
            "4|NULL|40",
        ]
        assert _rows(uc, "SELECT id FROM unity.plain.part WHERE p = 'b';") == ["3"]
        assert _rows(uc, "SELECT id FROM unity.plain.part WHERE p IS NULL;") == ["4"]


@pytest.mark.parametrize("mode", MODES)
def test_insert_into_a_partitioned_table_through_the_catalog(tmp_path, mode):
    location = _copy(f"spark_partitioned_{mode}_mode", tmp_path)
    with MockUnityCatalog([delta_table("part", location, PARTITIONED_COLUMNS)]) as uc:
        _rows(uc, "INSERT INTO unity.plain.part VALUES (5, 'a', 50), (6, 'c', 60), (7, NULL, 70);")
        assert _rows(uc, "SELECT p, count(*) FROM unity.plain.part GROUP BY p ORDER BY p;") == [
            "a|3",
            "b|1",
            "c|1",
            "NULL|2",
        ]

        # Spark's adds and ours share one partition key: p's physical name
        keys = f"""
            SELECT count(*), count(DISTINCT k), bool_and(k LIKE 'col-%')
            FROM (SELECT unnest(json_keys(add.partitionValues)) AS k
                  FROM read_json('{location}/_delta_log/*.json') WHERE add IS NOT NULL);
        """
        assert _rows(uc, keys) == ["6|1|true"]


# -----------------------------------------------------------------------------
# A column Spark renamed
#
# The catalog reports the column's current name. A read at a version before the rename binds the log
# of that version, so the old name is the one it knows.
#


@pytest.mark.parametrize("mode", MODES)
def test_a_renamed_column_reads_under_each_versions_name(tmp_path, mode):
    location = _copy(f"spark_renamed_{mode}_mode", tmp_path)
    with MockUnityCatalog([delta_table("renamed", location, RENAMED_COLUMNS)]) as uc:
        assert _rows(uc, "SELECT id, who FROM unity.plain.renamed ORDER BY id;") == ["1|before", "2|after"]
        assert _rows(uc, "SELECT id, s FROM unity.plain.renamed AT (VERSION => 1);") == ["1|before"]

        _rows(uc, "INSERT INTO unity.plain.renamed (id, who) VALUES (3, 'duck');")
        assert _rows(uc, "SELECT id, who FROM unity.plain.renamed ORDER BY id;") == [
            "1|before",
            "2|after",
            "3|duck",
        ]
        assert _rows(
            uc, f"SELECT count(*) FROM parquet_schema('{location}/duckdb-*.parquet') WHERE name IN ('s', 'who');"
        ) == ["0"]


# -----------------------------------------------------------------------------
# Nested columns
#


@pytest.mark.parametrize("mode", MODES)
def test_a_nested_table_reads_and_takes_inserts_through_the_catalog(tmp_path, mode):
    location = _copy(f"spark_nested_{mode}_mode", tmp_path)
    with MockUnityCatalog([delta_table("nested", location, _reported_columns(location))]) as uc:
        # Before the rename, s.x2 was s.x
        assert _rows(uc, "SELECT id, s.x FROM unity.plain.nested AT (VERSION => 1);") == ["1|10"]

        _rows(
            uc,
            "INSERT INTO unity.plain.nested VALUES (3, {'x2': 30, 'y': 'three'}, [4, 5], [{'a': 3}], MAP {'k3': {'b': 300}});",
        )
        assert _rows(uc, "SELECT * FROM unity.plain.nested ORDER BY id;") == [
            "1|{'x2': 10, 'y': one}|[1, 2]|[{'a': 1}]|{k1={'b': 100}}",
            "2|{'x2': 20, 'y': two}|[3]|[{'a': 2}]|{k2={'b': 200}}",
            "3|{'x2': 30, 'y': three}|[4, 5]|[{'a': 3}]|{k3={'b': 300}}",
        ]
        assert _rows(uc, "SELECT s.x2, ls[1].a FROM unity.plain.nested WHERE m['k3'].b = 300;") == ["30|3"]

        # Field ids on every column and struct field, none on list elements or map entries, as Spark writes them
        assert _rows(
            uc,
            f"SELECT count(*) FILTER (field_id IS NOT NULL), count(*) FILTER (name LIKE 'col-%') "
            f"FROM parquet_schema('{location}/duckdb-*.parquet');",
        ) == ["9|9"]

        result, _ = run(uc, "INSERT INTO unity.plain.nested VALUES (4, {'x2': 40, 'y': NULL}, [], [], MAP {});")
        assert "NOT NULL constraint failed" in result.stderr and ".s.y" in result.stderr, result.stderr


@pytest.mark.parametrize("fixture", ["name_mode", "id_mode", "upper_name_mode"])
def test_a_reserved_physical_name_is_refused_through_the_catalog(tmp_path, fixture):
    location = _copy(f"reserved_physical_name_{fixture}", tmp_path)
    with MockUnityCatalog([delta_table("reserved", location, _reported_columns(location))]) as uc:
        result, _ = run(uc, "INSERT INTO unity.plain.reserved VALUES ({'x': 7});")
        assert "a name DuckDB's parquet writer reserves for field ids" in result.stderr, result.stderr
    assert not list(location.glob("**/*.parquet"))


# -----------------------------------------------------------------------------
# Scan planning
#
# A planned scan reads the planned files with parquet alone, bound by position to the columns UC
# reports. The partition column is not in Spark's files, and a mapped table's files name their
# columns physically, so either one the catalog reports sends the read through the Delta log instead.
#


def _plan_for(location):
    files = sorted(str(p) for p in location.glob("*/*.parquet"))

    def plan(path):
        tasks = [{"data-file": {"content": "data", "file-path": f, "file-format": "parquet"}} for f in files]
        return {"status": "completed", "plan-id": "p1", "file-scan-tasks": tasks}

    return plan


@pytest.mark.parametrize(
    "columns,properties",
    [(PARTITIONED_COLUMNS, {}), (UNPARTITIONED_COLUMNS, {"delta.columnMapping.mode": "name"})],
    ids=["partitioned", "mapped"],
)
@pytest.mark.parametrize("mode", MODES)
def test_a_table_a_planned_scan_would_misread_reads_its_log(tmp_path, mode, columns, properties):
    location = _copy(f"spark_partitioned_{mode}_mode", tmp_path)
    table = {**delta_table("part", location, columns), "properties": properties}
    with MockUnityCatalog([table], plan=_plan_for(location)) as uc:
        options = f", USE_IRC_SCAN_PLAN true, API_IRC_ENDPOINT_OVERRIDE '{uc.endpoint}'"
        assert _rows(uc, "SELECT id, p, v FROM unity.plain.part ORDER BY id;", attach_options=options) == [
            "1|a|10",
            "2|a|20",
            "3|b|30",
            "4|NULL|40",
        ]
        assert not [p for p in uc.requests if p.endswith("/plan")], uc.requests


# Spark's renamed table: its files hold every column, in the reported order, so a planned scan reads it
# right. Reported unmapped it keeps the planned scan, and so does a table that is not Delta, which has
# no log to read instead.
@pytest.mark.parametrize(
    "columns,properties,data_source_format",
    [
        (RENAMED_COLUMNS, {}, "DELTA"),
        (RENAMED_COLUMNS, {"delta.columnMapping.mode": "none"}, "DELTA"),
        ([RENAMED_COLUMNS[0], {**RENAMED_COLUMNS[1], "partition_index": 0}], {}, "PARQUET"),
    ],
    ids=["unmapped", "mode-none", "not-delta"],
)
def test_any_other_table_keeps_the_planned_scan(tmp_path, columns, properties, data_source_format):
    location = _copy("spark_renamed_name_mode", tmp_path)
    table = {
        **delta_table("renamed", location, columns),
        "properties": properties,
        "data_source_format": data_source_format,
    }
    with MockUnityCatalog([table], plan=_plan_for(location)) as uc:
        options = f", USE_IRC_SCAN_PLAN true, API_IRC_ENDPOINT_OVERRIDE '{uc.endpoint}'"
        assert _rows(uc, "SELECT id, who FROM unity.plain.renamed ORDER BY id;", attach_options=options) == [
            "1|before",
            "2|after",
        ]
        assert [p for p in uc.requests if p.endswith("/plan")], uc.requests


# -----------------------------------------------------------------------------
# A managed CREATE takes the mapping mode UC suggests
#
# OSS UC suggests name mode for every managed table, and a CREATE takes it, whatever the columns.
#

STAGED_ID = "00000000-0000-0000-0000-0000000000cc"
STAGING = "/delta/v1/catalogs/duck/schemas/cmt/staging-tables"
REGISTER = "/delta/v1/catalogs/duck/schemas/cmt/tables"


def _staged(location):
    # OSS UC's answer for a managed table, suggestions included
    features = ["catalogManaged", "v2Checkpoint", "vacuumProtocolCheck", "deletionVectors"]
    return {
        "table-id": STAGED_ID,
        "table-type": "MANAGED",
        "location": f"file://{location}",
        "required-protocol": {
            "min-reader-version": 3,
            "min-writer-version": 7,
            "reader-features": features,
            "writer-features": features + ["inCommitTimestamp"],
        },
        "required-properties": {
            "delta.enableInCommitTimestamps": "true",
            "delta.checkpointPolicy": "v2",
            "delta.enableDeletionVectors": "true",
            "io.unitycatalog.tableId": STAGED_ID,
        },
        "suggested-properties": {
            "delta.columnMapping.mode": "name",
            "delta.columnMapping.maxColumnId": None,
            "delta.enableRowTracking": "true",
            "delta.randomizeFilePrefixes": "true",
            "parquet.compression.codec": "zstd",
        },
    }


def _created_mode(tmp_path, create):
    """The mapping mode the table `create` makes is registered with, or None."""
    registered = []

    def respond(method, path, body):
        if method == "POST" and path.endswith(STAGING):
            return 200, _staged(tmp_path / "created")
        if method == "POST" and path.endswith(REGISTER):
            registered.append(json.loads(body))
            return 200, {}
        return None

    with MockUnityCatalog([], schemas=["cmt"], respond=respond) as uc:
        result, _ = run(uc, create)
    assert result.returncode == 0, result.stderr
    (table,) = registered
    return table["properties"].get("delta.columnMapping.mode")


@pytest.mark.parametrize(
    "create,mode",
    [
        ("CREATE TABLE unity.cmt.t (id INTEGER, name VARCHAR);", "name"),
        ("CREATE TABLE unity.cmt.t (id INTEGER, s STRUCT(x INTEGER));", "name"),
        ("CREATE TABLE unity.cmt.t (id INTEGER, l INTEGER[]);", "name"),
        ("CREATE TABLE unity.cmt.t (id INTEGER, m MAP(VARCHAR, INTEGER));", "name"),
        ("CREATE TABLE unity.cmt.t (id INTEGER, v VARIANT);", "name"),
        ("CREATE TABLE unity.cmt.t (id INTEGER, p VARCHAR) PARTITIONED BY (p);", "name"),
    ],
    ids=["top-level", "struct", "list", "map", "variant", "partitioned"],
)
def test_a_managed_create_takes_the_suggested_mapping(tmp_path, create, mode):
    assert _created_mode(tmp_path, create) == mode
