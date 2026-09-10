"""OSS Unity Catalog server -- the shared docker service backing oss_local tests.

A running OSS UC docker container, provisioned per invocation and shared across xdist
workers via the driver STORE (first-need-wins single-flight -- see driver `provision_service` /
`copy_or_provision`), then stopped once by the controller at session end (driver `_stop_services`).

Policy: always-fresh per invocation.
  create  = ALWAYS_CREATE   (start_container does `docker rm -f` first -> fresh container)
  destroy = ALWAYS_DESTROY  (the controller stops it at session end)

Isolation is by-lifecycle at a FIXED name/port (a host singleton), so exactly ONE OSS invocation may
run at a time -- concurrent invocations would collide on the container/port (documented limitation).
The `uc_server` fixture reconstructs the store's block into a UcServer; `--repl` (OssProvisioner) owns
its own container via `start_container` directly, outside the store.
"""

import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

import pytest

from ducktest import provision_service, service, step
from uc import SCRIPTS_DIR


# Fixed name/port (by-lifecycle isolation). A host client resolves the server's absolute
# file:// table paths only if the bind-mount path matches, which the kit `run` script
# guarantees (identical host==container data dir); hence we provision via `run`.
CONTAINER = os.environ.get("DUCKTEST_UC_CONTAINER", "ducktest-uc")
PORT = int(os.environ.get("DUCKTEST_UC_PORT", "8080"))
# `:ci` is the published multi-arch tag CI and local runs both use by default; iterating on the
# image ITSELF (scripts/oss_uc_image/build_image tags `:local` locally) means overriding
# DUCKTEST_UC_IMAGE back to `:local`.
IMAGE = os.environ.get("DUCKTEST_UC_IMAGE", "ghcr.io/benfleis/ducktest-unitycatalog:ci")
ENDPOINT = f"http://127.0.0.1:{PORT}"

_CATALOG = "duck"
_SEED_SCHEMAS = ("cmt", "plain")  # entrypoint seeds these after the catalog
_READY_URL = f"{ENDPOINT}/api/2.1/unity-catalog/schemas?catalog_name={_CATALOG}"
# A healthy boot+seed is a few seconds; 45s is a generous ceiling that still FAILS FAST instead of
# masking a hang (a wedged boot under the start-lock blocks every OSS worker). Override if needed.
_READY_TIMEOUT_S = int(os.environ.get("DUCKTEST_UC_READY_TIMEOUT_S", "45"))


@dataclass(frozen=True)
class UcServer:
    """What the fixture yields to a test/driver."""

    endpoint: str
    container: str
    data_dir: str


def _docker(*args, check=True):
    return subprocess.run(["docker", *args], capture_output=True, text=True, check=check)


def _wait_ready(timeout_s):
    """Poll until the seeded catalog AND its cmt/plain schemas exist, or time out.

    The entrypoint seeds the `duck` catalog first, then its schemas, so waiting only on
    the catalog races the seed -- `uctl create` then 404s with SCHEMA_NOT_FOUND. Wait on
    the schemas instead (plain is seeded last, so its presence implies cmt too).
    """
    deadline = time.time() + timeout_s
    last = "no attempt"
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(_READY_URL, timeout=3) as resp:
                names = {s.get("name") for s in json.load(resp).get("schemas", [])}
            missing = [s for s in _SEED_SCHEMAS if s not in names]
            if not missing:
                return
            last = f"catalog up; schemas present={sorted(names)}, still missing={missing}"
        except (
            urllib.error.URLError,
            OSError,
        ) as e:  # 404 (catalog not yet) / refused while booting
            last = repr(e)
        time.sleep(1)
    raise RuntimeError(
        f"OSS UC container {CONTAINER!r} failed to become ready on port {PORT} "
        f"({_READY_URL}) after {timeout_s}s "
        f"(need schemas {list(_SEED_SCHEMAS)} in catalog {_CATALOG!r}): {last}"
    )


