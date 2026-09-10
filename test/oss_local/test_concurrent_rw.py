"""First PURE-PYTHON oss_local test: concurrent readers/writers, matrixed over commit type.

Collected as a plain pytest test (pytest.ini sets `python_files = test_*.py`). `@requires_matrix`
fans it over the `commit` axis -> `[cmt]` (catalog-managed) and `[plain]`. The table is
provisioned declaratively (same as the `.test` drivers) -- the OssProvisioner creates a unique
empty `id_name` table in the cell's schema and hands back its identity in `resources.env`; we bring
the connection (uc.duckdb) and the concurrency harness (uc.concurrent.run_concurrent).

Each writer appends `id = max(id) + 1` in ONE transaction. The guarantee differs by commit path,
so the assertion does too:

- **cmt** — UC's `UpdateTable` etag serializes EVERY commit, so losers get a version conflict and
  retry against the advanced state. The table is GAPLESS + UNIQUE: ids are exactly `1..K` after K
  commits, i.e. `max(id) == count(*) == version` at every committed snapshot. (Forcing function for
  the `etag optimistic-concurrency` gap in test/README.md.)
- **plain** — plain Delta permits concurrent BLIND APPENDS (no catalog serialization, and the
  isolation level doesn't make two appends conflict), so ids can collide (`[1,1]`). The guarantee is
  DURABILITY: every successful append persists, so `count(*) == K` — if that ever fails it's real
  data loss.
"""

from ducktest import TableSpec, requires_matrix

from uc.concurrent import run_concurrent
from uc.duckdb import connect


@requires_matrix(
    source=TableSpec("id_name").Seed(None),
    access="rw",
    properties={"commit": ["cmt", "plain"]},
)
def test_concurrent_readers_writers(request, uc_server, resources):
    concurrent_writers = 5
    concurrent_readers = 2
    required_commits = 10

    env = resources.env  # OssProvisioner: unique empty table (this cell's schema) + its identity
    db = connect(request, schema=env["UC_TEST_SCHEMA"])
    t = f'{env["UC_TEST_CATALOG"]}.{env["UC_TEST_SCHEMA"]}.{env["UC_TEST_TABLE"]}'
    strict = env["UC_TEST_SCHEMA"] == "cmt"  # catalog etag serializes commits; plain does not

    def append(wid):  # one write attempt: True = committed, False = UC version conflict (retried)
        return db.commit(f"INSERT INTO {t} SELECT coalesce(max(id), 0) + 1, 'w{wid}' FROM {t}")

    def check(rid):
        # cmt: every committed snapshot is exactly 1..n. plain: concurrent appends don't serialize,
        # so there is no per-snapshot gapless guarantee -- durability is checked once at the end.
        if not strict:
            return
        ids = db.column(f"SELECT id FROM {t} ORDER BY id")
        assert ids == list(range(1, len(ids) + 1)), f"reader {rid} saw a broken snapshot: {ids}"

    stats = run_concurrent(
        append, check, writers=concurrent_writers, readers=concurrent_readers, commits=required_commits
    )
    assert stats.commits == concurrent_writers * required_commits

    if strict:
        # cmt: full serialization -> exactly 1..K  (max(id) == count(*) == version); for
        assert db.column(f"SELECT id FROM {t} ORDER BY id") == list(range(1, stats.commits + 1))
    else:
        # plain: durability -> every append persisted (ids may collide under WriteSerializable)
        n = db.scalar(f"SELECT count(*) FROM {t}")
        assert n == stats.commits, f"plain lost appends: count {n} != {stats.commits} commits"

    print(
        f"\n[concurrent_rw:{env['UC_TEST_SCHEMA']}] {stats.commits} commits, "
        f"{stats.conflicts} conflicts, {stats.reads} reads"
    )
