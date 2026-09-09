-- Insert the id_name rows into id_name__cmt__duckdb via the DuckDB UC write path
-- (preview API -> promoted commits, not staged), after the empty catalog-managed shell has
-- been created server-side (id_name__cmt__duckdb.sql). Run through the duckdb shell with the
-- databricks creds in the env; the provisioner substitutes {catalog}/{schema}/{table_name},
-- so a manual run has to fill those in too.
CREATE SECRET (TYPE UNITY_CATALOG, TOKEN '${DATABRICKS_TOKEN}', ENDPOINT '${DATABRICKS_ENDPOINT}', AWS_REGION '${DATABRICKS_REGION}');
ATTACH '{catalog}' (TYPE UNITY_CATALOG, DEFAULT_SCHEMA '{schema}');
INSERT INTO {table_name} VALUES (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four'), (5, 'five');
