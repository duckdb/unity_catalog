PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=unity_catalog
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Core extensions that we need for crucial testing
# json: bodies that read a _delta_log commit back as JSON.
DEFAULT_TEST_EXTENSION_DEPS=parquet;httpfs;tpch;tpcds;json

#FULL_TEST_EXTENSION_DEPS=tpcds;tpch TODO: add

# uv should work if created as suggested in venv: below, but allow overrides
PYTHON_PIP=venv/bin/python3 -m pip
PYTHON_BIN=venv/bin/python3

ENV_DATABRICKS_CMD ?= scripts/run_databricks_env
BUILD_DIR ?= ./build/release

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

venv:
	# NOTE: must be py v3.12-3.14;
	# if using uv locally, do:
	# uv venv --python 3.14 && ln -s .venv venv && uv pip install -r scripts/databricks_data_gen/requirements.txt
	python3 --version | grep -q '^Python 3[.]1[2-4][.]'
	python3 -m venv venv
	${PYTHON_PIP} install -r scripts/databricks_data_gen/requirements.txt

# This is to (re)gen the test data in the remote databricks, this does not need to be rerun unless remote data needs refreshing
# Requires the same env variables from the write tests
# NOTE: requires databricks env IN PLACE, and $(BUILD_DIR)/duckdb with UC+Delta extensions loaded
test_data_prepare: venv
	for f in scripts/databricks_data_gen/custom_data_sources/*.sql; do \
		${PYTHON_BIN} scripts/databricks_data_gen/generate_databricks_test_data.py from-custom-sql $$f duckdb_testing.main; \
	done
	# id_day_managed_duckdb: loop above created empty catalog-managed table via Spark; now insert data via DuckDB UC extension
	# TODO: update gen+table structures to use this for all simple tables; it's currently 1-off for this specific bug fix
	bash -c 'envsubst < scripts/databricks_data_gen/duckdb_data_sources/insert_id_day_managed_duckdb.sql | $(BUILD_DIR)/duckdb'
	${PYTHON_BIN} scripts/databricks_data_gen/generate_databricks_test_data.py from-duckdb-sql scripts/databricks_data_gen/duckdb_data_sources/tpcds_sf0_01.sql duckdb_testing.tpcds_sf0_01
	${PYTHON_BIN} scripts/databricks_data_gen/generate_databricks_test_data.py from-duckdb-sql scripts/databricks_data_gen/duckdb_data_sources/tpch_sf0_01.sql duckdb_testing.tpch_sf0_01

################################################
# Databricks Tests
################################################

# Runs the regular databricks tests (non-write) with credentials from 1password
run_databricks_tests:
	${ENV_DATABRICKS_CMD} $(BUILD_DIR)/test/unittest "test/sql/databricks/*"

################################################
# Databricks Write Tests
################################################

# These tests will automatically load some data into a fresh schema in databricks, then allows you to run some tests on it
# NOTE: Easiest way is to just do make `run_write_tests` which runs all steps

# Before running this, ensure your env is configured:
#    >   . scripts/run_databricks_env

# Prepare the main write test files by copying the tables from the `source` schema to the `{DATABRICKS_WRITE_TEST_SCHEMA}` schema
write_tests_prepare: venv
	${PYTHON_BIN} scripts/databricks_data_gen/generate_databricks_test_data.py copy ${DATABRICKS_WRITE_TEST_CATALOG}.source ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}
	${PYTHON_BIN} scripts/databricks_data_gen/generate_databricks_test_data.py copy ${DATABRICKS_WRITE_TEST_CATALOG}.source ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA} --catalog-managed
	# Declared types the copy above cannot carry: it is a CTAS, and a CHAR width does not survive one.
	for f in scripts/databricks_data_gen/write_data_sources/*.sql; do \
		${PYTHON_BIN} scripts/databricks_data_gen/generate_databricks_test_data.py from-custom-sql $$f ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}; \
	done

write_tests_run:
	$(BUILD_DIR)/test/unittest "test/sql/databricks/write_tests/*"

write_tests_cleanup:
	${PYTHON_BIN} scripts/databricks_data_gen/clean_test_data.py ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}

# - fetches credentials from 1password
# - generates new schema name
# - sets all env variables
# - copies data into fresh table
# - runs write tests
# - cleans up data
# NOTE: may leave some data around on s3, needs investigation!
run_write_tests: venv
	RUN_WRITE_TESTS=1 ${ENV_DATABRICKS_CMD} $(MAKE) -k write_tests_prepare write_tests_run write_tests_cleanup
