# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Facility Topology - DCCP Tranche 1 (Canonical Facility State).
#
# The primary product surface is the library target `facility_topology`,
# exported as `dccp::facility_topology`. The inspection CLI, examples, tests
# and benchmarks are optional and are never installed except for the CLI.

cmake_minimum_required(VERSION 3.20)

# Keep in sync with include/dccp/facility_topology/version.hpp; the test suite
# compares this value against the compiled-in library version.
project(facility_topology
        VERSION 1.0.0
        DESCRIPTION "Generation-bound structural model of a data center facility"
        HOMEPAGE_URL "https://github.com/summonlabs/Facility-Topology"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FACILITY_TOPOLOGY_BUILD_TESTS "Build the Facility Topology test suite" ON)
option(FACILITY_TOPOLOGY_BUILD_TOOLS "Build the ftopctl inspection tool" ON)
option(FACILITY_TOPOLOGY_BUILD_EXAMPLES "Build the Facility Topology examples" ON)
option(FACILITY_TOPOLOGY_BUILD_BENCHMARKS "Build the Facility Topology benchmarks" ON)
option(FACILITY_TOPOLOGY_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(FACILITY_TOPOLOGY_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------

add_library(facility_topology_warnings INTERFACE)
add_library(dccp::facility_topology_warnings ALIAS facility_topology_warnings)

if(MSVC)
  target_compile_options(facility_topology_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  if(FACILITY_TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(facility_topology_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(facility_topology_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(FACILITY_TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(facility_topology_warnings INTERFACE -Werror)
  endif()
endif()

add_library(facility_topology_sanitizers INTERFACE)
if(FACILITY_TOPOLOGY_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(facility_topology_sanitizers INTERFACE /fsanitize=address)
    target_link_options(facility_topology_sanitizers INTERFACE /fsanitize=address)
  else()
    target_compile_options(facility_topology_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(facility_topology_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(facility_topology
  src/result.cpp
  src/text.cpp
  src/digest.cpp
  src/model.cpp
  src/clock.cpp
  src/graph_data.cpp
  src/topology.cpp
  src/validate.cpp
  src/canonical.cpp
  src/diff.cpp
  src/mutation.cpp
  src/file_ops.cpp
  src/store.cpp)
add_library(dccp::facility_topology ALIAS facility_topology)

target_compile_features(facility_topology PUBLIC cxx_std_20)
target_include_directories(facility_topology
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(facility_topology
  PRIVATE
    $<BUILD_INTERFACE:facility_topology_warnings>
    $<BUILD_INTERFACE:facility_topology_sanitizers>)
set_target_properties(facility_topology PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME facility_topology)

# ---------------------------------------------------------------------------
# Inspection tool
# ---------------------------------------------------------------------------

if(FACILITY_TOPOLOGY_BUILD_TOOLS)
  add_executable(ftopctl tools/ftopctl/main.cpp tools/ftopctl/script.cpp)
  target_link_libraries(ftopctl PRIVATE dccp::facility_topology facility_topology_warnings facility_topology_sanitizers)
  target_include_directories(ftopctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(ftopctl PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(FACILITY_TOPOLOGY_BUILD_EXAMPLES)
  foreach(example_name IN ITEMS
      build_facility
      durable_store
      consumer_query)
    add_executable(facility_topology_example_${example_name} examples/${example_name}.cpp)
    target_link_libraries(facility_topology_example_${example_name}
      PRIVATE dccp::facility_topology facility_topology_warnings facility_topology_sanitizers)
    set_target_properties(facility_topology_example_${example_name} PROPERTIES CXX_EXTENSIONS OFF)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(FACILITY_TOPOLOGY_BUILD_TESTS)
  enable_testing()

  add_library(facility_topology_test_support STATIC
    tests/test_main.cpp
    tests/child_process.cpp)
  target_link_libraries(facility_topology_test_support
    PUBLIC dccp::facility_topology facility_topology_warnings facility_topology_sanitizers)
  target_include_directories(facility_topology_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # The test suite compares the compiled-in library version with the CMake
  # project version so the two cannot drift apart silently.
  target_compile_definitions(facility_topology_test_support
    PUBLIC FACILITY_TOPOLOGY_CMAKE_VERSION="${PROJECT_VERSION}")
  set_target_properties(facility_topology_test_support PROPERTIES CXX_EXTENSIONS OFF)

  add_executable(facility_topology_tests
    tests/tests_text.cpp
    tests/tests_digest.cpp
    tests/tests_model.cpp
    tests/tests_topology.cpp
    tests/tests_mutation.cpp
    tests/tests_canonical.cpp
    tests/tests_store.cpp
    tests/tests_recovery.cpp
    tests/tests_property.cpp
    tests/tests_adversarial.cpp
    tests/tests_concurrency.cpp
    tests/tests_internal.cpp
    tests/tests_multiprocess.cpp)
  target_link_libraries(facility_topology_tests PRIVATE facility_topology_test_support)
  target_include_directories(facility_topology_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(facility_topology_tests PROPERTIES CXX_EXTENSIONS OFF)

  # The tool integration tests run ftopctl as an independent process.
  if(FACILITY_TOPOLOGY_BUILD_TOOLS)
    target_compile_definitions(facility_topology_tests
      PRIVATE FACILITY_TOPOLOGY_CLI_PATH="$<TARGET_FILE:ftopctl>")
    add_dependencies(facility_topology_tests ftopctl)
  endif()

  add_test(NAME facility_topology_tests COMMAND facility_topology_tests)
  set_tests_properties(facility_topology_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(FACILITY_TOPOLOGY_BUILD_BENCHMARKS)
  add_executable(facility_topology_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(facility_topology_benchmarks
    PRIVATE dccp::facility_topology facility_topology_warnings facility_topology_sanitizers)
  set_target_properties(facility_topology_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS facility_topology
  EXPORT facility_topologyTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FACILITY_TOPOLOGY_BUILD_TOOLS)
  install(TARGETS ftopctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(FACILITY_TOPOLOGY_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/facility_topology)

install(EXPORT facility_topologyTargets
  FILE facility_topologyTargets.cmake
  NAMESPACE dccp::
  DESTINATION ${FACILITY_TOPOLOGY_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/facility_topologyConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/facility_topologyConfig.cmake
  INSTALL_DESTINATION ${FACILITY_TOPOLOGY_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/facility_topologyConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/facility_topologyConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/facility_topologyConfigVersion.cmake
  DESTINATION ${FACILITY_TOPOLOGY_CMAKE_INSTALL_DIR})

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
  ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
