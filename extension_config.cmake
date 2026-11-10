# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(unity_catalog
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# NOTE: replace with SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../delta for local dev
duckdb_extension_load(delta
    GIT_URL https://github.com/duckdb/duckdb-delta
    GIT_TAG 0d01afade09e956a676cf29d3bb9549134140021 # main 2026-11-10 pin
    SUBMODULES extension-ci-tools
)
