# duckdb_data_sources

SQL files executed in DuckDB to generate test data for Databricks.

Each file defines one or more tables using DuckDB SQL (e.g. TPC-H/TPC-DS generators). The
`from-duckdb-sql` command in `generate_databricks_test_data.py` runs the file in DuckDB,
exports every resulting table to parquet, pushes the data to Databricks via a Spark DataFrame,
and creates a Delta table.