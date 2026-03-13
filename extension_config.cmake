# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(unity_catalog
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# NOTE: replace with SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../delta for local dev
duckdb_extension_load(delta
    GIT_URL https://github.com/duckdb/duckdb-delta
    GIT_TAG 0e8892c8708329dddb618306e4f35eac0b0764bf
    SUBMODULES extension-ci-tools
)
