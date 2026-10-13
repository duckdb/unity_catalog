"""Time travel through Unity Catalog, over a Delta table whose commit timestamps are known exactly.

The fixture carries hand-written in-commit timestamps: on 2023-11-14, v0 commits at 22:13:20 UTC with 10
rows, v1 at 22:14:20 with 11, and v2 at 22:15:20 with 12. So which version a timestamp reads is decided
by the log alone, not by when the files were checked out.
"""

from pathlib import Path

import pytest

from uc.mock import MockUnityCatalog, delta_table, run

_ICT = Path(__file__).resolve().parents[2] / "data" / "ict_timetravel"
_COLUMNS = [
    {
        "name": "i",
        "type_text": "bigint",
        "type_name": "LONG",
        "type_precision": 0,
        "type_scale": 0,
        "position": 0,
        "nullable": True,
        "type_json": '{"name":"i","type":"long","nullable":true,"metadata":{}}',
    }
]


@pytest.fixture
def uc():
    with MockUnityCatalog([delta_table("ict", _ICT, _COLUMNS)]) as mock:
        yield mock


def _rows(mock, sql):
    result, stdout = run(mock, sql)
    assert result.returncode == 0, f"{sql}\n--- stderr ---\n{result.stderr}"
    return stdout.splitlines()


def _error(mock, sql):
    result, stdout = run(mock, sql)
    assert result.returncode != 0, f"{sql}\n--- expected an error, got ---\n{stdout}"
    return result.stderr


def test_a_timestamp_reads_the_latest_commit_at_or_before_it(uc):
    sql = """
        SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => TIMESTAMPTZ '2023-11-14 22:14:19.999+00');
        SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => TIMESTAMPTZ '2023-11-14 22:14:20+00');
        SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => now());
    """
    assert _rows(uc, sql) == ["10", "11", "12"]


def test_timestamps_and_versions_share_one_transaction(uc):
    # 23:14:20+01 is 22:14:20 UTC written differently, so it reads v1 as well.
    sql = """
        SELECT
            (SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => TIMESTAMPTZ '2023-11-14 22:13:20+00')),
            (SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => '2023-11-14 22:14:20+00')),
            (SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => '2023-11-14 23:14:20+01')),
            (SELECT count(*) FROM unity.plain.ict AT (VERSION => 2));
    """
    assert _rows(uc, sql) == ["10|11|11|12"]


def test_a_timestamp_before_the_first_commit_is_refused(uc):
    sql = """
        SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => TIMESTAMPTZ '2023-11-14 22:13:19.999+00');
    """
    assert "LogHistoryError" in _error(uc, sql)


def test_a_timestamp_later_than_now_is_refused(uc):
    sql = """
        SELECT count(*) FROM unity.plain.ict AT (TIMESTAMP => TIMESTAMPTZ '2099-01-01 00:00:00+00');
    """
    assert "later than now" in _error(uc, sql)
