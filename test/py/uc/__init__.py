"""Per-extension (uc) pytest helpers.

Real, per-extension package (NOT symlinked) -- see test/py/driver/README.md "Layout".
Holds shared paths + the `uctl` table-ops wrapper; resource declarations/fixtures live
in sibling modules (e.g. server.py: the OSS UC server resource).

Imported by drivers as `from uc import uctl` / `from uc.server import uc_server`.
"""

import os
import subprocess
from pathlib import Path

from ducktest import ProvisionFailed

# test/py/uc/__init__.py -> repo root is 3 dirs up (uc -> py -> test -> root).
# uc/ is a real dir (only test/py/driver is a symlink), so resolve() is safe here.
REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS_DIR = REPO_ROOT / "scripts" / "oss_uc_image"
UCTL = os.environ.get("UC_UCTL", str(SCRIPTS_DIR / "uctl"))


def uctl(*args, check=True):
    """Run the image kit's `uctl <args>` (wraps `docker exec <container> bin/uc ...`).

    Container name via DUCKTEST_UC_CONTAINER (uctl reads it); the `uc_server` fixture
    starts that container. Surfaces stdout/stderr on failure. Examples:

        uctl("create", "managed", "id_name", "id INT, name STRING")
        uctl("drop", "managed", "id_name", check=False)   # ignore "doesn't exist"
    """
    r = subprocess.run([UCTL, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        # failed uctl IS provisioning failure -- raise with the output so it surfaces clean log line
        out = (r.stdout + r.stderr).strip()
        raise ProvisionFailed(f"uctl {' '.join(map(str, args))} failed (exit {r.returncode}): {out}")
    return r


def plain_table_location(uc_server, table, catalog="duck", schema="plain"):
    """Where uctl points an EXTERNAL table: `<data dir>/<catalog>/<schema>/<table>`.

    The data dir is bind-mounted at the same path host and container, so a test body reads back the
    same tree the write went to. Drivers that stage a hand-written `_delta_log` need this before the
    table exists, which is why it is computed rather than asked for.
    """
    if not uc_server.data_dir:
        raise ProvisionFailed(
            "uc_server exposes no data_dir, so an EXTERNAL table's storage location cannot be "
            "staged into -- the container was attached rather than started here"
        )
    return Path(uc_server.data_dir) / catalog / schema / table
