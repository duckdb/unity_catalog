"""UC connection helper for pure-Python tests: the driver's generic DuckShell runner wired
with the UC preamble (LOAD extensions + secret + ATTACH duck) and the UC conflict wording.

    db = connect(request, schema="cmt")
    db.column(f"SELECT id FROM {t} ORDER BY id")   # -> [1, 2, 3]
    db.commit(f"INSERT ...")                        # True = committed, False = UC conflict

Table provisioning is declarative via `@requires` + the `resources` fixture (the
OssProvisioner creates/drops the table and hands back UC_TEST_CATALOG/SCHEMA/TABLE) -- this
module only owns the *connection*. Everything generic (shell spawn, -json parsing,
query/column/scalar/exec/commit) lives in the driver.
"""

import os

from ducktest import connect_shell

from uc import server

# Locally-built extensions the connection needs, LOADed by full path under `-unsigned`.
_EXTS = ("parquet", "httpfs", "delta", "unity_catalog")

# Substrings marking a UC commit-version conflict (retryable) vs a genuine error. The exact
# surface text is the one empirical unknown -- widen if a real conflict slips through as an error.
_CONFLICT_MARKERS = ("conflict", "commitversionconflict", "409", "concurrent", "etag")


def connect(request, *, schema="cmt"):
    """A DuckShell bound to `schema` of the `duck` catalog (managed -> "cmt", external -> "plain")."""
    return connect_shell(
        request.config,
        lambda build_dir: _preamble(build_dir, schema),
        conflict_markers=_CONFLICT_MARKERS,
    )


def _preamble(build_dir, schema):
    loads = "\n".join(f"LOAD '{os.path.join(build_dir, 'extension', n, n + '.duckdb_extension')}';" for n in _EXTS)
    return (
        f"{loads}\n"
        "CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'not-used', "
        f"ENDPOINT '{server.ENDPOINT}', AWS_REGION 'us-east-2');\n"
        f"ATTACH '{server._CATALOG}' AS {server._CATALOG} "
        f"(TYPE unity_catalog, DEFAULT_SCHEMA '{schema}');\n"
        f"USE {server._CATALOG};\n"
    )
