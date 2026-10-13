# Standalone Catch runner for the unity_catalog extension's C++ unit tests --
# pure logic that no sqllogictest .test can drive in isolation (filter
# serialization, position-set application). Mirrors the httpfs test/unittest
# pattern (httpfs names its exe unittest_httpfs; we use the more generic,
# greppable unittest_cpp): an executable built entirely from the extension repo,
# so there is NO edit to the vendored duckdb submodule. It runs ONLY these C++
# Catch cases -- the base `unittest` binary still owns every sqllogictest
# .test/.py file. Build + run on demand: cmake --build build/<variant> --target
# unittest_cpp build/<variant>/test/unittest_cpp             # all cases
# build/<variant>/test/unittest_cpp "[uc][irc]" # one tag
include_directories(${CMAKE_SOURCE_DIR}/test/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party/catch)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../src/include)

add_executable(
  unittest_cpp EXCLUDE_FROM_ALL main.cpp test_uc_irc_expression.cpp
                                test_uc_position_delete_filter.cpp)
set_target_properties(unittest_cpp PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                              ${CMAKE_BINARY_DIR}/test)

target_link_libraries(unittest_cpp test_helpers duckdb_static)
link_extension_libraries(unittest_cpp "")
link_threads(unittest_cpp "")
