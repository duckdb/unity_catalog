PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=unity_catalog
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=parquet;httpfs;tpch;tpcds

#FULL_TEST_EXTENSION_DEPS=tpcds;tpch TODO: add

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

venv:
	python3.12 --version | grep -q '^Python 3[.]12[.]'
	python3.12 -m venv venv
	./venv/bin/pip3 install -r scripts/databricks_data_gen/requirements.txt

# This is to (re)gen the test data in the remote databricks, this does not need to be rerun unless remote data needs refreshing
# Requires the same env variables from the write tests
test_data_prepare: venv
	for f in scripts/databricks_data_gen/custom_data_sources/*.sql; do \
		./venv/bin/python3 scripts/databricks_data_gen/generate_databricks_test_data.py from-custom-sql $$f duckdb_testing.main; \
	done
	./venv/bin/python3 scripts/databricks_data_gen/generate_databricks_test_data.py from-duckdb-sql scripts/databricks_data_gen/duckdb_data_sources/tpcds_sf0_01.sql duckdb_testing.tpcds_sf0_01
	./venv/bin/python3 scripts/databricks_data_gen/generate_databricks_test_data.py from-duckdb-sql scripts/databricks_data_gen/duckdb_data_sources/tpch_sf0_01.sql duckdb_testing.tpch_sf0_01

################################################
# Databricks Tests
################################################

# Runs the regular databricks tests (non-write) with credentials from 1password
run_databricks_tests:
	. scripts/databricks_data_gen/run_databricks_env && \
	./build/debug/test/unittest "test/sql/databricks/*"

################################################
# Databricks Write Tests
################################################

# These tests will automatically load some data into a fresh schema in databricks, then allows you to run some tests on it
# NOTE: Easiest way is to just do make `run_write_tests` which runs all steps

# TODO: test data in `source` schema is currently hand generated, this should be cleaned up

# Before running this, ensure your env is configured:
#    >   . scripts/databricks_data_gen/run_databricks_env

# Prepare the main write test files by copying the tables from the `source` schema to the `{DATABRICKS_WRITE_TEST_SCHEMA}` schema
write_tests_prepare: venv
	./venv/bin/python3 scripts/databricks_data_gen/generate_databricks_test_data.py copy ${DATABRICKS_WRITE_TEST_CATALOG}.source ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}
	./venv/bin/python3 scripts/databricks_data_gen/generate_databricks_test_data.py copy ${DATABRICKS_WRITE_TEST_CATALOG}.source ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA} --catalog-managed

write_tests_run:
	./build/debug/test/unittest "test/sql/databricks/write_tests/*"

write_tests_cleanup:
	./venv/bin/python3 scripts/databricks_data_gen/clean_test_data.py ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}

# - fetches credentials from 1password
# - generates new schema name
# - sets all env variables
# - copies data into fresh table
# - runs write tests
# - cleans up data
# NOTE: may leave some data around on s3, needs investigation!
run_write_tests: venv
	RUN_WRITE_TESTS=1  . scripts/databricks_data_gen/run_databricks_env && \
	( $(MAKE) write_tests_prepare && $(MAKE) write_tests_run; EXIT=$$?; $(MAKE) write_tests_cleanup; exit $$EXIT )
