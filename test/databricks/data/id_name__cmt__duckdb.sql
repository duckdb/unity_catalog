-- id_name__cmt__duckdb: catalog-managed table whose STRUCTURE is created server-side (empty)
-- but whose DATA is inserted by the DuckDB UC write path (preview API -> promoted commits,
-- not staged). Contrast with id_name__cmt__spark (same schema, staged commits): the
-- promoted-commit path is readable even without the max_catalog_version fix.
-- Two steps: (1) this file creates the empty id_name-shaped shell via `databricks-gen
-- from-sql`; (2) id_name__cmt__duckdb.insert.sql inserts the rows via the duckdb shell.
CREATE OR REPLACE TABLE {table_name}
TBLPROPERTIES (
    'delta.feature.catalogManaged' = 'supported',
    'delta.enableRowTracking' = 'false'
)
AS SELECT * FROM VALUES (1, 'one') AS t(id, name) WHERE false
