// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal storage of a topology generation.
//
// This header is private to the library: consumers only ever see validated
// TopologySnapshot values and the TopologyBuilder mutation surface. Storage is
// a set of ordered maps, which makes every public iteration order canonical
// (NodeId byte order) without a separate sorting step, and gives every lookup
// a deterministic logarithmic cost.
//
// A GraphData is mutable while a builder owns it and immutable once it has
// been frozen into a snapshot.

#ifndef DCCP_FACILITY_TOPOLOGY_SRC_GRAPH_DATA_HPP
#define DCCP_FACILITY_TOPOLOGY_SRC_GRAPH_DATA_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/model.hpp"
#include "dccp/facility_topology/topology.hpp"

namespace dccp::facility_topology {

/// Key of a containment edge. At most one containment edge may exist between
/// a given parent and child, so the parent/child pair is the key and the
/// boundary kind is a property of the edge.
struct ContainmentKey {
  NodeId parent;
  NodeId child;

  friend bool operator<(const ContainmentKey& lhs, const ContainmentKey& rhs) noexcept {
    if (lhs.parent < rhs.parent) {
      return true;
    }
    if (rhs.parent < lhs.parent) {
      return false;
    }
    return lhs.child < rhs.child;
  }
  friend bool operator==(const ContainmentKey&, const ContainmentKey&) noexcept = default;
};

/// Key of an adjacency edge, stored with a canonical endpoint order so that the
/// relation is symmetric and duplicates are impossible.
struct AdjacencyKey {
  NodeId first;
  NodeId second;

  friend bool operator<(const AdjacencyKey& lhs, const AdjacencyKey& rhs) noexcept {
    if (lhs.first < rhs.first) {
      return true;
    }
    if (rhs.first < lhs.first) {
      return false;
    }
    return lhs.second < rhs.second;
  }
  friend bool operator==(const AdjacencyKey&, const AdjacencyKey&) noexcept = default;
};

/// Key of a domain association: a node references a domain at most once.
struct AssociationKey {
  NodeId node;
  DomainId domain;

  friend bool operator<(const AssociationKey& lhs, const AssociationKey& rhs) noexcept {
    if (lhs.node < rhs.node) {
      return true;
    }
    if (rhs.node < lhs.node) {
      return false;
    }
    return lhs.domain < rhs.domain;
  }
  friend bool operator==(const AssociationKey&, const AssociationKey&) noexcept = default;
};

/// Canonicalizes adjacency endpoints: the lesser identity is always `first`.
AdjacencyKey make_adjacency_key(const NodeId& a, const NodeId& b) noexcept;

/// Depth ceiling for any bounded ancestor walk. Valid topologies can never be
/// deeper (the structural schema forbids it), so a walk that exceeds it is
/// either operating on unvalidated candidate data or on a cycle, and stops
/// instead of looping forever.
inline constexpr std::size_t kMaxAncestryWalk = kMaxStructuralDepth + 1;

/// Orders node kinds by their stable numeric value.
struct NodeKindLess {
  bool operator()(NodeKind lhs, NodeKind rhs) const noexcept {
    return node_kind_index(lhs) < node_kind_index(rhs);
  }
};

struct GraphData {
  TopologyGeneration generation;
  /// Limits this generation was validated under; traversals are clamped to
  /// them so a caller can never request an unbounded walk.
  TopologyLimits limits;

  std::map<NodeId, NodeRecord> nodes;
  std::map<ContainmentKey, ContainmentEdge> containment;
  std::map<AdjacencyKey, AdjacencyEdge> adjacency;
  std::map<DomainId, DomainDeclaration> domains;
  std::map<AssociationKey, DomainAssociation> associations;

  // Derived indices. They are maintained incrementally by the builder and are
  // consistent with the maps above at every observable point.
  std::map<NodeId, NodeId> physical_parent;   ///< child -> physical parent
  std::map<NodeId, NodeId> logical_parent;    ///< child -> containing zone
  std::map<NodeId, std::set<NodeId>> children;
  std::map<NodeId, std::set<NodeId>> members;
  std::map<NodeId, std::set<NodeId>> neighbor_index;
  std::map<NodeId, std::set<AssociationKey>> node_associations;
  std::map<DomainId, std::set<NodeId>> domain_members;
  std::map<NodeKind, std::set<NodeId>, NodeKindLess> kind_index;

