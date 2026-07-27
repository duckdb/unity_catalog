"""Driver for scan_plan_fallback.test -- /plan-failure fallback to the Delta read path.

Same premade scan_plan_days_managed table as scan_plan.test; the body opts into scan-plan but points
API_IRC_ENDPOINT_OVERRIDE at a dead endpoint so the /plan call fails, then asserts the scan still
returns correct rows via the Delta fallback (the delegate-swap in uc_table_entry.cpp) plus the
api-irc fallback WARNING -- i.e. an unavailable scan-plan endpoint never errors the query.
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.scan_plan_days_managed", access="ro")
def test_scan_plan_fallback(request, resources):
    run_paired(request, env=resources.env)
