-- fixture: id_name
-- keys: [id]
-- The uc-module-generic id/name table, seeded 1..5. Shared by oss + databricks.
-- Consumers that want an empty table (body does its own inserts) use Fixture("id_name").Seed(None).
CREATE TABLE id_name (id INTEGER, name VARCHAR);
INSERT INTO id_name VALUES (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four'), (5, 'five');
