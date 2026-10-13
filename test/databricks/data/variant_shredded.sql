-- variant_shredded: the rows of data/variant_shredded (id INT, v VARIANT, s STRUCT<tag, v VARIANT>,
-- a ARRAY<VARIANT>), written by Databricks with variant shredding on. OSS cannot report the GA
-- `variantShredding` feature or serve a scan plan; this table covers both.
-- Databricks SQL: run via `databricks-gen from-sql`.
CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
TBLPROPERTIES ('delta.enableVariantShredding' = 'true')
AS SELECT
    id,
    parse_json(j) AS v,
    named_struct('tag', 't' || id, 'v', parse_json(j)) AS s,
    array(parse_json(j)) AS a
FROM VALUES
    (1, '{"a": 1, "b": "x"}'),
    (2, '{"a": 2, "b": "y"}'),
    (3, '{"a": 3, "b": "z", "c": [1, 2]}'),
    (4, '42'),
    (6, '"str"'),
    (9, 'null'),
    (10, '[1, "two", null]'),
    (11, '{"o": {"deep": [1, 2, {"k": true}]}}')
AS t(id, j)
