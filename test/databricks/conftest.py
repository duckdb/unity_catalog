"""Databricks subtree conftest: provisioner (for --repl) + catalog-default env.

Selection, up-front credentials, the creds hard-fail, and the per-test creds backstop are now the
driver's (see the root test/conftest.py `register_suite` + `credential`). This conftest keeps only what
the driver does NOT do: the `--repl` provisioner registration (resolution by test location, the
driver/provision.py "REGISTRATION SEAM") and the per-test catalog-default env.
"""

import pathlib

import pytest

from ducktest import register_provisioner
from uc.databricks import DatabricksProvisioner
from uc.databricks.engine import ensure_env

_DBX_DIR = pathlib.Path(__file__).parent


def pytest_configure(config):
    register_provisioner(config, DatabricksProvisioner(config), scope=str(_DBX_DIR))
    config.addinivalue_line("markers", "databricks: live Databricks tests (require credentials).")


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    """Catalog default only -- cheap, always; the creds hard-fail is now the driver's backstop.

    tryfirst: land the catalog default before the `resources` fixture provisions. A *hook* (not a
    fixture) so it covers .py drivers AND bare .test items.
    """
    ensure_env(dry_run=True)
