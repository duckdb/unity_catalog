"""Driver for scan_plan.test -- IRC scan-plan API (filter pushdown) against Unity Catalog.

Paired read: @requires(access="ro") references the premade scan_plan_days_managed table
(5 files, one per weekday; see test/databricks/data/) and injects CATALOG/SCHEMA, so the
body attaches with use_irc_scan_plan enabled and exercises server-side filter pushdown.
The read catalog is config.READ_CATALOG (env: DATABRICKS_READ_CATALOG).
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.scan_plan_days_managed", access="ro")
def test_scan_plan(request, resources):
    run_paired(request, env=resources.env)
