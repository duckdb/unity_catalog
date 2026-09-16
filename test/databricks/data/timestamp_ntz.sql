-- timestamp_ntz: a TIMESTAMP_NTZ column beside a plain id, five rows an hour apart. A zoneless
-- Databricks timestamp has no DuckDB counterpart carrying the same meaning, so the read is what
-- pins how it arrives. Databricks SQL: run via `databricks-gen from-sql`.
CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
AS SELECT
    id,
    CAST(TIMESTAMP '2024-01-01 00:00:00' + INTERVAL id HOURS AS TIMESTAMP_NTZ) AS created_at
FROM range(1, 6) AS t(id)
