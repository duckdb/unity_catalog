"""Driver for time_travel.test -- read-only Delta time travel against a premade table.

Paired read: @requires(access="ro") references the premade id_name__alter_colmap_id table
(schema-evolving, columnMapping.mode=id; see test/databricks/data/) and injects
CATALOG/SCHEMA, so the body attaches + reads AT (VERSION => n) through env.
The read catalog is config.READ_CATALOG (env: DATABRICKS_READ_CATALOG).
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.id_name__alter_colmap_id", access="ro")
def test_time_travel(request, resources):
    run_paired(request, env=resources.env)
