-- id_name__alter_colmap_name: same ADD-COLUMN evolution as id_name__alter_plain, but with
-- delta columnMapping.mode = 'name' (mapping keyed on physical name). Databricks SQL: run
-- via `databricks-gen from-sql`.
--   v0: (id, name)                v1: ALTER ADD COLUMN val INT   v2: insert a populated row
CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
TBLPROPERTIES (
    'delta.minReaderVersion' = '2',
    'delta.minWriterVersion' = '5',
    'delta.columnMapping.mode' = 'name'
)
AS SELECT * FROM VALUES (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four'), (5, 'five') AS t(id, name);

ALTER TABLE {table_name} ADD COLUMN val INT;

INSERT INTO {table_name} VALUES (6, 'six', 6)
