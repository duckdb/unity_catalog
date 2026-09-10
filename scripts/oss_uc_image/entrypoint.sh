#!/bin/bash
# Entrypoint for the unitycatalog "duck" test image.
#
# Runs inside the container as user `unitycatalog`, cwd /home/unitycatalog.
# Order matters: the data dir is a bind mount, so anything that touches it must
# happen AFTER the mount (i.e. here at runtime), not at image-build time.
#
#   1. ensure the (possibly freshly-mounted, empty) data dir layout exists
#   2. start the UC server in the background
#   3. wait until it answers
#   4. idempotently seed: catalog `duck` + schemas `duck.cmt` / `duck.plain`
#   5. hand the foreground back to the server (and forward signals for clean stop)
set -euo pipefail

UC_HOME=/home/unitycatalog
# Table data root. UC records ABSOLUTE file:// locations for tables, so a client
# can only open them if the path resolves in its own filesystem namespace:
#   - client INSIDE the container: default $UC_HOME/etc/data is fine.
#   - client on the HOST (e.g. the duckdb unittest binary): run.sh sets
#     DUCKTEST_UC_DATA_DIR to a path bind-mounted IDENTICALLY on host and container,
#     so an absolute path means the same files on both sides.
# Under the root: duck/cmt/ (catalog managed tables; UC nests __unitystorage here) and
# duck/plain/ (plain EXTERNAL tables). H2 metastore stays at $UC_HOME/etc/db.
DATA_DIR="${DUCKTEST_UC_DATA_DIR:-$UC_HOME/etc/data}"
DUCK_DIR="$DATA_DIR/duck"
CMT_ROOT="$DUCK_DIR/cmt"
PLAIN_ROOT="$DUCK_DIR/plain"

cd "$UC_HOME"

# We're usually run with `--user $(id -u):2008` (see run/README) so bind-mounted files are owned by the
# invoking HOST uid. That uid typically has no /etc/passwd entry, which makes the JVM inside
# delta-kernel die with "Invalid UID, could not determine effective user". Give it a name so getpwuid()
# resolves (/etc/passwd is group-writable by our gid -- see Dockerfile). No-op when run as the named user.
_uid="$(id -u)"
if ! awk -F: -v u="$_uid" '$3==u{f=1} END{exit !f}' /etc/passwd; then
	echo "ducktest:x:${_uid}:$(id -g):duck:${UC_HOME}:/sbin/nologin" >>/etc/passwd
fi

# 1. Layout. The bind mount may shadow the image's dirs with an empty host dir.
mkdir -p "$CMT_ROOT" "$PLAIN_ROOT"

# 1b. Optional S3-compatible (e.g. MinIO) endpoint for the CLI's S3A writer.
# Only when DUCKTEST_UC_S3_ENDPOINT is set do we write a core-site.xml (the conf dir is
# already on the classpath from the image build). Unset => local FS / AWS S3 with
# the default endpoint. Static keys here cover EXTERNAL tables (no UC vending);
# CMTs additionally need s3.* vending config in server.properties.
if [ -n "${DUCKTEST_UC_S3_ENDPOINT:-}" ]; then
	cat >"$UC_HOME/conf/core-site.xml" <<EOF
<?xml version="1.0"?>
<configuration>
  <property><name>fs.s3a.endpoint</name><value>${DUCKTEST_UC_S3_ENDPOINT}</value></property>
  <property><name>fs.s3a.endpoint.region</name><value>${DUCKTEST_UC_S3_REGION:-us-east-1}</value></property>
  <property><name>fs.s3a.path.style.access</name><value>true</value></property>
  <property><name>fs.s3a.connection.ssl.enabled</name><value>${DUCKTEST_UC_S3_SSL:-false}</value></property>
  <property><name>fs.s3a.access.key</name><value>${DUCKTEST_UC_S3_KEY:-}</value></property>
  <property><name>fs.s3a.secret.key</name><value>${DUCKTEST_UC_S3_SECRET:-}</value></property>
  <property><name>fs.s3a.aws.credentials.provider</name><value>org.apache.hadoop.fs.s3a.SimpleAWSCredentialsProvider</value></property>
</configuration>
EOF
	echo "ducktest-entrypoint: wrote core-site.xml for S3 endpoint $DUCKTEST_UC_S3_ENDPOINT"
else
	rm -f "$UC_HOME/conf/core-site.xml"
fi

# 2. Start server in background.
./bin/start-uc-server &
SERVER_PID=$!

# Forward termination so `docker stop` shuts the JVM down cleanly.
term() { kill -TERM "$SERVER_PID" 2>/dev/null || true; }
trap term TERM INT

# 3. Wait for readiness by polling a cheap, auth-free CLI call.
echo "ducktest-entrypoint: waiting for UC server..."
ready=
for _ in $(seq 1 90); do
	if ./bin/uc catalog list >/dev/null 2>&1; then
		ready=1
		break
	fi
	if ! kill -0 "$SERVER_PID" 2>/dev/null; then
		echo "ducktest-entrypoint: server exited before becoming ready" >&2
		wait "$SERVER_PID"
		exit 1
	fi
	sleep 1
done
if [ -z "$ready" ]; then
	echo "ducktest-entrypoint: server did not become ready in time" >&2
	term
	exit 1
fi

# 4. Idempotent seed. `get` first so a restart against persisted state is a no-op.
uc() { ./bin/uc "$@"; }

if ! uc catalog get --name duck >/dev/null 2>&1; then
	uc catalog create --name duck --comment "DuckDB test catalog"
	echo "ducktest-entrypoint: created catalog 'duck'"
fi

# cmt: schema cmt+storage=MANAGED. Its storage_root is duck/cmt, so UC
# allocates per-table locations (and its __unitystorage tree) underneath it.
if ! uc schema get --full_name duck.cmt >/dev/null 2>&1; then
	uc schema create --catalog duck --name cmt \
		--comment "catalog-managed (MANAGED) Delta tables" \
		--storage_root "$CMT_ROOT"
	echo "ducktest-entrypoint: created schema 'duck.cmt' -> $CMT_ROOT"
fi

# plain: Delta tables stroage=EXTERNAL (plain log, not catalog-managed). No cmt
# storage_root -- each plain table brings its own location, which uctl.sh
# places under $PLAIN_ROOT (a sibling of duck/cmt, so it neither sits under nor
# above cmt storage -> no external-location registration needed).
if ! uc schema get --full_name duck.plain >/dev/null 2>&1; then
	uc schema create --catalog duck --name plain \
		--comment "plain (EXTERNAL) Delta tables"
	echo "ducktest-entrypoint: created schema 'duck.plain' (plain tables under $PLAIN_ROOT)"
fi

echo "ducktest-entrypoint: ready. catalog 'duck' with schemas 'cmt' and 'plain'."

# 5. Hand off to the server in the foreground.
wait "$SERVER_PID"
