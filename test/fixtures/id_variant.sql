-- fixture: id_variant
-- keys: [id]
-- id + VARIANT column. Seeded empty: the body writes one row per variant value shape.
CREATE TABLE id_variant (id INTEGER, data VARIANT);
