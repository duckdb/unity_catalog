"""Drive DuckDB through its C API (ctypes on the build's libduckdb), for what only an API client does.

The CLI runs each statement once; API clients (Python, JDBC, ODBC, the C API itself) prepare a statement and then
execute it, which plans it again. `run_capi(mock, steps)` runs `steps` in a fresh process against the mock catalog
attached as `unity`, and returns one result per step:

    ("query", sql)    -> rows, or {"error": message}
    ("prepare", sql)  -> "prepared", or {"error": message}; becomes the current prepared statement
    ("execute",)      -> rows of the current prepared statement, or {"error": message}

The process is separate because the library is loaded into it: a sanitizer build needs its runtime preloaded.
Skips when the build has no libduckdb.
"""

import ctypes as C
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from uc import build_dir, load_statements


def _library():
    return build_dir() / "src" / "libduckdb.so"


def _sanitizer_runtime(library):
    """The ASan runtime a sanitizer build of `library` needs preloaded into a plain Python, or None"""
    symbols = subprocess.run(["nm", "-D", "--undefined-only", str(library)], capture_output=True, text=True)
    if "__asan_init" not in symbols.stdout:
        return None
    for compiler in ("clang", "cc"):
        found = subprocess.run(
            [compiler, "-print-file-name=libclang_rt.asan-x86_64.so"], capture_output=True, text=True
        ).stdout.strip()
        if found and Path(found).is_file():
            return found
    return None


def run_capi(mock, steps):
    library = _library()
    if not library.exists():
        pytest.skip(f"libduckdb not built at {library} (set BUILD_DIR)")
    env = dict(os.environ)
    runtime = _sanitizer_runtime(library)
    if runtime:
        env["LD_PRELOAD"] = runtime
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    env["PYTHONPATH"] = os.pathsep.join([str(Path(__file__).resolve().parents[1]), env.get("PYTHONPATH", "")])
    request = {"library": str(library), "build_dir": str(build_dir()), "endpoint": mock.endpoint, "steps": steps}
    result = subprocess.run(
        [sys.executable, "-m", "uc.capi"],
        input=json.dumps(request),
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.splitlines()[-1])


class _Result(C.Structure):
    _fields_ = [
        ("cols", C.c_uint64),
        ("rows", C.c_uint64),
        ("changed", C.c_uint64),
        ("columns", C.c_void_p),
        ("error", C.c_void_p),
        ("internal", C.c_void_p),
    ]


def _main():
    request = json.loads(sys.stdin.read())
    lib = C.CDLL(request["library"], mode=C.RTLD_GLOBAL)
    ptr, ref, text = C.c_void_p, C.POINTER, C.c_char_p

    def fn(name, args, restype=C.c_int):
        f = getattr(lib, "duckdb_" + name)
        f.argtypes, f.restype = args, restype
        return f

    create_config = fn("create_config", [ref(ptr)])
    set_config = fn("set_config", [ptr, text, text])
    open_ext = fn("open_ext", [text, ref(ptr), ptr, ref(text)])
    destroy_config = fn("destroy_config", [ref(ptr)], None)
    connect = fn("connect", [ptr, ref(ptr)])
    disconnect = fn("disconnect", [ref(ptr)], None)
    close = fn("close", [ref(ptr)], None)
    query = fn("query", [ptr, text, ref(_Result)])
    result_error = fn("result_error", [ref(_Result)], text)
    row_count = fn("row_count", [ref(_Result)], C.c_uint64)
    column_count = fn("column_count", [ref(_Result)], C.c_uint64)
    value_varchar = fn("value_varchar", [ref(_Result), C.c_uint64, C.c_uint64], ptr)
    free = fn("free", [ptr], None)
    destroy_result = fn("destroy_result", [ref(_Result)], None)
    prepare = fn("prepare", [ptr, text, ref(ptr)])
    prepare_error = fn("prepare_error", [ptr], text)
    execute_prepared = fn("execute_prepared", [ptr, ref(_Result)])
    destroy_prepare = fn("destroy_prepare", [ref(ptr)], None)

    config = ptr()
    create_config(C.byref(config))
    set_config(config, b"allow_unsigned_extensions", b"true")
    db, error = ptr(), text()
    if open_ext(None, C.byref(db), config, C.byref(error)):
        raise RuntimeError(error.value)
    destroy_config(C.byref(config))
    con = ptr()
    connect(db, C.byref(con))

    def rows(rc, res):
        try:
            if rc:
                return {"error": result_error(C.byref(res)).decode()}
            out = []
            for r in range(row_count(C.byref(res))):
                row = []
                for c in range(column_count(C.byref(res))):
                    p = value_varchar(C.byref(res), c, r)
                    row.append(C.string_at(p).decode() if p else None)
                    free(p)
                out.append(row)
            return out
        finally:
            destroy_result(C.byref(res))

    def run_query(sql):
        res = _Result()
        return rows(query(con, sql.encode(), C.byref(res)), res)

    setup = [
        load_statements(request["build_dir"]),
        f"CREATE SECRET (TYPE UNITY_CATALOG, TOKEN 'x', ENDPOINT '{request['endpoint']}', AWS_REGION 'us-east-2');",
        "ATTACH 'duck' AS unity (TYPE unity_catalog, DEFAULT_SCHEMA 'cmt');",
    ]
    for sql in setup:
        answer = run_query(sql)
        if isinstance(answer, dict):
            raise RuntimeError(f"{sql}: {answer['error']}")

    results, statement = [], None
    for step in request["steps"]:
        if step[0] == "query":
            results.append(run_query(step[1]))
        elif step[0] == "prepare":
            if statement is not None:
                destroy_prepare(C.byref(statement))
            statement = ptr()
            if prepare(con, step[1].encode(), C.byref(statement)):
                results.append({"error": prepare_error(statement).decode()})
            else:
                results.append("prepared")
        elif step[0] == "execute":
            res = _Result()
            results.append(rows(execute_prepared(statement, C.byref(res)), res))
        else:
            raise ValueError(step)
    if statement is not None:
        destroy_prepare(C.byref(statement))
    disconnect(C.byref(con))
    close(C.byref(db))
    print(json.dumps(results))


if __name__ == "__main__":
    _main()
