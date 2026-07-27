cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME unity_catalog)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# Roaring is installed via vcpkg and used to decode Iceberg deletion-vector-v1 puffin blobs
# (see src/uc_puffin.cpp).
find_package(roaring CONFIG REQUIRED)
if(TARGET roaring::roaring-headers)
  get_target_property(ROARING_INCLUDE_DIRS roaring::roaring-headers INTERFACE_INCLUDE_DIRECTORIES)
else()
  get_target_property(ROARING_INCLUDE_DIRS roaring::roaring INTERFACE_INCLUDE_DIRECTORIES)
endif()
if(NOT ROARING_INCLUDE_DIRS OR ROARING_INCLUDE_DIRS STREQUAL "ROARING_INCLUDE_DIRS-NOTFOUND")
  set(ROARING_INCLUDE_DIRS "${roaring_DIR}/../../include")
endif()
include_directories(${ROARING_INCLUDE_DIRS})

add_subdirectory(src)

build_static_extension(${TARGET_NAME} ${ALL_OBJECT_FILES})
build_loadable_extension(${TARGET_NAME} " " ${ALL_OBJECT_FILES})

if(TARGET roaring::roaring-headers)
  target_link_libraries(${EXTENSION_NAME} roaring::roaring roaring::roaring-headers roaring::roaring-headers-cpp)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} roaring::roaring roaring::roaring-headers roaring::roaring-headers-cpp)
else()
  target_link_libraries(${EXTENSION_NAME} roaring::roaring)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} roaring::roaring)
endif()

# C++ (Catch) unit tests for pure logic no sqllogictest .test can drive in isolation. Standalone
# `unittest_cpp` runner, built on demand -- see test/functions/CMakeLists.txt.
if(BUILD_UNITTESTS AND NOT EMSCRIPTEN)
  add_subdirectory(test/functions)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
