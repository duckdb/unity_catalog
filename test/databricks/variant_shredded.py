"""Driver for variant_shredded.test -- a Databricks-written, shredded VARIANT table.

Paired read: @requires(access="ro") references the premade variant_shredded table (see
test/databricks/data/variant_shredded.sql) and injects CATALOG/SCHEMA. The read catalog is
config.READ_CATALOG (env: DATABRICKS_READ_CATALOG).
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.variant_shredded", access="ro")
def test_variant_shredded(request, resources):
    run_paired(request, env=resources.env)
