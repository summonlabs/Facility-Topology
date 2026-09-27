# Facility Topology - downstream consumption check
#
# A standalone CMake project that consumes an *installed* Facility Topology
# package through find_package(). It is intentionally not part of the parent
# build: building it in-tree would not prove distributability.
#
# Usage:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build build
#   ./build/downstream_consumer

cmake_minimum_required(VERSION 3.20)

project(facility_topology_downstream
        VERSION 1.0.0
        DESCRIPTION "Independent consumer of the Facility Topology package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(facility_topology 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE dccp::facility_topology)

message(STATUS "found Facility Topology ${facility_topology_VERSION}")
message(STATUS "include directories: ${FACILITY_TOPOLOGY_INCLUDE_DIRS}")
