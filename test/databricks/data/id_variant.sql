-- id_variant: id INT + a Spark-written VARIANT column.
-- Spark is the writer here; test/oss_local/variant.test writes the same payloads row-for-row
-- through duckdb-delta. Row 7 is a variant null.
-- Run via `databricks-gen from-sql`.
CREATE OR REPLACE TABLE {table_name}
AS SELECT id, parse_json(payload) AS data FROM VALUES
    (1, '{"tag": "one", "value": 1}'),
    (2, '{"nested": {"deep": [1, 2, 3]}}'),
    (3, '"plain string"'),
    (4, '42'),
    (5, 'true'),
    (6, '["a", 1, false, {"k": "v"}]'),
    (7, 'null')
AS t(id, payload)
