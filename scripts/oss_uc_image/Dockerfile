# Thin overlay on top of a locally-built unitycatalog image (built by build
# from a UC source checkout; see $UC_REPO / --uc-repo). Keeping the source build
# and this overlay separate means we don't re-run sbt when only the duck
# config/scripts change, and the UC checkout stays untouched. build always
# passes --build-arg BASE=...; the default below is only for standalone builds.
ARG BASE=ghcr.io/benfleis/ducktest-unitycatalog-base:local
FROM ${BASE}

USER root

# Duck-only server config + seeding entrypoint.
COPY server.properties /home/unitycatalog/etc/conf/server.properties
COPY entrypoint.sh     /home/unitycatalog/bin/ducktest-entrypoint.sh

RUN set -eux; \
    chmod 0755 /home/unitycatalog/bin/ducktest-entrypoint.sh; \
    # Let the entrypoint add a passwd entry for an arbitrary --user uid (delta-kernel needs a
    # resolvable effective user): make /etc/passwd writable by the unitycatalog gid (2008 = the run gid).
    chgrp unitycatalog /etc/passwd; chmod g+w /etc/passwd; \
    # Start from an empty metastore: drop the bundled H2 db (the sample
    # `unity` catalog) so the image is duck-only and seeds deterministically.
    rm -f /home/unitycatalog/etc/db/h2db*.db; \
    # Drop bundled sample table/volume data.
    rm -rf /home/unitycatalog/etc/data/managed /home/unitycatalog/etc/data/external; \
    mkdir -p /home/unitycatalog/etc/data/duck/cmt /home/unitycatalog/etc/data/duck/plain; \
    # S3-readiness: put a conf dir on the CLI + server classpath so a core-site.xml
    # the entrypoint generates at runtime (only when DUCKTEST_UC_S3_ENDPOINT is set,
    # e.g. MinIO) is loaded by the S3A writer. Dormant for local FS and AWS S3 (no file
    # written -> default endpoint).
    mkdir -p /home/unitycatalog/conf; \
    for cp in /home/unitycatalog/examples/cli/target/classpath /home/unitycatalog/server/target/classpath; do \
      [ -f "$cp" ] && printf ':/home/unitycatalog/conf' >> "$cp" || true; \
    done; \
    chown -R unitycatalog:unitycatalog /home/unitycatalog/etc /home/unitycatalog/conf; \
    chmod -R 0770 /home/unitycatalog/etc /home/unitycatalog/conf

USER unitycatalog
WORKDIR /home/unitycatalog

EXPOSE 8080
ENTRYPOINT ["./bin/ducktest-entrypoint.sh"]
