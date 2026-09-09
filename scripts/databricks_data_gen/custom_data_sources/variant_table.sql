CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
AS SELECT
    id,
    parse_json('{"value": ' || CAST(id AS STRING) || '}')::variant AS data
FROM range(1, 6) AS t(id)
