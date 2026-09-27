// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Facility Topology - DCCP Tranche 1 (Canonical Facility State).

#ifndef DCCP_FACILITY_TOPOLOGY_VERSION_HPP
#define DCCP_FACILITY_TOPOLOGY_VERSION_HPP

#include <cstdint>
#include <string_view>

/// Semantic version of the Facility Topology library and of the canonical
/// serialization schema it publishes.
#define FACILITY_TOPOLOGY_VERSION_MAJOR 1
#define FACILITY_TOPOLOGY_VERSION_MINOR 0
#define FACILITY_TOPOLOGY_VERSION_PATCH 0

namespace dccp::facility_topology {

/// Version of the C++ library.
inline constexpr std::string_view kLibraryVersion = "1.0.0";

/// Version of the canonical text serialization understood by this build.
/// Bumping this value is a breaking change to durable and exported files.
inline constexpr std::uint32_t kCanonicalSchemaVersion = 1;

/// Canonical banner line of every serialized topology document.
inline constexpr std::string_view kCanonicalBanner = "ftop/1";

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_VERSION_HPP
