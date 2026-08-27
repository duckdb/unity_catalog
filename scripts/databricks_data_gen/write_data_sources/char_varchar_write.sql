-- The width lives in Delta column metadata (__CHAR_VARCHAR_TYPE_STRING) that Spark writes and UC's
-- own type map drops, so it cannot be expressed from the DuckDB side.
CREATE OR REPLACE TABLE {table_name} (
    id INT,
    code CHAR(5)
)
USING DELTA
LOCATION '{location}'
