# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(unity_catalog
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# TODO: restore to duckdb/duckdb
duckdb_extension_load(delta
    GIT_URL https://github.com/samansmink/duckdb-delta
    GIT_TAG 2898f907078c805a2972382ffc7a99026f6f60f6
    SUBMODULES extension-ci-tools
)
