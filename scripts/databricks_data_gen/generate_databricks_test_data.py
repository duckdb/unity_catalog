# Usage:
#   python scripts/generate_databricks_test_data.py <command> [options]
#
# Commands:
#
#   copy              - Copy all tables from one catalog.schema to another
#     python scripts/generate_databricks_test_data.py copy source_cat.schema dest_cat.schema
#     python scripts/generate_databricks_test_data.py copy source_cat.schema dest_cat.schema --dry-run
#     python scripts/generate_databricks_test_data.py copy source_cat.schema dest_cat.schema --catalog-managed
#
#   from-duckdb-sql   - Create Databricks tables from a SQL file executed in DuckDB
#     python scripts/generate_databricks_test_data.py from-duckdb-sql path/to/file.sql dest_cat.schema
#     python scripts/generate_databricks_test_data.py from-duckdb-sql path/to/file.sql dest_cat.schema --dry-run
#     python scripts/generate_databricks_test_data.py from-duckdb-sql path/to/file.sql dest_cat.schema --catalog-managed
#
#   from-custom-sql   - Create a single Databricks table from a SQL file executed directly on Databricks
#                       The SQL file is a full CREATE TABLE statement with {table_name} and {location} placeholders.
#                       Table name and location are derived from the filename and destination catalog.schema.
#     python scripts/generate_databricks_test_data.py from-custom-sql scripts/custom_data_sources/example.sql dest_cat.schema
#     python scripts/generate_databricks_test_data.py from-custom-sql scripts/custom_data_sources/example.sql dest_cat.schema --dry-run
#
# Required env vars (all commands):
#   DATABRICKS_TOKEN
#   DATABRICKS_ENDPOINT
#
# from-duckdb-sql flow:
#   1. Executes the SQL file in DuckDB (can define multiple tables)
#   2. Every table in DuckDB's in-memory session gets exported to a local parquet file
#   3. The parquet is read into pandas and pushed to Databricks as a Spark DataFrame (via gRPC, no S3 staging)
#   4. A Delta table is created at s3://<S3_BUCKET>/<dest_catalog>/<dest_schema>/<table>
#
# from-custom-sql flow:
#   1. The SQL file contains the full CREATE TABLE statement with {table_name} and {location} placeholders
#   2. The table name and S3 location are derived from the SQL filename and destination catalog.schema
#   3. Databricks executes the substituted SQL directly
#
# Use --dry-run to print all SQL and actions without executing anything.
# Use --catalog-managed to set delta.feature.catalogManaged and delta.enableRowTracking table properties.

import os
import sys
import argparse
import tempfile
from databricks.connect import DatabricksSession
import duckdb
import pandas as pd


S3_BUCKET = "duckdb-databricks-testing-ccv2"

CATALOG_MANAGED_TBLPROPERTIES = (
    'TBLPROPERTIES ('
    '"delta.feature.catalogManaged" = "supported", '
    '"delta.enableRowTracking" = "false"'
    ')'
)


def get_spark_session():
    token = os.environ.get('DATABRICKS_TOKEN')
    endpoint = os.environ.get('DATABRICKS_ENDPOINT')

    if not all([token, endpoint]):
        raise ValueError("Missing required environment variables: DATABRICKS_TOKEN and DATABRICKS_ENDPOINT")

    return DatabricksSession.builder.remote(host=endpoint, token=token, serverless=True).getOrCreate()


def build_create_sql(full_table_name, location, select_expr, catalog_managed=False):
    # Catalog-managed tables are UC-managed and must not have an explicit LOCATION
    location_clause = "" if catalog_managed else f"\n            LOCATION '{location}'"
    tblproperties = f"\n            {CATALOG_MANAGED_TBLPROPERTIES}" if catalog_managed else ""
    return (
        f"CREATE OR REPLACE TABLE {full_table_name}"
        f"{location_clause}"
        f"{tblproperties}\n"
        f"            AS\n"
        f"            SELECT * FROM {select_expr}"
    )


def copy_tables(source, destination, dry_run=False, catalog_managed=False):
    spark = get_spark_session()

    source_catalog, source_schema = source.split('.')
    dest_catalog, dest_schema = destination.split('.')

    tables = spark.sql(f"SHOW TABLES IN {source_catalog}.{source_schema}").collect()

    spark.sql(f"CREATE SCHEMA IF NOT EXISTS {dest_catalog}.{dest_schema}")

    for table in tables:
        source_table_name = table.tableName
        dest_table_name = f"{source_table_name}_catalog_managed" if catalog_managed else source_table_name
        location = f"s3://{S3_BUCKET}/{dest_catalog}/{dest_schema}/{dest_table_name}"
        create_sql = build_create_sql(
            f"{dest_catalog}.{dest_schema}.{dest_table_name}",
            location,
            f"{source_catalog}.{source_schema}.{source_table_name}",
            catalog_managed,
        )
        if dry_run:
            print(create_sql)
        else:
            print(f"Copying table {source_catalog}.{source_schema}.{source_table_name} to {dest_catalog}.{dest_schema}.{dest_table_name}")
            spark.sql(create_sql)


