"""AT (TIMESTAMP => ...) through Unity Catalog reads the latest commit at or before the timestamp.

Matrixed over the commit axis, catalog-managed and plain. Commit timestamps are milliseconds, taken from
in-commit timestamps where a table has them and from the log files' modification times otherwise, so the
test sleeps around each timestamp it takes: a commit within the same millisecond could land on either side.

Assertions use row counts, not versions: whether a freshly provisioned table already has an empty
first commit is the provisioner's business.
"""

import time
from datetime import UTC, datetime, timedelta, timezone

from ducktest import TableSpec, requires_matrix

from uc.duckdb import connect

_PAUSE_S = 0.1


def _now_between_commits():
    time.sleep(_PAUSE_S)
    now = datetime.now(UTC).isoformat()
    time.sleep(_PAUSE_S)
    return now


@requires_matrix(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": ["cmt", "plain"]},
)
def test_time_travel_timestamp(request, uc_server, resources):
    env = resources.env
    db = connect(request, schema=env["UC_TEST_SCHEMA"])
    t = f'{env["UC_TEST_CATALOG"]}.{env["UC_TEST_SCHEMA"]}.{env["UC_TEST_TABLE"]}'

    db.exec(f"INSERT INTO {t} VALUES (1, 'one')")
    after_one = _now_between_commits()
    db.exec(f"INSERT INTO {t} VALUES (2, 'two')")
    after_two = _now_between_commits()
    db.exec(f"INSERT INTO {t} VALUES (3, 'three')")

    assert db.scalar(f"SELECT count(*) FROM {t} AT (TIMESTAMP => TIMESTAMPTZ '{after_one}')") == 1
    assert db.scalar(f"SELECT count(*) FROM {t} AT (TIMESTAMP => TIMESTAMPTZ '{after_two}')") == 2
    assert db.scalar(f"SELECT count(*) FROM {t} AT (TIMESTAMP => now())") == 3

    # One transaction reading several timestamps of one table. The same instant at another offset reads
    # what the first spelling does.
    after_one_elsewhere = datetime.fromisoformat(after_one).astimezone(timezone(timedelta(hours=5, minutes=30)))
    sql = f"""
        SELECT
            (SELECT count(*) FROM {t} AT (TIMESTAMP => TIMESTAMPTZ '{after_one}')) AS after_one,
            (SELECT count(*) FROM {t} AT (TIMESTAMP => '{after_one_elsewhere.isoformat()}')) AS after_one_elsewhere,
            (SELECT count(*) FROM {t} AT (TIMESTAMP => TIMESTAMPTZ '{after_two}')) AS after_two,
            (SELECT count(*) FROM {t}) AS latest
    """
    assert db.query(sql) == [{"after_one": 1, "after_one_elsewhere": 1, "after_two": 2, "latest": 3}]
