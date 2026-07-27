"""Collect the extension's C++ Catch cases (the standalone `unittest_cpp` binary) as pytest items.

Default behaviour: if `unittest_cpp` is present under the resolved build dir, each Catch test case
becomes a pytest item that runs it and asserts a clean exit; if the binary isn't built, one skipped
item says how. The binary is server-free (no container, no creds) -- it's the pure-logic unit
coverage (SerializeFiltersToIRC, UCPositionDeleteFilter) that no sqllogictest .test can drive in
isolation. Build it with `dbuild` (which now targets it) or `cmake --build <dir> --target unittest_cpp`.

Binary resolution mirrors how the driver resolves the base `unittest` binary: $BUILD_DIR (or
$DUCKDB_BUILD_DIR) wins; otherwise probe the usual build kinds under build/<kind>/test/.
"""

import os
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_KINDS = ("debug", "reldebug", "relassert", "release")


def _cpp_binary():
    build_dir = os.environ.get("BUILD_DIR") or os.environ.get("DUCKDB_BUILD_DIR")
    if build_dir:
        cand = _REPO / build_dir / "test" / "unittest_cpp"
        return cand if cand.exists() else None
    for kind in _KINDS:
        cand = _REPO / "build" / kind / "test" / "unittest_cpp"
        if cand.exists():
            return cand
    return None


def _cases(binary):
    out = subprocess.run(
        [str(binary), "--list-test-names-only"], capture_output=True, text=True, timeout=30
    )
    return [ln for ln in out.stdout.splitlines() if ln.strip()]


_BIN = _cpp_binary()

if _BIN is None:

    def test_unittest_cpp_present():
        pytest.skip("unittest_cpp not built; run `dbuild` or `cmake --build <dir> --target unittest_cpp`")

else:

    @pytest.mark.parametrize("case", _cases(_BIN), ids=lambda c: c)
    def test_unittest_cpp(case):
        r = subprocess.run([str(_BIN), case], capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, f"{case}:\n{r.stdout}\n{r.stderr}"
