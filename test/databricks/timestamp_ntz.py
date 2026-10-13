"""Driver for timestamp_ntz.test -- read-only TIMESTAMP_NTZ read against a premade table.

Paired read: @requires(access="ro") references the premade timestamp_ntz table (see
test/databricks/data/) and injects CATALOG/SCHEMA, so the body attaches + reads through env.
The read catalog is config.READ_CATALOG (env: DATABRICKS_READ_CATALOG).
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.timestamp_ntz", access="ro")
def test_timestamp_ntz(request, resources):
    run_paired(request, env=resources.env)
