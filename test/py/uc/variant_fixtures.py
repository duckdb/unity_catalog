"""The variant fixtures (data/variant_*), as Unity Catalog would describe them.

Columns and properties mirror the Delta log and Spark's SHOW TBLPROPERTIES, so a read that gates on
the reported type or on the shredding feature sees what a Spark-created table would report.
"""

import json

from uc import REPO_ROOT


def _field(name, typ):
    return {"name": name, "type": typ, "nullable": True, "metadata": {}}


def _json(name, typ):
    return json.dumps(_field(name, typ))


COLUMNS = [
    ("id", "int", "INT", _json("id", "integer")),
    ("v", "variant", "VARIANT", _json("v", "variant")),
    (
        "s",
        "struct<tag:string,v:variant>",
        "STRUCT",
        _json("s", {"type": "struct", "fields": [_field("tag", "string"), _field("v", "variant")]}),
    ),
    ("a", "array<variant>", "ARRAY", _json("a", {"type": "array", "elementType": "variant", "containsNull": True})),
]

_BASE = {"delta.feature.variantType": "supported", "delta.minReaderVersion": "3", "delta.minWriterVersion": "7"}

FIXTURES = {
    "unshredded": (REPO_ROOT / "data" / "variant_unshredded", _BASE),
    "shredded": (
        REPO_ROOT / "data" / "variant_shredded",
        {**_BASE, "delta.enableVariantShredding": "true", "delta.feature.variantShredding-preview": "supported"},
    ),
}

TOPLEVEL = (
    REPO_ROOT / "data" / "variant_toplevel",
    [COLUMNS[0], COLUMNS[1]],
    {"delta.feature.variantType": "supported", "delta.minReaderVersion": "3", "delta.minWriterVersion": "7"},
)

# Spark-written, top-level variant only, with delta.enableVariantShredding: a table DuckDB can write
# into while shredding is on. A copy of duckdb-delta's data/inlined/variant/spark_shredded_variant_stats.
SHREDDED_TOPLEVEL = (
    REPO_ROOT / "data" / "variant_shredded_toplevel",
    [COLUMNS[0], COLUMNS[1]],
    {**_BASE, "delta.enableVariantShredding": "true", "delta.feature.variantShredding-preview": "supported"},
)

# A log with protocol and metadata only: DuckDB writes its first data file.
EMPTY = (
    REPO_ROOT / "data" / "variant_empty",
    [COLUMNS[0], ("data", "variant", "VARIANT", _json("data", "variant"))],
    {"delta.feature.variantType": "supported", "delta.minReaderVersion": "3", "delta.minWriterVersion": "7"},
)
