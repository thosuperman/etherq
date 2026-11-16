# SPDX-License-Identifier: GPL-3.0-or-later
#
# EtherQ - EtherCAT for QNX OS 8.0
#
# Upstream SOEM is fetched by URL (pinned tarball + SHA-256), never vendored or
# forked. The QNX port in port/soem/ is copied over the fetched tree; it only
# adds files (osal/qnx, oshw/qnx, cmake/QNX.cmake) and changes no upstream file.
#
# QNX build:   cmake -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800-x86_64.cmake
# Host build:  cmake -B build-host     (portable core + unit tests only)
cmake_minimum_required(VERSION 3.28)
project(etherq VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# CMake defines QNXNTO (not QNX) for QNX targets; use an explicit flag
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(ETHERQ_QNX ON)
else()
  set(ETHERQ_QNX OFF)
endif()

option(ETHERQ_FETCH_SOEM "Fetch and build SOEM (required for the QNX daemon)" ${ETHERQ_QNX})
option(ETHERQ_BUILD_TESTS "Build host unit tests" ON)

set(SOEM_URL "https://github.com/OpenEtherCATsociety/SOEM/archive/refs/tags/v2.0.1.tar.gz"
    CACHE STRING "SOEM release tarball")
set(SOEM_SHA256 "933208261f1737ae530d25b915c1ce459413dcdba13d92e95af3726380601648"
    CACHE STRING "SHA-256 of the SOEM tarball")

# ---------------------------------------------------------------- portable core
add_library(ecatcore STATIC src/ecatcore/ecatcore.c)
target_include_directories(ecatcore PUBLIC include src/ecatcore)
target_compile_options(ecatcore PRIVATE -Wall -Wextra)

if(ETHERQ_BUILD_TESTS AND NOT ETHERQ_QNX)
  enable_testing()
  add_executable(test_ecatcore tests/c/test_ecatcore.c)
  target_link_libraries(test_ecatcore PRIVATE ecatcore)
  target_compile_options(test_ecatcore PRIVATE -Wall -Wextra)
  add_test(NAME ecatcore COMMAND test_ecatcore)
endif()

# ------------------------------------------------------------------------ SOEM
if(ETHERQ_FETCH_SOEM)
  include(FetchContent)
  set(SOEM_BUILD_SAMPLES ON CACHE BOOL "Build SOEM samples (slaveinfo, simple_ng)" FORCE)
  FetchContent_Declare(soem URL "${SOEM_URL}" URL_HASH SHA256=${SOEM_SHA256})
  FetchContent_GetProperties(soem)
  if(NOT soem_POPULATED)
    FetchContent_Populate(soem)
  endif()
  # Copy the QNX port overlay on every configure (not only at download time) and
  # re-run configure whenever a port file changes, so edits always reach the build.
  file(GLOB_RECURSE ETHERQ_PORT_FILES CONFIGURE_DEPENDS
       RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/port/soem" "${CMAKE_CURRENT_SOURCE_DIR}/port/soem/*")
  foreach(f IN LISTS ETHERQ_PORT_FILES)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/port/soem/${f}" "${soem_SOURCE_DIR}/${f}" COPYONLY)
  endforeach()
  add_subdirectory("${soem_SOURCE_DIR}" "${soem_BINARY_DIR}")
endif()

# ------------------------------------------------------------- QNX components
if(ETHERQ_QNX)
  add_executable(ecatd
    src/ecatd/main.c
    src/ecatd/master.c
    src/ecatd/resmgr.c
    src/ecatd/privileges.c)
  target_include_directories(ecatd PRIVATE include src/ecatd)
  target_link_libraries(ecatd PRIVATE soem ecatcore)
  target_compile_options(ecatd PRIVATE -Wall -Wextra)

  add_executable(ecatctl src/ecatctl/ecatctl.c)
  target_include_directories(ecatctl PRIVATE include)
  target_compile_options(ecatctl PRIVATE -Wall -Wextra)

  add_executable(axisdemo src/axisdemo/axisdemo.c)
  target_include_directories(axisdemo PRIVATE include)
  target_link_libraries(axisdemo PRIVATE m)
  target_compile_options(axisdemo PRIVATE -Wall -Wextra)

  add_executable(blink examples/blink/blink.c)
  target_include_directories(blink PRIVATE include)
  target_compile_options(blink PRIVATE -Wall -Wextra)

  install(TARGETS ecatd ecatctl axisdemo blink DESTINATION bin)
  install(TARGETS slaveinfo simple_ng DESTINATION bin)
endif()
