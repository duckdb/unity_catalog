"""Driver for read_column_mapped.test -- read-only reads of schema-evolved Delta tables.

Paired read: @requires(access="ro") references a premade evolved id_name variant (no DDL)
and injects CATALOG/SCHEMA; the body reads both id_name__alter_colmap_name (name-mode
column mapping) and id_name__alter_plain (plain ADD COLUMN) through env. The read catalog is
config.READ_CATALOG (env: DATABRICKS_READ_CATALOG).
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.id_name__alter_colmap_name", access="ro")
@requires(source=f"{config.READ_CATALOG}.main.id_name__alter_plain", access="ro")
def test_read_column_mapped(request, resources):
    run_paired(request, env=resources.env)