  // -- structural helpers (index access) ------------------------------------

  const NodeRecord* find_node(const NodeId& id) const;
  bool has_node(const NodeId& id) const;
  std::optional<NodeKind> kind_of(const NodeId& id) const;
  const std::set<NodeId>& children_of(const NodeId& id) const;
  const std::set<NodeId>& members_of(const NodeId& id) const;
  const std::set<NodeId>& neighbors_of(const NodeId& id) const;
  const std::set<NodeId>& nodes_of_kind(NodeKind kind) const;
  const std::set<NodeId>& nodes_in_domain(const DomainId& domain) const;
  const DomainDeclaration* find_domain(const DomainId& id) const;
  const ContainmentEdge* find_containment(const NodeId& parent, const NodeId& child) const;
  const AdjacencyEdge* find_adjacency(const NodeId& a, const NodeId& b) const;
  const DomainAssociation* find_association(const NodeId& node, const DomainId& domain) const;

  // -- bounded structural walks ---------------------------------------------

  /// Physical parent chain, from the topmost ancestor down to (excluding) the
  /// node. Walks at most kMaxAncestryWalk steps.
  std::vector<NodeId> ancestry(const NodeId& id) const;

  /// Facility root that contains the node, or the node itself when it is a
  /// facility. nullopt when the node is not connected to a facility root.
  std::optional<NodeId> facility_of(const NodeId& id) const;

  /// Number of Physical containment steps from the facility root.
  std::optional<std::uint32_t> depth_of(const NodeId& id) const;

  /// True when `ancestor` is on the Physical path above `node`.
  bool is_descendant_of(const NodeId& node, const NodeId& ancestor) const;

  /// Deterministic traversal of the Physical containment tree.
  Result<std::vector<NodeId>> traverse(const NodeId& root, TraversalOrder order, TraversalLimits request) const;

  /// Breadth-first descendants in canonical sibling order (root excluded).
  std::vector<NodeId> descendants(const NodeId& id) const;

  /// Aggregate counts.
  TopologyStats stats() const;

  /// Structural (content) digest: nodes, containment, adjacency, domains and
  /// associations, excluding lineage, provenance values and timestamps.
  Digest content_digest() const;

  /// True when both graphs describe the same structure.
  bool same_structure_as(const GraphData& other) const;
};

/// Orders containment edges canonically: by parent, then child.
inline bool containment_less(const ContainmentEdge& lhs, const ContainmentEdge& rhs) noexcept {
  return ContainmentKey{lhs.parent, lhs.child} < ContainmentKey{rhs.parent, rhs.child};
}

/// Orders adjacency edges canonically: by first endpoint, then second.
inline bool adjacency_less(const AdjacencyEdge& lhs, const AdjacencyEdge& rhs) noexcept {
  return AdjacencyKey{lhs.first, lhs.second} < AdjacencyKey{rhs.first, rhs.second};
}

/// Orders domain associations canonically: by node, then domain.
inline bool association_less(const DomainAssociation& lhs, const DomainAssociation& rhs) noexcept {
  return AssociationKey{lhs.node, lhs.domain} < AssociationKey{rhs.node, rhs.domain};
}

/// Collects the sorted canonical-order views used by the public API.
std::vector<NodeRecord> collect_nodes(const GraphData& data);
std::vector<NodeId> collect_node_ids(const GraphData& data);
std::vector<ContainmentEdge> collect_containment(const GraphData& data);
std::vector<AdjacencyEdge> collect_adjacency(const GraphData& data);
std::vector<DomainDeclaration> collect_domains(const GraphData& data);
std::vector<DomainAssociation> collect_associations(const GraphData& data);

/// True when a boundary kind is the one implied by the parent's node kind.
bool boundary_matches_parent(NodeKind parent_kind, BoundaryKind boundary) noexcept;

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_SRC_GRAPH_DATA_HPP