def duckdb_sql_to_tables(sql_file, destination, dry_run=False, catalog_managed=False):
    """Run sql_file in DuckDB, push each table to Databricks via pandas, create Delta tables in Databricks."""
    dest_catalog, dest_schema = destination.split('.')

    with open(sql_file) as f:
        sql = f.read()

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(sql)

    tables = [row[0] for row in con.execute("SHOW TABLES").fetchall()]
    if not tables:
        print("No tables found in DuckDB after executing SQL file.")
        return

    print(f"Found tables: {tables}")

    spark = get_spark_session()
    spark.sql(f"CREATE SCHEMA IF NOT EXISTS {dest_catalog}.{dest_schema}")

    with tempfile.TemporaryDirectory() as tmpdir:
        for table_name in tables:
            parquet_path = os.path.join(tmpdir, f"{table_name}.parquet")
            con.execute(f"COPY (SELECT * FROM {table_name}) TO '{parquet_path}' (FORMAT parquet)")
            print(f"  Exported '{table_name}' -> {parquet_path}")

            full_table_name = f"{dest_catalog}.{dest_schema}.{table_name}"
            table_location = f"s3://{S3_BUCKET}/{dest_catalog}/{dest_schema}/{table_name}"
            temp_view = f"_tmp_{table_name}"

            create_sql = build_create_sql(full_table_name, table_location, temp_view, catalog_managed)

            if dry_run:
                print(f"  [dry-run] Would load {parquet_path} into Spark temp view '{temp_view}'")
                print(f"  [dry-run] {create_sql}")
                continue

            print(f"  Creating Delta table '{full_table_name}'")
            pandas_df = pd.read_parquet(parquet_path)
            spark.createDataFrame(pandas_df).createOrReplaceTempView(temp_view)
            spark.sql(create_sql)

            print(f"  Done: {full_table_name} @ {table_location}")


def custom_sql_to_table(sql_file, destination, dry_run=False):
    """Execute a custom SQL file directly on Databricks, substituting {table_name} and {location} placeholders.

    The SQL file may contain multiple semicolon-separated statements (e.g. CREATE TABLE then ALTER TABLE then INSERT).
    Both {table_name} and {location} are replaced in every statement.
    """
    dest_catalog, dest_schema = destination.split('.')
    table_name = os.path.splitext(os.path.basename(sql_file))[0]

    with open(sql_file) as f:
        sql_template = f.read()

    full_table_name = f"{dest_catalog}.{dest_schema}.{table_name}"
    table_location = f"s3://{S3_BUCKET}/{dest_catalog}/{dest_schema}/{table_name}"

    substituted = sql_template.replace("{table_name}", full_table_name).replace("{location}", table_location)
    statements = [s.strip() for s in substituted.split(';') if s.strip()]

    if dry_run:
        for stmt in statements:
            print(stmt + ';')
            print()
        return

    spark = get_spark_session()
    spark.sql(f"CREATE SCHEMA IF NOT EXISTS {dest_catalog}.{dest_schema}")
    print(f"  Creating Delta table '{full_table_name}' ({len(statements)} statement(s))")
    for stmt in statements:
        spark.sql(stmt)
    print(f"  Done: {full_table_name} @ {table_location}")


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest='command', required=True)

    copy_parser = subparsers.add_parser('copy', help='Copy tables between two catalog.schema locations')
    copy_parser.add_argument('source', help='Source catalog.schema')
    copy_parser.add_argument('destination', help='Destination catalog.schema')
    copy_parser.add_argument('--dry-run', action='store_true', default=False, help='Print tables to be copied without copying them')
    copy_parser.add_argument('--catalog-managed', action='store_true', default=False, help='Set catalog-managed table properties')

    duckdb_sql_parser = subparsers.add_parser('from-duckdb-sql', help='Create Databricks tables from a SQL file run through DuckDB')
    duckdb_sql_parser.add_argument('sql_file', help='Path to the SQL file to run in DuckDB')
    duckdb_sql_parser.add_argument('destination', help='Destination catalog.schema')
    duckdb_sql_parser.add_argument('--dry-run', action='store_true', default=False, help='Print actions without executing them')
    duckdb_sql_parser.add_argument('--catalog-managed', action='store_true', default=False, help='Set catalog-managed table properties')

    custom_sql_parser = subparsers.add_parser('from-custom-sql', help='Create a Databricks table from a full CREATE TABLE SQL file with {table_name} and {location} placeholders')
    custom_sql_parser.add_argument('sql_file', help='Path to the SQL file in scripts/custom_data_sources/; table name is derived from the filename')
    custom_sql_parser.add_argument('destination', help='Destination catalog.schema')
    custom_sql_parser.add_argument('--dry-run', action='store_true', default=False, help='Print the substituted SQL without executing it')

    args = parser.parse_args()

    if args.command == 'copy':
        copy_tables(args.source, args.destination, args.dry_run, args.catalog_managed)
    elif args.command == 'from-duckdb-sql':
        if not os.path.isfile(args.sql_file):
            print(f"Error: SQL file not found: {args.sql_file}", file=sys.stderr)
            sys.exit(1)
        duckdb_sql_to_tables(args.sql_file, args.destination, args.dry_run, args.catalog_managed)
    elif args.command == 'from-custom-sql':
        if not os.path.isfile(args.sql_file):
            print(f"Error: SQL file not found: {args.sql_file}", file=sys.stderr)
            sys.exit(1)
        custom_sql_to_table(args.sql_file, args.destination, args.dry_run)


if __name__ == "__main__":
    main()