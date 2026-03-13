-- Version 0: single column, one row
CREATE OR REPLACE TABLE {table_name}
LOCATION '{location}'
TBLPROPERTIES (
    'delta.minReaderVersion' = '2',
    'delta.minWriterVersion' = '5',
    'delta.columnMapping.mode' = 'id'
)
AS SELECT 1 AS id;

-- Version 1: add second column (existing row gets NULL)
ALTER TABLE {table_name} ADD COLUMN val INT;

-- Version 2: insert row with both columns populated
INSERT INTO {table_name} VALUES (2, 2)