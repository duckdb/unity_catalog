"""Driver for variant.test -- reading a Spark-written VARIANT column from Unity Catalog.

Paired read: @requires(access="ro") references the premade id_variant table (see
test/databricks/data/id_variant.sql) and injects CATALOG/SCHEMA. The read catalog is
config.READ_CATALOG (env: DATABRICKS_READ_CATALOG).

Spark writes the payloads here, so the column types and on-disk variant encoding are
Databricks'; the OSS twin (test/oss_local/variant_write.py) has duckdb-delta as the writer.
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.id_variant", access="ro")
def test_variant(request, resources):
    run_paired(request, env=resources.env)
