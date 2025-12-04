PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=unity_catalog
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=parquet;httpfs

#FULL_TEST_EXTENSION_DEPS=tpcds;tpch TODO: add

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

######################################## WRITE TESTS #########################################################

# Write tests use a remote databricks server. Requires setting the env variables from:
#
#     op read op://testing-rw/databricks_free/_env | op inject
#
# Then also setting:
#
#     export DATABRICKS_WRITE_TEST_CATALOG=duckdb_write_testing
#     export DATABRICKS_WRITE_TEST_SCHEMA=test_schema_<some_random_string you can choose yourself>
#
# Then just run the following targets in order. Note that tests can be ran once, consequent runs may fail

write_tests_prepare:
	# fast fail databricks-connect req for py3.12
	python3 --version | grep -q '^Python 3[.]12[.]'
	python3 -m venv venv
	./venv/bin/pip3 install -r scripts/requirements.txt
	./venv/bin/python3 scripts/copy_write_test_data.py ${DATABRICKS_WRITE_TEST_CATALOG}.source ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}

write_tests_run:
	./build/release/test/unittest test/sql/databricks/write_tests/*

# WARNING: does not delete the underlying data TODO:
write_tests_cleanup:
	./venv/bin/python3 scripts/clean_test_data.py ${DATABRICKS_WRITE_TEST_CATALOG}.${DATABRICKS_WRITE_TEST_SCHEMA}

##############################################################################################################
