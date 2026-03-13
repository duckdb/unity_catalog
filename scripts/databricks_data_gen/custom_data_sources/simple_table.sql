CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
AS SELECT id FROM range(1, 6) AS t(id)