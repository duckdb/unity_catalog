PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension config.
EXT_NAME=unity_catalog
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
# json: bodies that read a _delta_log commit back as JSON.
DEFAULT_TEST_EXTENSION_DEPS=parquet;httpfs;tpch;tpcds;json

# Defaults default to the uv-built .venv (see `make venv`); override any on the CLI.
UV      ?= uv
VENV    ?= .venv
PYTEST  ?= $(VENV)/bin/python -m pytest
export DUCKTEST_UC_IMAGE ?= ghcr.io/benfleis/ducktest-unitycatalog:local
UC_REPO ?= $(HOME)/src/d/unitycatalog
ENV_DATABRICKS_CMD ?= scripts/env_databricks   # injects DATABRICKS_* creds -- read the script to hack at it

# Build targets (release/debug/…).
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Test venv from pyproject: uv sync installs the dev group (driver[xdist] + databricks-sdk); the driver's
# pytest11 entry point registers the plugin. No `venv` symlink -- it's `.venv` all the way down.
.PHONY: venv
venv:
	$(UV) sync

# `make test` == default oss_local suite (smoke); hooks the ci-tools chain so the extension builds first.
test_release_internal:
	$(PYTEST) test/oss_local

# `make test_all` == oss_local + the creds-gated databricks suite.
.PHONY: test_all
test_all: test test_databricks

.PHONY: test_databricks
test_databricks:
	$(ENV_DATABRICKS_CMD) $(PYTEST) test/databricks

# `make publish_image` == the 3-step promote in scripts/oss_uc_image/README.md (docker login ghcr first).
.PHONY: publish_image
publish_image:
	@test -f "$(UC_REPO)/build.sbt" || { echo "set UC_REPO=/path/to/unitycatalog (no build.sbt at '$(UC_REPO)')" >&2; exit 1; }
	UC_REPO="$(UC_REPO)" scripts/oss_uc_image/build_image --push
	scripts/oss_uc_image/smoke_test
	UC_REPO="$(UC_REPO)" scripts/oss_uc_image/build_image --merge --alias ci
