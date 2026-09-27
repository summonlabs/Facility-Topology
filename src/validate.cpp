// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "dccp/facility_topology/topology.hpp"
#include "graph_data.hpp"

namespace dccp::facility_topology {
namespace {

class IssueCollector {
 public:
  void add(ErrorCode code, std::string subject, std::string explanation) {
    if (issues_.size() >= kMaxIssues) {
      return;
    }
    ValidationIssue issue;
    issue.code = code;
    issue.subject = std::move(subject);
    issue.explanation = std::move(explanation);
    issues_.push_back(std::move(issue));
  }

  void add_node_issue(ErrorCode code, const NodeId& subject, std::string explanation) {
    add(code, subject.str(), std::move(explanation));
  }

  std::vector<ValidationIssue> take() { return std::move(issues_); }

 private:
  // Bounded so that validating adversarial input cannot itself exhaust memory.
  static constexpr std::size_t kMaxIssues = 1024;

  std::vector<ValidationIssue> issues_;
};

std::string quoted_kind(NodeKind kind) { return std::string(node_kind_token(kind)); }

}  // namespace

ValidationReport validate_graph(const GraphData& data, const TopologyLimits& limits) {
  IssueCollector issues;

  std::string limits_explanation;
  if (!validate_limits(limits, limits_explanation)) {
    issues.add(ErrorCode::InvalidArgument, "limits", "invalid topology limits: " + limits_explanation);
  }

  // -- budgets --------------------------------------------------------------
  if (data.nodes.size() > limits.max_nodes) {
    issues.add(ErrorCode::LimitExceeded, "nodes", "node count exceeds the configured maximum");
  }
  if (data.containment.size() > limits.max_containment_edges) {
    issues.add(ErrorCode::LimitExceeded, "containment", "containment count exceeds the configured maximum");
  }
  if (data.adjacency.size() > limits.max_adjacency_edges) {
    issues.add(ErrorCode::LimitExceeded, "adjacency", "adjacency count exceeds the configured maximum");
  }
  if (data.domains.size() > limits.max_domain_declarations) {
    issues.add(ErrorCode::LimitExceeded, "domains", "domain declaration count exceeds the configured maximum");
  }
  if (data.associations.size() > limits.max_domain_associations) {
    issues.add(ErrorCode::LimitExceeded, "associations", "domain association count exceeds the configured maximum");
  }

  // -- nodes ----------------------------------------------------------------
  for (const auto& entry : data.nodes) {
    const NodeId& id = entry.first;
    const NodeRecord& node = entry.second;
    if (id.empty()) {
      issues.add(ErrorCode::MissingField, "<empty>", "node identity is empty");
      continue;
    }
    if (node.id != id) {
      issues.add_node_issue(ErrorCode::InternalError, id, "node record identity does not match its index key");
    }
    if (node.kind == NodeKind::Zone) {
      if (!node.zone_member_kind.has_value()) {
        issues.add_node_issue(ErrorCode::MissingField, id, "zone node does not declare the kind it groups");
      } else if (!node_kind_schema(*node.zone_member_kind).zonable) {
        issues.add_node_issue(ErrorCode::ZoneTargetKindInvalid, id,
                              "zone declares a non-zonable member kind " + quoted_kind(*node.zone_member_kind));
      }
    } else if (node.zone_member_kind.has_value()) {
      issues.add_node_issue(ErrorCode::InvalidArgument, id, "non-zone node declares a zone member kind");
    }
    if (!text::is_valid_utf8(node.label) || text::contains_control_characters(node.label)) {
      issues.add_node_issue(ErrorCode::MalformedRecord, id, "node label is not a valid canonical label");
    }
    if (node.label.size() > limits.max_label_bytes) {
      issues.add_node_issue(ErrorCode::TextTooLong, id, "node label exceeds the configured maximum length");
    }
    if (node.provenance.actor.empty() || node.provenance.source.empty()) {
      issues.add_node_issue(ErrorCode::MissingField, id, "node provenance is incomplete");
    }
    if (!node.provenance.recorded_at.empty() && !is_canonical_utc_timestamp(node.provenance.recorded_at)) {
      issues.add_node_issue(ErrorCode::MalformedRecord, id, "node provenance timestamp is not canonical UTC");
    }
  }

  // -- derived node indices -------------------------------------------------
  for (const auto& entry : data.kind_index) {
    for (const NodeId& id : entry.second) {
      const NodeRecord* record = data.find_node(id);
      if (record == nullptr) {
        issues.add_node_issue(ErrorCode::InternalError, id, "kind index references an unknown node");
      } else if (record->kind != entry.first) {
        issues.add_node_issue(ErrorCode::InternalError, id, "kind index disagrees with the node record");
      }
    }
  }
  for (const auto& entry : data.nodes) {
    const auto index_entry = data.kind_index.find(entry.second.kind);
    if (index_entry == data.kind_index.end() || index_entry->second.find(entry.first) == index_entry->second.end()) {
      issues.add_node_issue(ErrorCode::InternalError, entry.first, "node is missing from the kind index");
    }
  }

  // -- containment ----------------------------------------------------------
  for (const auto& entry : data.containment) {
    const ContainmentEdge& edge = entry.second;
    const NodeRecord* parent = data.find_node(edge.parent);
    const NodeRecord* child = data.find_node(edge.child);
    if (parent == nullptr) {
      issues.add(ErrorCode::MissingEndpoint, edge.parent.str(), "containment parent does not exist");
      continue;
    }
    if (child == nullptr) {
      issues.add(ErrorCode::MissingEndpoint, edge.child.str(), "containment child does not exist");
      continue;
    }
    if (edge.parent == edge.child) {
      issues.add_node_issue(ErrorCode::SelfEdge, edge.child, "containment edge is a self edge");
      continue;
    }
    if (!boundary_matches_parent(parent->kind, edge.boundary)) {
      issues.add_node_issue(ErrorCode::InvalidParentKind, edge.child,
                            std::string("containment boundary \"") + std::string(boundary_kind_token(edge.boundary)) +
                                "\" is not valid for a " + quoted_kind(parent->kind) + " parent");
      continue;
    }
    if (edge.boundary == BoundaryKind::Physical) {
      if (child->kind == NodeKind::Facility) {
        issues.add_node_issue(ErrorCode::FacilityMustBeRoot, edge.child, "a facility must be a physical root");
      } else if (node_kind_schema(child->kind).root_eligible) {
        issues.add_node_issue(ErrorCode::InvalidParentKind, edge.child,
                              quoted_kind(child->kind) + " is root eligible and cannot be contained");
      } else if (!schema_allows_physical_child(parent->kind, child->kind)) {
        issues.add_node_issue(ErrorCode::InvalidParentKind, edge.child,
                              "a " + quoted_kind(parent->kind) + " may not contain a " + quoted_kind(child->kind));
      }
      const auto indexed = data.physical_parent.find(edge.child);
      if (indexed == data.physical_parent.end() || indexed->second != edge.parent) {
        issues.add_node_issue(ErrorCode::InternalError, edge.child, "physical parent index disagrees with the edges");
      }
    } else {
      if (child->kind == NodeKind::Facility || child->kind == NodeKind::Zone ||
          !node_kind_schema(child->kind).zonable) {
        issues.add_node_issue(ErrorCode::ZoneMemberKindInvalid, edge.child,
                              quoted_kind(child->kind) + " cannot be a zone member");
      } else if (parent->zone_member_kind.has_value() && *parent->zone_member_kind != child->kind) {
        issues.add_node_issue(ErrorCode::ZoneTargetKindInvalid, edge.child,
                              "zone groups " + quoted_kind(*parent->zone_member_kind) + " nodes but this member is a " +
                                  quoted_kind(child->kind));
      }
      const std::optional<NodeId> zone_facility = data.facility_of(edge.parent);
      const std::optional<NodeId> member_facility = data.facility_of(edge.child);
      if (!zone_facility.has_value() || !member_facility.has_value() || *zone_facility != *member_facility) {
        issues.add_node_issue(ErrorCode::ZoneConflict, edge.child, "zone member is not in the zone's facility");
      }
      const auto indexed = data.logical_parent.find(edge.child);
      if (indexed == data.logical_parent.end() || indexed->second != edge.parent) {
        issues.add_node_issue(ErrorCode::InternalError, edge.child, "zone membership index disagrees with the edges");
      }
    }
  }

  for (const auto& entry : data.physical_parent) {
    if (data.containment.find(ContainmentKey{entry.second, entry.first}) == data.containment.end()) {
      issues.add_node_issue(ErrorCode::InternalError, entry.first, "physical parent index has no matching edge");
    }
  }
  for (const auto& entry : data.logical_parent) {
    if (data.containment.find(ContainmentKey{entry.second, entry.first}) == data.containment.end()) {
      issues.add_node_issue(ErrorCode::InternalError, entry.first, "zone membership index has no matching edge");
    }
  }
  for (const auto& entry : data.children) {
    for (const NodeId& child : entry.second) {
      const ContainmentEdge* edge = data.find_containment(entry.first, child);
      if (edge == nullptr || edge->boundary != BoundaryKind::Physical) {
        issues.add_node_issue(ErrorCode::InternalError, child, "child index disagrees with the containment edges");
      }
    }
  }
  for (const auto& entry : data.members) {
    for (const NodeId& member : entry.second) {
      const ContainmentEdge* edge = data.find_containment(entry.first, member);
      if (edge == nullptr || edge->boundary != BoundaryKind::Logical) {
        issues.add_node_issue(ErrorCode::InternalError, member, "member index disagrees with the containment edges");
      }
    }
  }

  // -- roots and containment closure ---------------------------------------
  // Each node's own containment requirement is reported before the
  // generation-level "no facility root" finding so that an orphan subtree is
  // attributed to the node that is actually orphaned.
  for (const auto& entry : data.nodes) {
    const NodeRecord& node = entry.second;
    const bool has_physical_parent = data.physical_parent.find(entry.first) != data.physical_parent.end();
    if (node.kind == NodeKind::Facility && has_physical_parent) {
      issues.add_node_issue(ErrorCode::FacilityMustBeRoot, entry.first, "a facility must not have a physical parent");
    }
    if (node.kind != NodeKind::Facility && !node_kind_schema(node.kind).root_eligible && !has_physical_parent) {
      issues.add_node_issue(ErrorCode::NonFacilityMustBeContained, entry.first,
                            "every node except a facility root must have exactly one physical parent");
    }
  }

  const std::set<NodeId>& facilities = data.nodes_of_kind(NodeKind::Facility);
  if (facilities.empty()) {
    issues.add(ErrorCode::MissingField, "facility", "a generation must contain at least one facility root");
  }

  // Physical reachability: a node that is not reachable from a facility root
  // is an orphan subtree, which is exactly what orphan prevention forbids.
  std::set<NodeId> reached;
  {
    std::vector<NodeId> stack(facilities.begin(), facilities.end());
    reached.insert(facilities.begin(), facilities.end());
    while (!stack.empty()) {
      const NodeId current = stack.back();
      stack.pop_back();
      for (const NodeId& child : data.children_of(current)) {
        if (reached.insert(child).second) {
          stack.push_back(child);
        }
      }
    }
    for (const auto& entry : data.nodes) {
      if (reached.find(entry.first) == reached.end() && entry.second.kind != NodeKind::Facility) {
        issues.add_node_issue(ErrorCode::NonFacilityMustBeContained, entry.first,
                              "node is not reachable from any facility root");
      }
    }
  }

  // Every reachable node must sit within the schema's structural depth. This
  // is unreachable for data built through the builder, which is exactly why it
  // is checked: validation is the backstop for storage that was constructed
  // some other way.
  for (const auto& entry : data.nodes) {
    if (reached.find(entry.first) == reached.end()) {
      continue;
    }
    const std::optional<std::uint32_t> depth = data.depth_of(entry.first);
    if (!depth.has_value() || *depth > kMaxStructuralDepth) {
      issues.add_node_issue(ErrorCode::InvalidParentKind, entry.first,
                            "node sits deeper than the maximum structural depth");
    }
  }

  // -- cycle detection ------------------------------------------------------
  // Iterative depth-first search with colouring over every containment edge,
  // physical and logical, built directly from the edge set so the check does
  // not depend on the derived indices. Iteration is explicit, so an
  // adversarial hierarchy cannot exhaust the native stack.
  {
    enum class Colour : std::uint8_t { White, Grey, Black };
    std::map<NodeId, std::vector<NodeId>> outgoing;
    for (const auto& entry : data.containment) {
      outgoing[entry.second.parent].push_back(entry.second.child);
    }

    std::map<NodeId, Colour> colour;
    for (const auto& entry : data.nodes) {
      colour.emplace(entry.first, Colour::White);
    }

    struct Frame {
      NodeId id;
      std::size_t next = 0;
    };
    std::vector<Frame> stack;
    for (const auto& entry : data.nodes) {
      if (colour[entry.first] != Colour::White) {
        continue;
      }
      colour[entry.first] = Colour::Grey;
      stack.push_back(Frame{entry.first, 0});
      while (!stack.empty()) {
        Frame& frame = stack.back();
        const auto targets = outgoing.find(frame.id);
        const std::size_t target_count = (targets == outgoing.end()) ? 0 : targets->second.size();
        if (frame.next >= target_count) {
          colour[frame.id] = Colour::Black;
          stack.pop_back();
          continue;
        }
        const NodeId target = targets->second[frame.next];
        ++frame.next;
        const Colour target_colour = colour[target];
        if (target_colour == Colour::Grey) {
          issues.add_node_issue(ErrorCode::ContainmentCycle, target, "containment cycle detected");
          continue;
        }
        if (target_colour == Colour::White) {
          colour[target] = Colour::Grey;
          stack.push_back(Frame{target, 0});
        }
      }
    }
  }

  // -- adjacency ------------------------------------------------------------
  for (const auto& entry : data.adjacency) {
    const AdjacencyEdge& edge = entry.second;
    if (data.find_node(edge.first) == nullptr) {
      issues.add(ErrorCode::MissingEndpoint, edge.first.str(), "adjacency endpoint does not exist");
      continue;
    }
    if (data.find_node(edge.second) == nullptr) {
      issues.add(ErrorCode::MissingEndpoint, edge.second.str(), "adjacency endpoint does not exist");
      continue;
    }
    if (edge.first == edge.second) {
      issues.add_node_issue(ErrorCode::SelfEdge, edge.first, "adjacency edge is a self edge");
      continue;
    }
    if (!(edge.first < edge.second)) {
      issues.add_node_issue(ErrorCode::MalformedRecord, edge.first,
                            "adjacency endpoints are not stored in canonical order");
    }
    const NodeKind first_kind = data.kind_of(edge.first).value();
    const NodeKind second_kind = data.kind_of(edge.second).value();
    if (!schema_allows_adjacency(first_kind, second_kind)) {
      issues.add_node_issue(ErrorCode::InvalidArgument, edge.first,
                            "physical adjacency requires two physical non-facility nodes");
    }
    const std::optional<NodeId> first_facility = data.facility_of(edge.first);
    const std::optional<NodeId> second_facility = data.facility_of(edge.second);
    if (first_facility.has_value() && second_facility.has_value() && *first_facility != *second_facility) {
      issues.add_node_issue(ErrorCode::AdjacencyCrossFacility, edge.first,
                            "adjacency endpoints belong to different facilities");
    }
    if (data.neighbors_of(edge.first).find(edge.second) == data.neighbors_of(edge.first).end() ||
        data.neighbors_of(edge.second).find(edge.first) == data.neighbors_of(edge.second).end()) {
      issues.add_node_issue(ErrorCode::InternalError, edge.first, "adjacency index disagrees with the edges");
    }
  }
  for (const auto& entry : data.neighbor_index) {
    for (const NodeId& neighbor : entry.second) {
      if (data.find_adjacency(entry.first, neighbor) == nullptr) {
        issues.add_node_issue(ErrorCode::InternalError, entry.first, "adjacency index has no matching edge");
      }
    }
  }

  // -- domains --------------------------------------------------------------
  for (const auto& entry : data.domains) {
    const DomainDeclaration& declaration = entry.second;
    if (declaration.id.empty()) {
      issues.add(ErrorCode::MissingField, "<empty>", "domain declaration identity is empty");
      continue;
    }
    if (declaration.id != entry.first) {
      issues.add(ErrorCode::InternalError, declaration.id.str(), "domain identity does not match its index key");
    }
    if (declaration.provenance.actor.empty() || declaration.provenance.source.empty()) {
      issues.add(ErrorCode::MissingField, declaration.id.str(), "domain provenance is incomplete");
    }
    if (!text::is_valid_utf8(declaration.label) || text::contains_control_characters(declaration.label)) {
      issues.add(ErrorCode::MalformedRecord, declaration.id.str(), "domain label is not a valid canonical label");
    }
  }

  for (const auto& entry : data.associations) {
    const DomainAssociation& association = entry.second;
    if (data.find_node(association.node) == nullptr) {
      issues.add(ErrorCode::MissingEndpoint, association.node.str(), "domain association node does not exist");
      continue;
    }
    const DomainDeclaration* declaration = data.find_domain(association.domain);
    if (declaration == nullptr) {
      issues.add(ErrorCode::MissingEndpoint, association.domain.str(), "domain association references an undeclared domain");
      continue;
    }
    if (declaration->kind != association.kind) {
      issues.add(ErrorCode::IdentityConflict, association.domain.str(),
                 "domain association kind disagrees with the declaration");
    }
    const auto node_refs = data.node_associations.find(association.node);
    if (node_refs == data.node_associations.end() ||
        node_refs->second.find(AssociationKey{association.node, association.domain}) == node_refs->second.end()) {
      issues.add_node_issue(ErrorCode::InternalError, association.node, "domain association index is incomplete");
    }
    const auto domain_refs = data.domain_members.find(association.domain);
    if (domain_refs == data.domain_members.end() ||
        domain_refs->second.find(association.node) == domain_refs->second.end()) {
      issues.add_node_issue(ErrorCode::InternalError, association.node, "domain membership index is incomplete");
    }
  }
  for (const auto& entry : data.node_associations) {
    for (const AssociationKey& key : entry.second) {
      if (data.associations.find(key) == data.associations.end()) {
        issues.add_node_issue(ErrorCode::InternalError, entry.first, "domain association index has no matching record");
      }
    }
  }
  for (const auto& entry : data.domain_members) {
    for (const NodeId& node : entry.second) {
      if (data.associations.find(AssociationKey{node, entry.first}) == data.associations.end()) {
        issues.add_node_issue(ErrorCode::InternalError, node, "domain membership index has no matching record");
      }
    }
  }

  ValidationReport report;
  report.issues = issues.take();
  report.valid = report.issues.empty();
  return report;
}

}  // namespace dccp::facility_topology
