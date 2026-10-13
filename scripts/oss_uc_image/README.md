# unitycatalog "duck" test image

A Docker image of Unity Catalog (built from a UC source checkout located via
`$UC_REPO`, default `~/src/d/unitycatalog`, which `build_image` fetches + switches
to `$UC_REF` — default `v0.5.1` — before building) preconfigured for dev and CI:

- managed tables enabled, auth disabled, **local filesystem only** (no cloud creds);
- one catalog **`duck`** with two schemas:
  - **`duck.cmt`** — **MANAGED** tables, which in this build are always
    **catalog-managed** (coordinated commits);
  - **`duck.plain`** — **EXTERNAL** tables, i.e. plain Delta logs (no catalog
    commit coordination);
- table data under a single dir bind-mounted at an **identical path** on host and
  container (so a host client can resolve UC's absolute `file://` locations), laid
  out as `duck/cmt/…` and `duck/plain/…` (see Storage layout).

## Files

| file              | what it is                                                            |
|-------------------|-----------------------------------------------------------------------|
| `Dockerfile`      | thin overlay (duck config + entrypoint) on the source-built base       |
| `Dockerfile.base` | upstream `unitycatalog/Dockerfile` + one `COURSIER_CACHE` line (see below) |
| `patches/`        | UC source patches applied to the checkout at build time (see S3 section) |
| `server.properties` | duck server config (managed on, auth off, local FS)                  |
| `entrypoint.sh`   | starts server, waits, idempotently seeds `duck` + `cmt`/`plain` |
| `build_image`  | two-step build + `--push`/`--merge` (source base → overlay); see Publishing |
| `run`          | the single `docker run -v …` line (dev + CI)                           |
| `uctl`         | host table mgmt via `docker exec … bin/uc …`                           |
| `smoke_test`   | boots a throwaway container, create/drop in each schema, asserts files |

## Quick start

```bash
# UC source checkout is found via $UC_REPO (default ~/src/d/unitycatalog)
./build_image                        # slow first time (sbt build); cached after
./build_image --uc-repo /path/to/uc  # ...or point at a different checkout
./run                                # starts container, binds a fresh temp dir, prints where
./run /path/to/datadir               # ...or bind a dir you choose

./uctl create cmt   id_name "id INT, name STRING"   # catalog-managed (MANAGED) table
./uctl create plain id_name "id INT, name STRING"   # plain (EXTERNAL) table
./uctl list cmt
./uctl drop cmt id_name

docker stop ducktest-uc
```

## Image naming

`build_image` derives the tag from the UC checkout (version + UC sha in the **tag**):

```
ghcr.io/benfleis/ducktest-unitycatalog:<version>-uc<gitref7>
  e.g. ghcr.io/benfleis/ducktest-unitycatalog:0.5.0-snapshot-uc1321705
```

- `<version>` = `version.sbt` (lowercased); `<gitref7>` = 7-char UC SHA. Branch and a
  `-dirty` flag are intentionally omitted from the published tag (the build patches the
  UC source, so it always reads dirty, and the checkout is usually detached). The full
  SHA is stamped as the `org.opencontainers.image.revision` label (`docker inspect`).
- The base (source-built) image is `ghcr.io/benfleis/ducktest-unitycatalog-base:<same-tag>`,
  keyed to the same ref so a new commit triggers a fresh sbt build.
- Two tags, one meaning each: **`:local`** is the image you just built — the `oss_uc_image/`
  toolkit works on it (`build_image` stamps it on a native build; `run`/`smoke_test` default to
  it) — and **`:ci`** is the validated, published image the test suite (`server.py`) and CI
  consume. You promote `:local`→`:ci` with `build_image --merge --alias ci` once `smoke_test`
  passes (see Publishing). The canonical registry `:<tag>` is a proper multi-arch manifest,
  assembled by that same `--merge`.
- Namespace/stem are `IMAGE_NS` / `IMAGE_NAME` env vars (default `ghcr.io/benfleis` /
  `ducktest-unitycatalog`). Override `IMAGE_NS` to publish elsewhere.

## Publishing to ghcr

The build scripts do **no auth** — they call `docker push` / `docker buildx imagetools`, which use
whatever `docker login` you already ran. So the same commands publish locally and in CI; only the login
differs (a personal PAT locally, `GHCR_PAT`/`GITHUB_TOKEN` in the workflow).

Publish from a native **amd64 Linux** box (arm64: run the same on an Apple-Silicon Mac):

```bash
docker login ghcr.io -u <you>                                          # a PAT with write:packages, once
UC_REPO=/path/to/unitycatalog scripts/oss_uc_image/build_image --push  # build :local + push :<tag>-<arch>
scripts/oss_uc_image/smoke_test                                        # GATE: validate :local (per arch)
scripts/oss_uc_image/build_image --merge --alias ci                    # promote -> :ci (once all arches pass)
```

- `--push` publishes the arch-suffixed `:<tag>-<arch>`; `--merge` assembles the canonical multi-arch
  `:<tag>` from whatever arch slices exist; `--alias ci` moves a stable **`:ci`** tag onto it.
- **`:ci` is what consumers pull.** `server.py` defaults to it and CI's `Ducktest.yml` sets
  `DUCKTEST_UC_IMAGE=…:ci` explicitly; the `build_image`/`run`/`smoke_test` toolkit works on `:local`.
  The `--merge --alias ci` above is the promotion — run it only after `smoke_test` passes.
- **Multi-arch:** build each arch natively where it's cheap and re-merge — e.g. amd64 in CI, arm64 from a
  Mac (`build_image --push` there), then `build_image --merge --alias ci` picks up both. No QEMU.
- A republish reuses an already-pushed base (build_image consults the registry), so only the first
  publish pays the slow sbt build.
- **In CI:** `.github/workflows/publish-uc-image.yml` — manual `workflow_dispatch`, guarded to the owner
  + default branch, GHCR_PAT auth — runs exactly the amd64 flow above. It only shows in the Actions UI
  once it's on the default branch, and the package must be public for the anonymous CI pull.

## The two axes (important)

Two **orthogonal** properties are often conflated:

- **table type `MANAGED` vs `EXTERNAL`** — *storage ownership*: does UC allocate and
  own the table's location, or do you provide an external one UC just registers.
- **catalog-managed (the `catalogManaged` Delta feature) vs plain** — *commit
  coordination*: are Delta commits coordinated through the catalog (0.5 coordinated
  commits) or written directly by the client.

In principle that's a 2×2. **But in this OSS 0.5 build, the `uc` CLI only reaches the
diagonal** (`examples/cli/.../delta/DeltaKernelUtils.java`): a `MANAGED` create always
commits via `UCCatalogManagedClient` (→ catalog-managed), and an `EXTERNAL` create
always writes a plain log (→ not catalog-managed). There is **no** "MANAGED but plain"
or "EXTERNAL but catalog-managed" path here. So:

| schema | table type | result |
|--------|-----------|--------|
| `duck.cmt`   | `MANAGED`  | catalog-managed, UC-allocated location |
| `duck.plain` | `EXTERNAL` | plain Delta log, client location under `duck/plain` |

`uctl` just picks the type per schema. The `uc` CLI writes a real Delta log
end-to-end via Delta Kernel (no Spark needed), so both are genuine Delta tables.

## Storage layout (identical-path bind mount)

UC records **absolute** `file://` locations for tables. A client can only open
them if that path resolves in its own filesystem namespace — so `run.sh` bind-mounts
the data dir at the **same absolute path** on host and container (`-v $DIR:$DIR`)
and sets `DUCKTEST_UC_DATA_DIR=$DIR`. A host client (e.g. the duckdb `unittest` binary)
then resolves `file://$DIR/...` to the same files the container wrote. (Mounting to a
*different* container path like `/home/unitycatalog/etc/data` breaks host clients:
UC hands back `/home/unitycatalog/...`, which doesn't exist on the host.)

```
$DUCKTEST_UC_DATA_DIR/                    # same path on host and in the container
  duck/
    cmt/                              # storage_root of schema duck.cmt
      __unitystorage/schemas/<schema-uuid>/tables/<table-uuid>/_delta_log + parquet
    plain/                            # plain tables, one subdir per table
      <table>/_delta_log + parquet
```

- `duck.cmt` has `storage_root = $DUCKTEST_UC_DATA_DIR/duck/cmt`, so UC nests its
  `__unitystorage` tree (and every cmt table) under it.
- `duck.plain` has **no** storage_root; `uctl` gives each plain table a location
  at `$DUCKTEST_UC_DATA_DIR/duck/plain/<table>` (read from the container's env). Because
  that is a **sibling** of `duck/cmt` (neither under nor above cmt storage),
  UC's overlap check passes and **no external location needs to be registered**.
- `DUCKTEST_UC_DATA_DIR` defaults to `/home/unitycatalog/etc/data` (fine for an
  in-container client); `run.sh` overrides it to the identical-path host dir.
- The H2 metastore lives at `etc/db` (NOT under the data mount), so it is fresh per
  container — see CI usage.

## CI usage

The metastore (H2) is **fresh inside the container each run** and the entrypoint
re-seeds `duck`/`cmt`/`plain` after the mount, so each `docker run` against a
new temp dir is isolated. A typical CI step:

```bash
./build
./smoke_test            # boots throwaway container, asserts, tears down
```

or drive it yourself:

```bash
tmp=$(mktemp -d)
./run "$tmp"
# ... exercise via ./uctl or the REST API at http://localhost:8080 ...
docker stop ducktest-uc        # --rm cleans the container; rm -rf "$tmp" when done
```

## Why `Dockerfile.base` (the COURSIER_CACHE fix)

The upstream `unitycatalog/Dockerfile` builds with `sbt` as **root**, so coursier
caches dependency jars under `/root/.cache` (it keys off the uid's home, not the
`HOME` env). But the runtime stage only copies `$HOME/.cache`
(`/home/unitycatalog/.cache`), so every dependency jar — vertx, etc. — is left out
and the server dies at startup with `NoClassDefFoundError: io/vertx/core/Verticle`.
`Dockerfile.base` is that same Dockerfile with one added line —
`ENV COURSIER_CACHE=$HOME/.cache/coursier` — so the cache lands where the runtime
stage copies from. (The same one-line fix has been applied to `unitycatalog/Dockerfile`
in the checkout for upstreaming; once it merges, `Dockerfile.base` can be dropped and
`build` pointed back at the repo's own Dockerfile.)

## S3 / MinIO (optional, dormant by default)

The image is built **S3-ready** but operates on **local FS** unless you opt in. Two
pieces ride along regardless:

- **`patches/*.patch`** are applied to the UC checkout at build time (idempotent;
  a no-op if your checkout already has them). Currently one: ambient-S3 credentials
  in the CLI's Delta writer (`DeltaKernelUtils.getHDFSConfiguration`) so a `s3://`
  **EXTERNAL** table doesn't NPE when UC vends no creds. Harmless for local FS/AWS.
- A **conf dir on the classpath**, so a `core-site.xml` the entrypoint can write is
  picked up by S3A.

To point the CLI's S3A writer at an S3-compatible store (e.g. MinIO), set at
`docker run`: **`DUCKTEST_UC_S3_ENDPOINT`** (+ optional `DUCKTEST_UC_S3_REGION`,
`DUCKTEST_UC_S3_KEY`, `DUCKTEST_UC_S3_SECRET`, `DUCKTEST_UC_S3_SSL`). The entrypoint then writes
`core-site.xml`; unset → no file → **AWS default endpoint / local FS** untouched.

Caveats (from the spike — see project notes):
- **EXTERNAL** s3:// tables work with the above alone.
- **MANAGED** s3:// tables additionally need UC credential vending configured in
  `server.properties` (`s3.bucketPath.0` + region + keys + a **non-empty**
  `sessionToken`). MinIO happens to accept the bogus token; a stricter S3 would not.
- The duckdb **client read** still needs the S3 endpoint injected client-side
  (vended creds carry no endpoint) — that lives in the UC extension, not here.

## Notes / knobs

- Data dir is bind-mounted host==container (`-v $DIR:$DIR`) and the server is told
  where via `DUCKTEST_UC_DATA_DIR`; `./run <hostdir>` sets both. Pick `<hostdir>` in a
  Docker-shareable location (a temp dir under `/var/folders`, `/tmp`, or `/Users`
  works on macOS). Default when unset: `/home/unitycatalog/etc/data` (in-container
  clients only).
- `build --rebuild-base` forces a fresh source build; otherwise the base image is
  reused once built (it is keyed to the checkout's git ref, so a new commit rebuilds).
- UC source checkout: `UC_REPO=/path` or `--uc-repo /path` (default
  `~/src/d/unitycatalog`). `build_image` fetches + switches it (detached) to `UC_REF`
  / `--uc-ref` (default `v0.5.1`, from `UC_REMOTE`, default `origin`); pass
  `--no-switch` to build the current HEAD as-is. It un-applies its own `patches/`
  before switching, but *unrelated* uncommitted changes make it bail — commit/stash first.
- Env overrides: `UC_REPO`, `UC_REF`, `UC_REMOTE`, `DUCKTEST_UC_CONTAINER`, `DUCKTEST_UC_PORT`,
  `FINAL_IMAGE`, `BASE_IMAGE`, `STABLE_ALIAS`.
- Baking the catalog/schema into the image at build time (instead of seeding at
  startup) is possible later if startup seeding proves too slow; startup seeding is
  used now because it keeps the fresh H2 and the fresh bind mount consistent.
