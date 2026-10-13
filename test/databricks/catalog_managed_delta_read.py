"""Driver for catalog_managed_delta_read.test -- read catalog-managed tables via Delta.

Paired read: @requires(access="ro") references a premade catalog-managed id_name variant
(no DDL) and injects CATALOG/SCHEMA; the body verifies max_catalog_version is passed
to the kernel for both the Spark-staged (id_name__cmt__spark) and DuckDB-promoted
(id_name__cmt__duckdb) commit tables. Read catalog: config.READ_CATALOG.
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.id_name__cmt__spark", access="ro")
@requires(source=f"{config.READ_CATALOG}.main.id_name__cmt__duckdb", access="ro")
def test_catalog_managed_delta_read(request, resources):
    run_paired(request, env=resources.env)
