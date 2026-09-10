-- id_name__cmt__spark: catalog-managed table, all rows written server-side (staged commits).
-- Verifies max_catalog_version is passed to the Delta kernel so staged commits are visible.
-- On an unfixed build a read fails with "Staged commits in log_tail require
-- max_catalog_version to be set". The multiple INSERTs create multiple staged commits.
-- id_name data (no LOCATION -> UC-managed). Run via `databricks-gen from-sql`.
CREATE OR REPLACE TABLE {table_name}
TBLPROPERTIES (
    'delta.feature.catalogManaged' = 'supported',
    'delta.enableRowTracking' = 'false'
)
AS SELECT * FROM VALUES (1, 'one'), (2, 'two'), (3, 'three') AS t(id, name);

INSERT INTO {table_name} VALUES (4, 'four');

INSERT INTO {table_name} VALUES (5, 'five')
