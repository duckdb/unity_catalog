"""Regression: a UC /tables ColumnInfo with null type_precision must parse (coerce to 0), not throw.

Delta-Spark-created tables report type_precision/type_scale as JSON null in the UC /tables response;
`ParseColumnDefinition` (src/uc_api.cpp) once parsed them with fail_on_missing=true and threw
`IO Error: Invalid field found while parsing field: type_precision` on ANY listing/read -- before
touching Delta data. `uctl`/`bin/uc` always write 0, so a live OSS server can't reproduce the shape.

This drives the extension's read path (CREATE SECRET -> ATTACH -> SHOW ALL TABLES) against a small
in-process **mock** Unity Catalog server (uc.mock_catalog) whose /tables response carries the exact
null-precision ColumnInfo. That makes it a deterministic test of the *parser* -- no live UC
container, no registration, no read-your-writes/cold-start timing (an earlier live version was flaky
on CI for exactly those reasons; see git history and scripts/uc_read_after_write_repro.py).
"""

import os
import subprocess

import pytest

from uc import REPO_ROOT
from uc.mock_catalog import MockUnityCatalog, table

# The Delta-Spark shape `uctl` never produces: columns whose type_precision/type_scale are JSON null.
# This is the exact ColumnInfo pre-fix ParseColumnDefinition threw on.
_SPARK_LIKE_COLUMNS = [
    {
        "name": "id",
        "type_text": "bigint",
        "type_name": "LONG",
        "type_precision": None,
        "type_scale": None,
        "position": 0,
        "nullable": True,
    },
    {"name": "name", "type_text": "string", "type_name": "STRING", "position": 1, "nullable": True},
]


def _duckdb_bin():
    binary = REPO_ROOT / os.environ.get("DUCKDB_BUILD_DIR", "build/debug") / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set DUCKDB_BUILD_DIR)")
    return str(binary)


def test_type_precision_null():
    """SHOW ALL TABLES over a null-type_precision ColumnInfo must succeed (not throw) and list the table."""
    tables = {"cmt": [table("spark_like", "cmt", _SPARK_LIKE_COLUMNS)], "plain": []}
    with MockUnityCatalog(tables) as mock:
        sql = mock.attach_sql("cmt") + (
            "SELECT 'spark_like_count=' || count(*) FROM (SHOW ALL TABLES) t WHERE t.name = 'spark_like';"
        )
        result = subprocess.run([_duckdb_bin(), "-unsigned", "-c", sql], capture_output=True, text=True, timeout=60)

    detail = f"\n--- stderr ---\n{result.stderr}\n--- mock requests ---\n" + "\n".join(mock.requests)
    # Core regression: the null type_precision must not raise while listing.
    assert result.returncode == 0, "SHOW ALL TABLES errored on a null-type_precision ColumnInfo" + detail
    # And the table must actually list -- proving the value was coerced (null -> 0), not dropped.
    assert "spark_like_count=1" in result.stdout, "spark_like was not listed" + detail