def start_container():
    """Start a fresh OSS UC container on the fixed name/port; return UcServer.

    Shared by the `uc_server` fixture (run path) and OssProvisioner (--repl, which runs
    no fixtures so the provisioner owns the container).
    """
    data_dir = tempfile.mkdtemp(prefix="ducktest-uc-data-")
    env = {
        **os.environ,
        "FINAL_IMAGE": IMAGE,
        "DUCKTEST_UC_CONTAINER": CONTAINER,
        "DUCKTEST_UC_PORT": str(PORT),
    }
    with step(f"starting OSS UC docker image ({IMAGE})"):
        _docker("rm", "-f", CONTAINER, check=False)  # ALWAYS_CREATE: force a fresh container
        # `run` = the kit's single source of truth for the docker-run line (identical-path mount).
        # Suppress its stdout info-block so step() is the sole provisioning trace (the
        # --steps / --repl narration channel); stderr stays for failures.
        subprocess.run(
            [str(SCRIPTS_DIR / "run"), data_dir],
            env=env,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        _wait_ready(_READY_TIMEOUT_S)  # waits until the seeded duck.cmt/plain schemas exist
    return UcServer(endpoint=ENDPOINT, container=CONTAINER, data_dir=data_dir)


# Published while the session `uc_server` fixture holds the container. OssProvisioner
# reads it to tell the RUN path (reuse this container, don't touch its lifecycle) from
# the --repl path (no fixtures run, so the provisioner owns a fresh container itself).
_ACTIVE_SERVER = None


def active_server():
    """The session container if `uc_server` is active, else None (e.g. the --repl path)."""
    return _ACTIVE_SERVER


def stop_container(data_dir=None):
    """Stop the OSS UC container (run used --rm, so stop removes it) + clean its data dir."""
    with step("stopping OSS UC docker image"):
        _docker("stop", CONTAINER, check=False)  # ALWAYS_DESTROY
    if data_dir:
        with step("removing test temporary dir"):
            shutil.rmtree(data_dir, ignore_errors=True)


# --- store-backed shared container (first-need-wins via the driver store) ------------------
# The driver store's per-key provision lock gives first-worker-wins (replacing the old filesystem
# _StartLock + state file); the manager dies with the controller, so no lock file leaks, and
# ALWAYS_CREATE (docker rm -f in start_container) force-removes a container leaked by an interrupted
# run -- so reclaim_stale is obsolete. The controller stops the container once at sessionfinish
# (driver _stop_services), before the store manager shuts down.


def _start_service(config):
    """Service `start`: boot a fresh container; return a JSON block (the UcServer fields).
    Runs once per invocation under the store's per-key lock (driver copy_or_provision)."""
    srv = start_container()
    return {
        "endpoint": srv.endpoint,
        "container": srv.container,
        "data_dir": srv.data_dir,
    }


def _stop_service(config):
    """Service `stop`: stop the container + clean its data dir (data_dir from the store block).
    Runs once on the controller at sessionfinish (driver _stop_services); the store is still up."""
    from ducktest import store as _store
    from ducktest import get_store

    data_dir = None
    handle = get_store(config)
    if handle is not None:
        try:
            data_dir = _store.copy(handle, "oss-uc-server").get("data_dir")
        except Exception:
            pass
    stop_container(data_dir)


OSS_SERVICE = service("oss-uc-server", start=_start_service, stop=_stop_service, fixture="uc_server")


@pytest.fixture(scope="session")
def uc_server(request):
    """The ONE shared OSS UC server for this invocation (first-need-wins via the store).

    ALWAYS_CREATE (fresh per invocation) / ALWAYS_DESTROY (the controller stops it at session end).
    The driver store single-flights the boot across xdist workers; we reconstruct the block into a
    UcServer. No filesystem lock, no xdist_group.
    """
    block = provision_service(request.config, OSS_SERVICE)
    srv = UcServer(
        endpoint=block["endpoint"],
        container=block["container"],
        data_dir=block["data_dir"],
    )
    global _ACTIVE_SERVER
    _ACTIVE_SERVER = srv  # publish for OssProvisioner (run path reuses this container)
    try:
        yield srv
    finally:
        _ACTIVE_SERVER = None
        # No stop here: the container is shared across workers; the controller stops it once at
        # sessionfinish (driver _stop_services).
