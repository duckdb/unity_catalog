"""Root test conftest: declare the OSS + Databricks suites (driver-owned selection/creds/banner).

An *initial* conftest for every `test/…` invocation, so its `pytest_configure` fires on the
controller early -- which is what lets the driver fetch databricks credentials up front even for a
whole-suite `pytest test` run (the old subtree-conftest "full-suite gap").

The driver now owns: suite marking (auto-marker by `path`), the bare-run default scan + banner, the
up-front credential fetch + hard-fail, and the per-test creds backstop. The subtree conftests keep
only what the driver does NOT do (the `--repl` provisioner registration and the databricks
catalog-default env); the OSS container lifecycle stays on `uc.server` for now.

Imports are deferred into `pytest_configure` so they resolve after the driver has put `test/py` on
`sys.path` (its `tryfirst` `pytest_configure` runs before this one).
"""


def pytest_configure(config):
    from ducktest import credential, register_suite
    from uc.databricks import DatabricksProvisioner
    from uc.databricks.engine import (
        cred_failure_detail,
        creds_complete,
        have_core_creds,
        load_creds,
    )
    from uc.oss import OssProvisioner
    from uc.server import OSS_SERVICE

    register_suite(
        config,
        "oss_local",
        path="test/oss_local",
        marker="oss_local",
        default=True,
        provisioner=OssProvisioner(config),
        services=[OSS_SERVICE],
    )
    register_suite(
        config,
        "databricks",
        path="test/databricks",
        marker="databricks",
        default=False,
        provisioner=DatabricksProvisioner(config),
        credentials=[
            credential(
                "databricks_creds",
                fetch=load_creds,
                validate=creds_complete,
                error=cred_failure_detail,
                adopt="env",
                available=have_core_creds,  # non-interactive env check for the -k backstop
            )
        ],
    )
    # Server-free "core" tests -- no logical dependency on a UC/Databricks service, so no provisioner,
    # no services, no creds; runs by default. Two kinds live here: C++ Catch unit tests (via the
    # standalone unittest_cpp binary, collected by test/functions/test_cpp.py) and sqllogic/mock tests
    # that stand alone (dv_decode, type_precision). If unittest_cpp isn't built its collector
    # self-skips (absence != failure); build it with `dbuild` or `cmake --build <dir> --target unittest_cpp`.
    register_suite(
        config,
        "functions",
        path="test/functions",
        marker="functions",
        default=True,
    )
    # Also server-free: the IRC scan-plan mocks. A threaded Python HTTP server stands in for the
    # UC catalog + Iceberg REST plan endpoints, so the request/poll/cancel loop and the whole
    # ATTACH -> scan read path run with no container and no creds.
    register_suite(
        config,
        "irc",
        path="test/irc",
        marker="irc",
        default=True,
    )
