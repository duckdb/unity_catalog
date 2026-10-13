"""Shared fixtures for oss_local tests.

`uc_server` is re-exported here (from uc.server) so every oss_local test shares ONE OSS UC container
per invocation. Start/stop is now the driver's: the container is provisioned first-need-wins through
the store (driver `provision_service` / `copy_or_provision`) and stopped once by the controller at
sessionfinish (driver `_stop_services`) -- no filesystem lock, no xdist_group. This conftest keeps
only the `--repl` provisioner registration and the OSS catalog-default fixture.
"""

import pathlib

import pytest

from ducktest import register_provisioner
from uc.oss import OssProvisioner
from uc.server import uc_server  # noqa: F401  (re-exported dir-wide)

_OSS_DIR = pathlib.Path(__file__).parent


def pytest_configure(config):
    """Register the OSS provisioner so `--repl` works for oss_local tests.

    Scoped to this conftest (the subtree) -- resolution is by test location, same seam
    as the databricks provisioner (driver/provision.py). --repl only; the run path uses
    the uc_server fixture + uctl.
    """
    register_provisioner(config, OssProvisioner(config), scope=_OSS_DIR)
    config.addinivalue_line("markers", "oss_local: OSS-local UC tests (uc_server container).")


@pytest.fixture(autouse=True)
def _uc_test_catalog(monkeypatch):
    """Inject ${UC_TEST_CATALOG} for OSS bodies (the seeded ducktest catalog is `duck`).

    Scoped per-test via monkeypatch so it auto-restores -- it never leaks to a
    databricks test in a mixed run (whose provisioner resolves its own catalog).
    The run_paired subprocess inherits this env, so the body's `${UC_TEST_CATALOG}`
    substitutes to `duck`.
    """
    monkeypatch.setenv("UC_TEST_CATALOG", "duck")
