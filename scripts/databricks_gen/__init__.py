"""databricks_gen -- atomic Databricks table management over the SQL Statement Execution API.

A dependency-light library (databricks-sdk only) any tool can import; a thin CLI lives in
`cli.py` (invokable as the `databricks-gen` command). See `sql.py` for the primitives.
"""

from .sql import (  # noqa: F401
    CATALOG_MANAGED_PROPS,
    DATABRICKS_TYPE_MAP,
    execute,
    select,
    sql_literal,
    create_schema,
    drop_schema,
    create_table,
    drop_table,
    alter_table,
    insert,
    build_create_table,
    build_insert,
    split_statements,
    run_sql_file,
    external_location,
    split_fqn,
)

__all__ = [
    "CATALOG_MANAGED_PROPS",
    "DATABRICKS_TYPE_MAP",
    "execute",
    "select",
    "sql_literal",
    "create_schema",
    "drop_schema",
    "create_table",
    "drop_table",
    "alter_table",
    "insert",
    "build_create_table",
    "build_insert",
    "split_statements",
    "run_sql_file",
    "external_location",
    "split_fqn",
]
