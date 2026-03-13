import os
import sys
import argparse
from databricks.connect import DatabricksSession

def get_spark_session():
    token = os.environ.get('DATABRICKS_TOKEN')
    endpoint = os.environ.get('DATABRICKS_ENDPOINT')

    if not all([token, endpoint]):
        raise ValueError("Missing required environment variables: DATABRICKS_TOKEN and DATABRICKS_ENDPOINT")

    return DatabricksSession.builder.serverless().profile('DEFAULT').getOrCreate()

def drop_tables(schema_to_drop, dry_run=True):
    spark = get_spark_session()

    catalog, schema = schema_to_drop.split('.')

    if dry_run:
        print(f"Would drop schema {catalog}.{schema} CASCADE")
    else:
        print(f"Dropping schema {catalog}.{schema} CASCADE")
        spark.sql(f"DROP SCHEMA IF EXISTS {catalog}.{schema} CASCADE")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('schema_to_drop', help='Target catalog.schema to drop')
    parser.add_argument('--dry-run', action='store_true', default=False,
                        help='Print tables to be dropped without dropping them')
    args = parser.parse_args()

    drop_tables(args.schema_to_drop, args.dry_run)


if __name__ == "__main__":
    main()
