"""Spark-written Delta fixtures for the variant tests: data/variant_unshredded, data/variant_shredded and
data/variant_toplevel.

Same rows and commit layout in the first two; only the shredded one enables `delta.enableVariantShredding`.
Spark infers a shredding schema per file (one file per commit): the object and integer commits
shred, the mixed-scalar commit falls back to `value`.

    JAVA_TOOL_OPTIONS=-XX:-UseContainerSupport <python with pyspark 4.2 + delta-spark 4.4> \
        scripts/variant_fixtures.py data [table ...]

variant_toplevel holds a top-level variant only, for writers that cannot write one nested in a
struct or a list.
"""

import pathlib
import shutil
import sys

from pyspark.sql import SparkSession

OUT = pathlib.Path(sys.argv[1]).resolve()
ONLY = set(sys.argv[2:])
ROW = "SELECT id, parse_json(j), named_struct('tag', 't' || id, 'v', parse_json(j)), array(parse_json(j)) FROM VALUES {} AS t(id, j)"
COMMITS = [
    "(1, '{\"a\": 1, \"b\": \"x\"}'), (2, '{\"a\": 2, \"b\": \"y\"}'), (3, '{\"a\": 3, \"b\": \"z\", \"c\": [1, 2]}')",
    "(4, '42'), (5, '43')",
    "(6, '\"str\"'), (7, 'true'), (8, '12.5'), (9, 'null')",
    "(10, '[1, \"two\", null]'), (11, '{\"o\": {\"deep\": [1, 2, {\"k\": true}]}}')",
]

spark = (
    SparkSession.builder.appName("variant-fixtures")
    .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
    .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
    .config("spark.jars.packages", "io.delta:delta-spark_2.13:4.4.0")
    .config("spark.sql.session.timeZone", "UTC")
    .config("spark.driver.host", "127.0.0.1")
    .config("spark.ui.showConsoleProgress", "false")
    .master("local[1]")
    .getOrCreate()
)
spark.sparkContext.setLogLevel("ERROR")

for name, shredded in [("variant_unshredded", False), ("variant_shredded", True)]:
    if ONLY and name not in ONLY:
        continue
    loc = OUT / name
    shutil.rmtree(loc, ignore_errors=True)
    props = ", 'delta.enableVariantShredding' = 'true'" if shredded else ""
    spark.sql(
        f"CREATE TABLE delta.`{loc}` (id INT, v VARIANT, s STRUCT<tag: STRING, v: VARIANT>, a ARRAY<VARIANT>) "
        f"USING DELTA TBLPROPERTIES ('delta.checkpointInterval' = '100'{props})"
    )
    for values in COMMITS:
        spark.sql(f"INSERT INTO delta.`{loc}` {ROW.format(values)}")
    spark.sql(f"INSERT INTO delta.`{loc}` VALUES (12, NULL, NULL, NULL)")
    for crc in loc.rglob("*.crc"):
        crc.unlink()
    (loc / "_delta_log" / "_staged_commits").rmdir()

if not ONLY or "variant_toplevel" in ONLY:
    loc = OUT / "variant_toplevel"
    shutil.rmtree(loc, ignore_errors=True)
    spark.sql(f"CREATE TABLE delta.`{loc}` (id INT, v VARIANT) USING DELTA")
    spark.sql(
        f"INSERT INTO delta.`{loc}` SELECT id, parse_json(j) FROM VALUES (1, '{{\"a\": 1}}'), (2, '2') AS t(id, j)"
    )
    for crc in loc.rglob("*.crc"):
        crc.unlink()
    (loc / "_delta_log" / "_staged_commits").rmdir()
