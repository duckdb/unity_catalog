-- fixture: simple_table
-- keys: [id]
-- Minimal single-column table, ids 1..5. The clone source for the databricks write
-- tests (duckdb_write_testing.source.simple_table) and any single-column read. Neutral
-- (DB/OSS-agnostic); instantiated per backend via the driver's Fixture path.
CREATE TABLE simple_table (id INTEGER);
INSERT INTO simple_table VALUES (1), (2), (3), (4), (5);
