-- id_name__alter_plain: schema evolution by plain ADD COLUMN (no column mapping).
-- Extends the id_name base; pre-existing rows read NULL for the added column, then one row
-- populates it. Databricks SQL (not a portable fixture): run via `databricks-gen from-sql`.
--   v0: (id, name)                -- the id_name base, 5 rows
--   v1: ALTER ADD COLUMN val INT  -- old rows -> val = NULL
--   v2: insert a row with val populated
CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
AS SELECT * FROM VALUES (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four'), (5, 'five') AS t(id, name);

ALTER TABLE {table_name} ADD COLUMN val INT;

INSERT INTO {table_name} VALUES (6, 'six', 6)
