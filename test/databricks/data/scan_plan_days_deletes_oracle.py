#!/usr/bin/env python3
"""Regenerate scan_plan_days_deletes_oracle.parquet -- the committed post-delete oracle.

The oracle is the exact expected result of scan_plan_days_deletes AFTER the provision-time
DELETE (see scan_plan_days_deletes.sql): ids 1..50 with day = Mon(1-10)/Tue(11-20)/
Wed(21-30)/Thu(31-40)/Fri(41-50), minus the deleted ids {5,15,25,35,45} -> 45 rows.

scan_plan_deletes.test diffs the live catalog query against read_parquet() of this file via
matching `nosort` labels, so bulking up the table never means hand-enumerating deleted ids again:
just edit the ranges / DELETE set below, rerun, and commit the new parquet.

Run (any python with duckdb OR pyarrow; no build/binary needed):
    python test/databricks/data/scan_plan_days_deletes_oracle.py
"""

import os

DAYS = [("Mon", 1, 11), ("Tue", 11, 21), ("Wed", 21, 31), ("Thu", 31, 41), ("Fri", 41, 51)]
DELETED = {5, 15, 25, 35, 45}

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scan_plan_days_deletes_oracle.parquet")


def rows():
    for day, lo, hi in DAYS:
        for i in range(lo, hi):
            if i not in DELETED:
                yield i, day


def main():
    ids = [r[0] for r in rows()]
    days = [r[1] for r in rows()]
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq

        tbl = pa.table({"id": pa.array(ids, pa.int32()), "day": pa.array(days, pa.string())})
        pq.write_table(tbl, OUT)
    except ImportError:
        import duckdb

        con = duckdb.connect()
        con.execute("CREATE TABLE t(id INTEGER, day VARCHAR)")
        con.executemany("INSERT INTO t VALUES (?, ?)", list(zip(ids, days)))
        con.execute(f"COPY (SELECT * FROM t ORDER BY id) TO '{OUT}' (FORMAT parquet)")
    print(f"wrote {OUT} ({len(ids)} rows)")


if __name__ == "__main__":
    main()
