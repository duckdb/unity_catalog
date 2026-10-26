"""Listing a Unity Catalog is complete when several threads list it at once.

The schema list loads on first use. It used to be marked loaded before the fetch, with no lock, so a
thread listing the catalog meanwhile saw no schemas, and so no tables, with no error. Attaching one
catalog twice and listing both with the default thread count lost tables in most runs; the test
repeats that in fresh processes.
"""

import subprocess
from pathlib import Path

from uc import build_dir, load_statements
from uc.mock import CATALOG, MockUnityCatalog, delta_table, duckdb_bin

RUNS = 10
COLUMNS = [
    {
        "name": "id",
        "type_text": "int",
        "type_name": "INT",
        "position": 0,
        "nullable": True,
        "type_json": '{"name":"id","type":"integer","nullable":true,"metadata":{}}',
    }
]
# Listing reads only what the catalog reports, so the tables need no data
TABLES = [
    delta_table("first", Path("/nonexistent"), COLUMNS, schema="plain"),
    delta_table("second", Path("/nonexistent"), COLUMNS, schema="other"),
]


def test_every_listing_sees_every_table():
    with MockUnityCatalog(TABLES) as uc:
        # No `SET threads`: the default thread count is what lists in parallel
        sql = f"""
            {load_statements(build_dir())}
            CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{uc.endpoint}', AWS_REGION 'us-east-2');
            ATTACH '{CATALOG}' AS unity (TYPE unity_catalog, READ_ONLY, DEFAULT_SCHEMA 'plain');
            ATTACH '{CATALOG}' AS unity_again (TYPE unity_catalog, READ_ONLY, DEFAULT_SCHEMA 'plain');
            SELECT database || ':' || schema || '.' || name FROM (SHOW ALL TABLES)
            WHERE database IN ('unity', 'unity_again') ORDER BY ALL;
        """
        listings = []
        for _ in range(RUNS):
            result = subprocess.run(
                [duckdb_bin(), "-unsigned", "-list", "-noheader", "-c", sql],
                capture_output=True,
                text=True,
                timeout=120,
            )
            assert result.returncode == 0, result.stderr
            listings.append([line for line in result.stdout.splitlines() if ":" in line])

    expected = ["unity:other.second", "unity:plain.first", "unity_again:other.second", "unity_again:plain.first"]
    incomplete = [listing for listing in listings if listing != expected]
    assert not incomplete, f"{len(incomplete)} of {RUNS} listings were incomplete, e.g. {incomplete[0]}"
