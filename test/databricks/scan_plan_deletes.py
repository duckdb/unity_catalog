"""Driver for scan_plan_deletes.test -- scan-plan delete-file resolution (positional /
deletion-vector) against a table with actual deleted rows.

Same shape as scan_plan.py, against the premade scan_plan_days_deletes table (5 files,
one deleted row per file via a deletion-vector-enabled DELETE; see
test/databricks/data/scan_plan_days_deletes.sql).
"""

from ducktest import requires, run_paired

from uc.databricks import config


@requires(source=f"{config.READ_CATALOG}.main.scan_plan_days_deletes", access="ro")
def test_scan_plan_deletes(request, resources):
    run_paired(request, env=resources.env)
