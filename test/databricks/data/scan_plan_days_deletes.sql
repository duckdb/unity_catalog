-- scan_plan_days_deletes: same 5-files/50-rows/one-per-weekday layout as
-- scan_plan_days_managed, plus deletion vectors enabled and one row deleted per file
-- (ids 5, 15, 25, 35, 45 -- the last row of each weekday) so the scan-plan path is
-- exercised against a table that actually has delete files to resolve.
-- Whether Databricks surfaces these as `deletion-vector-v1` puffin blobs (the case
-- src/uc_puffin.cpp / BuildUCDeleteFilter handles) or some other shape is exactly what
-- scan_plan_deletes.test is meant to discover on a live run -- see docs/scan-plan-decisions.md.
CREATE OR REPLACE TABLE {table_name}
    TBLPROPERTIES (
        'delta.feature.catalogManaged' = 'supported',
        'delta.enableRowTracking' = 'false',
        'delta.enableDeletionVectors' = 'true'
    )
    AS SELECT id, 'Mon' AS day FROM range(1, 11) AS t(id);


INSERT INTO {table_name} SELECT id, 'Tue' AS day FROM range(11, 21) AS t(id);
INSERT INTO {table_name} SELECT id, 'Wed' AS day FROM range(21, 31) AS t(id);
INSERT INTO {table_name} SELECT id, 'Thu' AS day FROM range(31, 41) AS t(id);
INSERT INTO {table_name} SELECT id, 'Fri' AS day FROM range(41, 51) AS t(id);

-- This DELETE runs server-side at provision time, which is what materializes the deletion vectors
-- scan_plan_deletes.test exercises -- so it must live here, not in the read-only .test. The deleted
-- set is surfaced at that test's assertions (see its TODO re: a pure-Python alternative).
DELETE FROM {table_name} WHERE id IN (5, 15, 25, 35, 45)
