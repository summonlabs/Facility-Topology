// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_DIFF_HPP
#define DCCP_FACILITY_TOPOLOGY_DIFF_HPP

#include <string>
#include <vector>

#include "dccp/facility_topology/model.hpp"
#include "dccp/facility_topology/topology.hpp"

namespace dccp::facility_topology {

/// One node whose label changed between generations.
struct NodeLabelChange {
  NodeId id;
  std::string before;
  std::string after;

  friend bool operator==(const NodeLabelChange&, const NodeLabelChange&) noexcept = default;
};

/// One node whose zone scope changed between generations.
struct NodeZoneScopeChange {
  NodeId id;
  NodeKind before = NodeKind::Facility;
  NodeKind after = NodeKind::Facility;

  friend bool operator==(const NodeZoneScopeChange&, const NodeZoneScopeChange&) noexcept = default;
};

/// Structural difference between two generations.
///
/// Every vector is in canonical order, so two diffs of the same pair of
/// generations are byte-identical when rendered. A move appears as one removed
/// and one added containment edge; the moved node itself is not "added".
struct TopologyDiff {
  TopologyGeneration from;
  TopologyGeneration to;

  std::vector<NodeId> nodes_added;
  std::vector<NodeId> nodes_removed;
  std::vector<NodeLabelChange> nodes_relabeled;
  std::vector<NodeZoneScopeChange> node_scope_changed;

  std::vector<ContainmentEdge> containment_added;
  std::vector<ContainmentEdge> containment_removed;

  std::vector<AdjacencyEdge> adjacency_added;
  std::vector<AdjacencyEdge> adjacency_removed;

  std::vector<DomainDeclaration> domains_added;
  std::vector<DomainDeclaration> domains_removed;

  std::vector<DomainAssociation> associations_added;
  std::vector<DomainAssociation> associations_removed;

  /// True when no structural difference was found. Provenance, timestamps,
  /// generation numbers and lineage are deliberately not compared.
  bool empty() const noexcept;

  /// Total number of recorded changes.
  std::size_t change_count() const noexcept;

  /// Deterministic multi-line rendering.
  std::string to_string() const;
};

/// Computes the structural difference from `from` to `to`.
///
/// Both snapshots must be valid. Ordering, iteration and rendering are
/// deterministic and independent of how either generation was produced.
Result<TopologyDiff> diff(const TopologySnapshot& from, const TopologySnapshot& to);

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_DIFF_HPP
