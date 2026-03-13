# custom_data_sources

SQL files executed directly on Databricks to create test tables.

Each file contains a **full Spark SQL `CREATE TABLE` statement** (or multiple semicolon-separated
statements) with two placeholders that are substituted by `generate_databricks_test_data.py`:

- `{table_name}` — replaced with `<dest_catalog>.<dest_schema>.<filename_without_extension>`
- `{location}` — replaced with the s3 path to store the data in

Multi-statement files (statements separated by `;`) are executed in order, which allows building
up Delta version history via `ALTER TABLE` and `INSERT INTO` after the initial `CREATE TABLE`. |