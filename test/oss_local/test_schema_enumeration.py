"""Regression: concurrent schema enumeration must list EVERY schema's tables.

`UCSchemaSet`'s lazy load used to set `is_loaded = true` *before* `LoadEntries` populated the map,
with no lock around the check -- so a second thread entering `Scan`/`GetEntry` mid-load saw
"already loaded" over an empty map and silently returned a short table list (returncode 0, nothing
logged). Pure-Python (mock server, no OSS container) because the trigger is DuckDB-side
parallelism, not anything the catalog does.

The race is probabilistic, so one query proves nothing: pre-fix this dropped schemas in ~20-35% of
runs, hence the repeat loop.
"""

import os
import subprocess

import pytest

from uc import REPO_ROOT
from uc.mock_catalog import MockUnityCatalog, table

_SCHEMAS = [f"s{i}" for i in range(8)]
_RUNS = 15
_COLUMNS = [{"name": "id", "type_text": "bigint", "type_name": "LONG", "position": 0, "nullable": True}]


@pytest.fixture
def duckdb_bin():
    binary = REPO_ROOT / os.environ.get("DUCKDB_BUILD_DIR", "build/debug") / "duckdb"
    if not binary.exists():
        pytest.skip(f"duckdb CLI not built at {binary} (set DUCKDB_BUILD_DIR)")
    return str(binary)


def test_concurrent_schema_enumeration(duckdb_bin):
    """SHOW ALL TABLES must return all 8 schemas' tables on every run, at default thread count."""
    tables = {s: [table(f"t_{s}", s, _COLUMNS)] for s in _SCHEMAS}
    short_runs = []
    with MockUnityCatalog(tables) as mock:
        sql = mock.attach_sql(_SCHEMAS[0]) + "SELECT 'listed=' || count(*) FROM (SHOW ALL TABLES);"
        for i in range(_RUNS):
            result = subprocess.run(
                [duckdb_bin, "-unsigned", "-c", sql], capture_output=True, text=True, timeout=60
            )
            if result.returncode != 0 or f"listed={len(_SCHEMAS)}" not in result.stdout:
                short_runs.append(f"run {i}: rc={result.returncode}\n{result.stdout}\n{result.stderr}")

    # Distinct endpoints, not the full 100+ request log: a short listing with every schema's /tables
    # among them is the extension dropping schemas, not the mock withholding them.
    served = sorted(set(mock.requests))
    detail = f"\n--- endpoints served ({len(mock.requests)} requests) ---\n" + "\n".join(served)
    assert not short_runs, (
        f"{len(short_runs)}/{_RUNS} runs listed fewer than {len(_SCHEMAS)} tables:\n" + "\n".join(short_runs) + detail
    )
