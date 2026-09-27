// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal canonical-encoding helpers shared by the digest, serialization and
// diff implementations. Private to the library.

#ifndef DCCP_FACILITY_TOPOLOGY_SRC_CANONICAL_INTERNAL_HPP
#define DCCP_FACILITY_TOPOLOGY_SRC_CANONICAL_INTERNAL_HPP

#include <string>

#include "dccp/facility_topology/digest.hpp"
#include "graph_data.hpp"

namespace dccp::facility_topology {

/// Read-only bridge to a snapshot's storage for library-internal encoders.
struct SnapshotAccess {
  static const GraphData* data(const TopologySnapshot& snapshot) noexcept { return snapshot.data_.get(); }
};

/// Digest of the structural content of a generation.
///
/// The encoding is a canonical, line-oriented rendering of nodes, containment,
/// adjacency, domain declarations and domain associations in canonical order.
/// It deliberately excludes the generation number, lineage, provenance values
/// and timestamps, so two generations that describe the same facility have the
/// same content digest.
Digest structure_digest(const GraphData& data);

/// Appends a quoted canonical field, or "-" when the value is empty.
void append_optional_quoted(std::string& out, const std::string& value);

/// Appends a quoted canonical field (always quoted, possibly empty).
void append_quoted(std::string& out, const std::string& value);

/// Appends " <actor> <\"source\"> <\"reason\"|-> <recorded-at|->".
void append_provenance_suffix(std::string& out, const ProvenanceRecord& provenance);

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_SRC_CANONICAL_INTERNAL_HPP
