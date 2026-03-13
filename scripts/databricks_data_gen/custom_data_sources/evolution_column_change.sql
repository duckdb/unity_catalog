-- Initial table: only column 'a'
CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
AS SELECT col AS a FROM VALUES ('value1'), ('value3') AS t(col);

-- Add column 'b' (existing rows get NULL)
ALTER TABLE {table_name} ADD COLUMN b INT;

-- Insert row with both columns
INSERT INTO {table_name} VALUES ('value4', 5)