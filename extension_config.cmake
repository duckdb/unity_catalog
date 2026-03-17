# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(unity_catalog
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

duckdb_extension_load(delta
    GIT_URL https://github.com/duckdb/duckdb-delta
    GIT_TAG 928aa1f4db60c0dca7ba41062b574e815e7577e8
    SUBMODULES extension-ci-tools
)
