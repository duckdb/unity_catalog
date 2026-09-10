"""Databricks Unity Catalog backend of the `unity_catalog` extension's test helpers.

A backend submodule of the per-extension package `test/py/uc/` (NOT a peer of it):
Databricks is one backend of the same extension that OSS UC (server.py/uctl) serves.
This package implements the framework's Provisioner protocol (driver/provision.py)
for Databricks, turning generic `@requires` specs into Databricks fixtures + a
duckdb init script so `pytest --repl` can provision and drop into an interactive shell.

Auth is pure-env (DATABRICKS_TOKEN/ENDPOINT[/REGION]); underlying tools use
DatabricksSession.builder.remote(host, token, serverless=True). No ~/.databrickscfg.

Imported as `from uc.databricks import DatabricksProvisioner`.
"""

from .engine import Bindings, DatabricksProvisioner  # noqa: F401
