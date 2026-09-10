-- scan_plan_days_managed: 5 files, one per weekday, for filter-pushdown verification.
-- Catalog-managed (no LOCATION). Each INSERT creates one Parquet file so column-stat pruning is tight.
-- Run via `databricks-gen from-sql`.
CREATE OR REPLACE TABLE {table_name}
    TBLPROPERTIES (
        'delta.feature.catalogManaged' = 'supported',
        'delta.enableRowTracking' = 'false'
    )
    AS SELECT id, 'Mon' AS day FROM range(1, 11) AS t(id);

INSERT INTO {table_name} SELECT id, 'Tue' AS day FROM range(11, 21) AS t(id);
INSERT INTO {table_name} SELECT id, 'Wed' AS day FROM range(21, 31) AS t(id);
INSERT INTO {table_name} SELECT id, 'Thu' AS day FROM range(31, 41) AS t(id);
INSERT INTO {table_name} SELECT id, 'Fri' AS day FROM range(41, 51) AS t(id)
