"""`databricks-gen` CLI: atomic Databricks table management from the shell.

Thin wrapper over databricks_gen.sql -- each subcommand is ONE atomic operation, no test
lifecycle (no --keep: `create` makes a table and leaves it; `drop` removes it). `create
--source foo.sql` instantiates a driver fixture (shape + seed) into a Databricks table via
the neutral duckdb-canonicalized path -- the same definition the OSS side consumes.

Config: DATABRICKS_ENDPOINT/TOKEN/WAREHOUSE_ID (see databricks_gen.sql). Fixture
canonicalization needs a duckdb CLI: $DUCKDB_BIN, else $BUILD_DIR/duckdb, else `duckdb`.
"""

import argparse
import os

from . import sql as g
from .sql import CATALOG_MANAGED_PROPS, DATABRICKS_TYPE_MAP


def _duckdb_bin():
    if os.environ.get("DUCKDB_BIN"):
        return os.environ["DUCKDB_BIN"]
    if os.environ.get("BUILD_DIR"):
        return os.path.join(os.environ["BUILD_DIR"], "duckdb")
    return "duckdb"


def _storage_location(storage):
    """Map the `--storage` value to a LOCATION (or None) -- the `storage` half of the 2x2.

    `@managed` (or bare `managed`) => UC-managed, no LOCATION; anything else is an external
    location path. The `@` sigil keeps the `managed` keyword distinct from a path literal.
    """
    return None if storage in ("@managed", "managed") else storage


def _emit(statements, dry_run):
    for sql in statements:
        if dry_run:
            print(sql + ";")
        else:
            g.execute(sql)


def cmd_create(args):
    """Fixture .sql (shape + seed) -> a Databricks table, via the neutral duckdb canonicalizer."""
    from ducktest.fixtures import canonicalize, map_columns, parse_table_spec

    with open(args.source) as f:
        definition = parse_table_spec(f.read(), args.source)
    table = canonicalize(_duckdb_bin(), definition)
    columns = ", ".join(f"{name} {typ}" for name, typ in map_columns(table, DATABRICKS_TYPE_MAP))
    schema_fqn = args.fqn.rsplit(".", 1)[0]

    props = dict(CATALOG_MANAGED_PROPS) if args.commit == "cmt" else None
    statements = [
        f"CREATE SCHEMA IF NOT EXISTS {schema_fqn}",
        g.build_create_table(
            args.fqn,
            columns,
            properties=props,
            location=_storage_location(args.storage),
        ),
    ]
    if table.seed_data:
        statements.append(g.build_insert(args.fqn, table.seed_data))
    _emit(statements, args.dry_run)
    print(
        f"{'[dry-run] ' if args.dry_run else ''}created {args.fqn} "
        f"({len(table.seed_data)} row(s) from {os.path.basename(args.source)})"
    )


def cmd_drop(args):
    _emit([f"DROP TABLE IF EXISTS {args.fqn}"], args.dry_run)


def cmd_drop_schema(args):
    _emit(
        [f"DROP SCHEMA IF EXISTS {args.schema}" + ("" if args.no_cascade else " CASCADE")],
        args.dry_run,
    )


def cmd_sql(args):
    """Run one raw statement (the escape hatch for alter/insert/select/…); print result rows."""
    for row in g.execute(args.statement):
        print("\t".join("" if v is None else str(v) for v in row))


def cmd_from_sql(args):
    """Run a raw Databricks SQL file verbatim -- for Delta artifacts (evolution / column
    mapping / catalog-managed) that aren't a portable fixture shape. The non-portable sibling
    of `create --source`; the reusable core is databricks_gen.run_sql_file.
    """
    statements = g.run_sql_file(
        args.file,
        table=args.table,
        location=_storage_location(args.storage),
        dry_run=args.dry_run,
    )
    if args.dry_run:
        for stmt in statements:
            print(stmt + ";")
    print(
        f"{'[dry-run] ' if args.dry_run else ''}ran {len(statements)} statement(s) "
        f"from {os.path.basename(args.file)} -> {args.table}"
    )


def main(argv=None):
    p = argparse.ArgumentParser(prog="databricks-gen", description="Atomic Databricks table management.")
    sub = p.add_subparsers(dest="command", required=True)

    c = sub.add_parser("create", help="instantiate a fixture (.sql shape+seed) as a Databricks table")
    c.add_argument("fqn", help="destination catalog.schema.table")
    c.add_argument("--source", required=True, help="path to the fixture .sql")
    c.add_argument(
        "--commit",
        choices=("plain", "cmt"),
        default="plain",
        help="plain | cmt (adds the catalog-managed TBLPROPERTIES)",
    )
    c.add_argument(
        "--storage",
        default="@managed",
        metavar="@managed|PATH",
        help="@managed (UC-managed, no LOCATION) | an external location path",
    )
    c.add_argument("--dry-run", action="store_true", help="print SQL, do not execute")
    c.set_defaults(func=cmd_create)

    d = sub.add_parser("drop", help="DROP TABLE IF EXISTS <fqn>")
    d.add_argument("fqn")
    d.add_argument("--dry-run", action="store_true")
    d.set_defaults(func=cmd_drop)

    ds = sub.add_parser("drop-schema", help="DROP SCHEMA IF EXISTS <schema> [CASCADE]")
    ds.add_argument("schema", help="catalog.schema")
    ds.add_argument("--no-cascade", action="store_true", help="omit CASCADE")
    ds.add_argument("--dry-run", action="store_true")
    ds.set_defaults(func=cmd_drop_schema)

    s = sub.add_parser("sql", help="run one raw SQL statement, print result rows")
    s.add_argument("statement")
    s.set_defaults(func=cmd_sql)

    fs = sub.add_parser(
        "from-sql",
        help="run a raw Databricks SQL file (multi-statement, {table_name}/{location})",
    )
    fs.add_argument("file", help="path to the Databricks SQL definition file")
    fs.add_argument(
        "--table",
        required=True,
        help="destination catalog.schema.table (substitutes {table_name})",
    )
    fs.add_argument(
        "--storage",
        default="@managed",
        metavar="@managed|PATH",
        help="@managed | an external location path (substitutes {location})",
    )
    fs.add_argument("--dry-run", action="store_true", help="print SQL, do not execute")
    fs.set_defaults(func=cmd_from_sql)

    args = p.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
