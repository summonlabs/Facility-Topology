// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "graph_data.hpp"

#include <algorithm>
#include <deque>
#include <utility>

#include "canonical_internal.hpp"

namespace dccp::facility_topology {
namespace {

const std::set<NodeId> kNoNodes;
const std::set<AssociationKey> kNoAssociations;

}  // namespace

AdjacencyKey make_adjacency_key(const NodeId& a, const NodeId& b) noexcept {
  return (b < a) ? AdjacencyKey{b, a} : AdjacencyKey{a, b};
}

bool boundary_matches_parent(NodeKind parent_kind, BoundaryKind boundary) noexcept {
  const bool zone_parent = (parent_kind == NodeKind::Zone);
  return zone_parent ? (boundary == BoundaryKind::Logical) : (boundary == BoundaryKind::Physical);
}

const NodeRecord* GraphData::find_node(const NodeId& id) const {
  const auto it = nodes.find(id);
  return (it == nodes.end()) ? nullptr : &it->second;
}

bool GraphData::has_node(const NodeId& id) const { return nodes.find(id) != nodes.end(); }

std::optional<NodeKind> GraphData::kind_of(const NodeId& id) const {
  const NodeRecord* record = find_node(id);
  if (record == nullptr) {
    return std::nullopt;
  }
  return record->kind;
}

const std::set<NodeId>& GraphData::children_of(const NodeId& id) const {
  const auto it = children.find(id);
  return (it == children.end()) ? kNoNodes : it->second;
}

const std::set<NodeId>& GraphData::members_of(const NodeId& id) const {
  const auto it = members.find(id);
  return (it == members.end()) ? kNoNodes : it->second;
}

const std::set<NodeId>& GraphData::neighbors_of(const NodeId& id) const {
  const auto it = neighbor_index.find(id);
  return (it == neighbor_index.end()) ? kNoNodes : it->second;
}

const std::set<NodeId>& GraphData::nodes_of_kind(NodeKind kind) const {
  const auto it = kind_index.find(kind);
  return (it == kind_index.end()) ? kNoNodes : it->second;
}

const std::set<NodeId>& GraphData::nodes_in_domain(const DomainId& domain) const {
  const auto it = domain_members.find(domain);
  return (it == domain_members.end()) ? kNoNodes : it->second;
}

const DomainDeclaration* GraphData::find_domain(const DomainId& id) const {
  const auto it = domains.find(id);
  return (it == domains.end()) ? nullptr : &it->second;
}

const ContainmentEdge* GraphData::find_containment(const NodeId& parent, const NodeId& child) const {
  const auto it = containment.find(ContainmentKey{parent, child});
  return (it == containment.end()) ? nullptr : &it->second;
}

const AdjacencyEdge* GraphData::find_adjacency(const NodeId& a, const NodeId& b) const {
  const auto it = adjacency.find(make_adjacency_key(a, b));
  return (it == adjacency.end()) ? nullptr : &it->second;
}

const DomainAssociation* GraphData::find_association(const NodeId& node, const DomainId& domain) const {
  const auto it = associations.find(AssociationKey{node, domain});
  return (it == associations.end()) ? nullptr : &it->second;
}

std::vector<NodeId> GraphData::ancestry(const NodeId& id) const {
  std::vector<NodeId> chain;
  NodeId cursor = id;
  for (std::size_t step = 0; step < kMaxAncestryWalk; ++step) {
    const auto it = physical_parent.find(cursor);
    if (it == physical_parent.end()) {
      break;
    }
    chain.push_back(it->second);
    cursor = it->second;
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

std::optional<NodeId> GraphData::facility_of(const NodeId& id) const {
  if (!has_node(id)) {
    return std::nullopt;
  }
  const auto parent = physical_parent.find(id);
  if (parent == physical_parent.end()) {
    const NodeRecord* record = find_node(id);
    if (record != nullptr && record->kind == NodeKind::Facility) {
      return id;
    }
    return std::nullopt;
  }
  const std::vector<NodeId> chain = ancestry(id);
  if (chain.empty() || chain.size() >= kMaxAncestryWalk) {
    // No root was reached within the bound: the storage is not a validated
    // generation, so no facility can be attributed to this node.
    return std::nullopt;
  }
  const NodeRecord* root = find_node(chain.front());
  if (root == nullptr || root->kind != NodeKind::Facility) {
    return std::nullopt;
  }
  return chain.front();
}

std::optional<std::uint32_t> GraphData::depth_of(const NodeId& id) const {
  if (!has_node(id)) {
    return std::nullopt;
  }
  const std::vector<NodeId> chain = ancestry(id);
  if (chain.size() >= kMaxAncestryWalk) {
    // A valid topology can never reach this depth: the data is a candidate
    // that has not been validated, or it contains a cycle.
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(chain.size());
}

bool GraphData::is_descendant_of(const NodeId& node, const NodeId& ancestor) const {
  if (node == ancestor) {
    return false;
  }
  NodeId cursor = node;
  for (std::size_t step = 0; step < kMaxAncestryWalk; ++step) {
    const auto it = physical_parent.find(cursor);
    if (it == physical_parent.end()) {
      return false;
    }
    if (it->second == ancestor) {
      return true;
    }
    cursor = it->second;
  }
  return false;
}

Result<std::vector<NodeId>> GraphData::traverse(const NodeId& root, TraversalOrder order,
                                                TraversalLimits request) const {
  if (!has_node(root)) {
    return Error(ErrorCode::NotFound, "traversal root does not exist in this generation").with_subject(root.str());
  }
  const std::size_t depth_ceiling = std::min(request.max_depth, limits.max_traversal_depth);
  const std::size_t node_ceiling = std::min(request.max_nodes, limits.max_traversal_nodes);

  struct Frame {
    NodeId id;
    std::size_t depth;
  };

  std::vector<NodeId> out;
  if (order == TraversalOrder::BreadthFirst) {
    std::deque<Frame> queue;
    for (const NodeId& child : children_of(root)) {
      queue.push_back(Frame{child, 1});
    }
    while (!queue.empty()) {
      const Frame frame = queue.front();
      queue.pop_front();
      if (frame.depth > depth_ceiling) {
        return Error(ErrorCode::TraversalDepthExceeded, "traversal exceeded the requested depth bound")
            .with_subject(root.str());
      }
      if (out.size() >= node_ceiling) {
        return Error(ErrorCode::LimitExceeded, "traversal exceeded the requested node bound").with_subject(root.str());
      }
      out.push_back(frame.id);
      for (const NodeId& child : children_of(frame.id)) {
        if (out.size() + queue.size() >= node_ceiling) {
          return Error(ErrorCode::LimitExceeded, "traversal exceeded the requested node bound")
              .with_subject(root.str());
        }
        queue.push_back(Frame{child, frame.depth + 1});
      }
    }
    return out;
  }

  // Depth-first pre-order with an explicit stack: no recursion, so the walk
  // cannot exhaust the native stack on an adversarial hierarchy.
  std::vector<Frame> stack;
  {
    const std::set<NodeId>& root_children = children_of(root);
    for (auto it = root_children.rbegin(); it != root_children.rend(); ++it) {
      stack.push_back(Frame{*it, 1});
    }
  }
  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    if (frame.depth > depth_ceiling) {
      return Error(ErrorCode::TraversalDepthExceeded, "traversal exceeded the requested depth bound")
          .with_subject(root.str());
    }
    if (out.size() >= node_ceiling) {
      return Error(ErrorCode::LimitExceeded, "traversal exceeded the requested node bound").with_subject(root.str());
    }
    out.push_back(frame.id);
    const std::set<NodeId>& node_children = children_of(frame.id);
    if (out.size() + stack.size() + node_children.size() >= node_ceiling) {
      return Error(ErrorCode::LimitExceeded, "traversal exceeded the requested node bound").with_subject(root.str());
    }
    for (auto it = node_children.rbegin(); it != node_children.rend(); ++it) {
      stack.push_back(Frame{*it, frame.depth + 1});
    }
  }
  return out;
}

TopologyStats GraphData::stats() const {
  TopologyStats out;
  out.nodes = nodes.size();
  out.adjacency = adjacency.size();
  out.domains = domains.size();
  out.associations = associations.size();
  for (const auto& entry : containment) {
    if (entry.second.boundary == BoundaryKind::Physical) {
      ++out.physical_containment;
    } else {
      ++out.logical_containment;
    }
  }
  out.facilities = nodes_of_kind(NodeKind::Facility).size();

  // Single breadth-first pass from every facility root gives exact depths and
  // leaves any unconnected node at depth 0.
  std::map<NodeId, std::uint32_t> depth;
  std::deque<NodeId> queue;
  for (const NodeId& facility : nodes_of_kind(NodeKind::Facility)) {
    depth.emplace(facility, 0);
    queue.push_back(facility);
  }
  while (!queue.empty()) {
    const NodeId current = queue.front();
    queue.pop_front();
    const std::uint32_t next_depth = depth[current] + 1U;
    for (const NodeId& child : children_of(current)) {
      if (depth.emplace(child, next_depth).second) {
        queue.push_back(child);
      }
    }
  }
  for (const auto& entry : depth) {
    if (entry.second > out.max_depth) {
      out.max_depth = entry.second;
    }
  }
  return out;
}

std::vector<NodeId> GraphData::descendants(const NodeId& id) const {
  std::vector<NodeId> out;
  std::deque<NodeId> queue;
  for (const NodeId& child : children_of(id)) {
    queue.push_back(child);
  }
  while (!queue.empty()) {
    if (out.size() >= limits.max_traversal_nodes) {
      break;
    }
    const NodeId current = queue.front();
    queue.pop_front();
    out.push_back(current);
    for (const NodeId& child : children_of(current)) {
      queue.push_back(child);
    }
  }
  return out;
}

Digest GraphData::content_digest() const { return structure_digest(*this); }

bool GraphData::same_structure_as(const GraphData& other) const {
  return digest_equal(content_digest(), other.content_digest());
}

std::vector<NodeRecord> collect_nodes(const GraphData& data) {
  std::vector<NodeRecord> out;
  out.reserve(data.nodes.size());
  for (const auto& entry : data.nodes) {
    out.push_back(entry.second);
  }
  return out;
}

std::vector<NodeId> collect_node_ids(const GraphData& data) {
  std::vector<NodeId> out;
  out.reserve(data.nodes.size());
  for (const auto& entry : data.nodes) {
    out.push_back(entry.first);
  }
  return out;
}

std::vector<ContainmentEdge> collect_containment(const GraphData& data) {
  std::vector<ContainmentEdge> out;
  out.reserve(data.containment.size());
  for (const auto& entry : data.containment) {
    out.push_back(entry.second);
  }
  return out;
}

std::vector<AdjacencyEdge> collect_adjacency(const GraphData& data) {
  std::vector<AdjacencyEdge> out;
  out.reserve(data.adjacency.size());
  for (const auto& entry : data.adjacency) {
    out.push_back(entry.second);
  }
  return out;
}

std::vector<DomainDeclaration> collect_domains(const GraphData& data) {
  std::vector<DomainDeclaration> out;
  out.reserve(data.domains.size());
  for (const auto& entry : data.domains) {
    out.push_back(entry.second);
  }
  return out;
}

std::vector<DomainAssociation> collect_associations(const GraphData& data) {
  std::vector<DomainAssociation> out;
  out.reserve(data.associations.size());
  for (const auto& entry : data.associations) {
    out.push_back(entry.second);
  }
  return out;
}

}  // namespace dccp::facility_topology
