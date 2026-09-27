// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/topology.hpp"

#include <algorithm>
#include <utility>

#include "graph_data.hpp"

namespace dccp::facility_topology {
namespace {

const TopologyLimits kFallbackLimits{};
const TopologyGeneration kNoGeneration{};

Result<void> require_identity(const NodeId& id, std::string_view what) {
  if (id.empty()) {
    return Error(ErrorCode::MissingField, std::string(what) + " identity is empty");
  }
  return ok();
}

Result<void> require_node(const GraphData& data, const NodeId& id, std::string_view what) {
  if (id.empty()) {
    return Error(ErrorCode::MissingField, std::string(what) + " identity is empty");
  }
  if (!data.has_node(id)) {
    return Error(ErrorCode::NotFound, std::string(what) + " does not exist in this generation").with_subject(id.str());
  }
  return ok();
}

Result<void> check_node_budget(const GraphData& data) {
  if (data.nodes.size() >= data.limits.max_nodes) {
    return Error(ErrorCode::LimitExceeded, "node budget exhausted").with_subject(std::to_string(data.limits.max_nodes));
  }
  return ok();
}

std::string container_kind_explanation(NodeKind parent, NodeKind child) {
  return "a " + std::string(node_kind_token(parent)) + " may not contain a " + std::string(node_kind_token(child));
}

}  // namespace

std::string_view traversal_order_token(TraversalOrder order) noexcept {
  switch (order) {
    case TraversalOrder::BreadthFirst:
      return "bfs";
    case TraversalOrder::DepthFirstPreOrder:
      return "dfs";
  }
  return "unknown";
}

Result<TraversalOrder> traversal_order_parse(std::string_view token) {
  if (token == "bfs") {
    return TraversalOrder::BreadthFirst;
  }
  if (token == "dfs") {
    return TraversalOrder::DepthFirstPreOrder;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown traversal order token; expected \"bfs\" or \"dfs\"")
      .with_subject(std::string(token.substr(0, 32)));
}

std::string TopologyStats::to_string() const {
  return "nodes=" + std::to_string(nodes) + " facilities=" + std::to_string(facilities) +
         " containment=" + std::to_string(physical_containment + logical_containment) +
         " physical=" + std::to_string(physical_containment) + " logical=" + std::to_string(logical_containment) +
         " adjacency=" + std::to_string(adjacency) + " domains=" + std::to_string(domains) +
         " associations=" + std::to_string(associations) + " max-depth=" + std::to_string(max_depth);
}

std::size_t ValidationReport::count(ErrorCode code) const {
  std::size_t total = 0;
  for (const ValidationIssue& issue : issues) {
    if (issue.code == code) {
      ++total;
    }
  }
  return total;
}

std::string ValidationReport::to_string() const {
  std::string out = valid ? "valid" : "invalid";
  out.append(" issues=");
  out.append(std::to_string(issues.size()));
  for (const ValidationIssue& issue : issues) {
    out.push_back('\n');
    out.append("  ");
    out.append(error_code_name(issue.code));
    out.push_back(' ');
    out.append(issue.explanation);
    if (!issue.subject.empty()) {
      out.append(" [subject=");
      out.append(issue.subject);
      out.push_back(']');
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// TopologySnapshot
// ---------------------------------------------------------------------------

TopologySnapshot::TopologySnapshot() noexcept = default;
TopologySnapshot::TopologySnapshot(const TopologySnapshot&) noexcept = default;
TopologySnapshot::TopologySnapshot(TopologySnapshot&&) noexcept = default;
TopologySnapshot& TopologySnapshot::operator=(const TopologySnapshot&) noexcept = default;
TopologySnapshot& TopologySnapshot::operator=(TopologySnapshot&&) noexcept = default;
TopologySnapshot::~TopologySnapshot() = default;

TopologySnapshot::TopologySnapshot(std::shared_ptr<const GraphData> data) noexcept : data_(std::move(data)) {}

Result<TopologySnapshot> snapshot_from_data(std::shared_ptr<const GraphData> data) {
  if (data == nullptr) {
    return Error(ErrorCode::InternalError, "snapshot construction requires topology storage");
  }
  return TopologySnapshot(std::move(data));
}

const TopologyGeneration& TopologySnapshot::generation() const noexcept {
  return (data_ == nullptr) ? kNoGeneration : data_->generation;
}

bool TopologySnapshot::valid() const noexcept { return data_ != nullptr; }

const TopologyLimits& TopologySnapshot::limits() const noexcept {
  return (data_ == nullptr) ? kFallbackLimits : data_->limits;
}

TopologyStats TopologySnapshot::stats() const {
  return (data_ == nullptr) ? TopologyStats{} : data_->stats();
}

Digest TopologySnapshot::content_digest() const {
  return (data_ == nullptr) ? Digest{} : data_->content_digest();
}

bool TopologySnapshot::same_structure_as(const TopologySnapshot& other) const {
  if (data_ == nullptr || other.data_ == nullptr) {
    return data_ == other.data_;
  }
  if (data_.get() == other.data_.get()) {
    return true;
  }
  return data_->same_structure_as(*other.data_);
}

const NodeRecord* TopologySnapshot::find_node(const NodeId& id) const {
  return (data_ == nullptr) ? nullptr : data_->find_node(id);
}

bool TopologySnapshot::contains_node(const NodeId& id) const {
  return (data_ != nullptr) && data_->has_node(id);
}

std::optional<NodeKind> TopologySnapshot::node_kind(const NodeId& id) const {
  return (data_ == nullptr) ? std::nullopt : data_->kind_of(id);
}

std::optional<NodeId> TopologySnapshot::physical_parent(const NodeId& id) const {
  if (data_ == nullptr) {
    return std::nullopt;
  }
  const auto it = data_->physical_parent.find(id);
  return (it == data_->physical_parent.end()) ? std::nullopt : std::optional<NodeId>(it->second);
}

std::optional<NodeId> TopologySnapshot::zone_of(const NodeId& id) const {
  if (data_ == nullptr) {
    return std::nullopt;
  }
  const auto it = data_->logical_parent.find(id);
  return (it == data_->logical_parent.end()) ? std::nullopt : std::optional<NodeId>(it->second);
}

std::vector<NodeId> TopologySnapshot::children(const NodeId& id) const {
  if (data_ == nullptr) {
    return {};
  }
  const std::set<NodeId>& set = data_->children_of(id);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologySnapshot::zone_members(const NodeId& zone) const {
  if (data_ == nullptr) {
    return {};
  }
  const std::set<NodeId>& set = data_->members_of(zone);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologySnapshot::ancestry(const NodeId& id) const {
  return (data_ == nullptr) ? std::vector<NodeId>{} : data_->ancestry(id);
}

std::vector<NodeId> TopologySnapshot::descendants(const NodeId& id) const {
  return (data_ == nullptr) ? std::vector<NodeId>{} : data_->descendants(id);
}

std::vector<NodeId> TopologySnapshot::neighbors(const NodeId& id) const {
  if (data_ == nullptr) {
    return {};
  }
  const std::set<NodeId>& set = data_->neighbors_of(id);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<AdjacencyEdge> TopologySnapshot::adjacency_of(const NodeId& id) const {
  std::vector<AdjacencyEdge> out;
  if (data_ == nullptr) {
    return out;
  }
  for (const NodeId& neighbor : data_->neighbors_of(id)) {
    const AdjacencyEdge* edge = data_->find_adjacency(id, neighbor);
    if (edge != nullptr) {
      out.push_back(*edge);
    }
  }
  std::sort(out.begin(), out.end(), adjacency_less);
  return out;
}

std::vector<NodeId> TopologySnapshot::facilities() const {
  if (data_ == nullptr) {
    return {};
  }
  const std::set<NodeId>& set = data_->nodes_of_kind(NodeKind::Facility);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologySnapshot::nodes_of_kind(NodeKind kind) const {
  if (data_ == nullptr) {
    return {};
  }
  const std::set<NodeId>& set = data_->nodes_of_kind(kind);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::optional<NodeId> TopologySnapshot::facility_of(const NodeId& id) const {
  return (data_ == nullptr) ? std::nullopt : data_->facility_of(id);
}

std::optional<std::uint32_t> TopologySnapshot::depth_of(const NodeId& id) const {
  return (data_ == nullptr) ? std::nullopt : data_->depth_of(id);
}

const DomainDeclaration* TopologySnapshot::find_domain(const DomainId& id) const {
  return (data_ == nullptr) ? nullptr : data_->find_domain(id);
}

std::vector<DomainDeclaration> TopologySnapshot::domains() const {
  return (data_ == nullptr) ? std::vector<DomainDeclaration>{} : collect_domains(*data_);
}

std::vector<NodeId> TopologySnapshot::nodes_in_domain(const DomainId& domain) const {
  if (data_ == nullptr) {
    return {};
  }
  const std::set<NodeId>& set = data_->nodes_in_domain(domain);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<DomainAssociation> TopologySnapshot::associations_of(const NodeId& node) const {
  std::vector<DomainAssociation> out;
  if (data_ == nullptr) {
    return out;
  }
  const auto it = data_->node_associations.find(node);
  if (it == data_->node_associations.end()) {
    return out;
  }
  for (const AssociationKey& key : it->second) {
    const auto found = data_->associations.find(key);
    if (found != data_->associations.end()) {
      out.push_back(found->second);
    }
  }
  return out;
}

std::vector<NodeRecord> TopologySnapshot::nodes() const {
  return (data_ == nullptr) ? std::vector<NodeRecord>{} : collect_nodes(*data_);
}

std::vector<NodeId> TopologySnapshot::node_ids() const {
  return (data_ == nullptr) ? std::vector<NodeId>{} : collect_node_ids(*data_);
}

std::vector<ContainmentEdge> TopologySnapshot::containment_edges() const {
  return (data_ == nullptr) ? std::vector<ContainmentEdge>{} : collect_containment(*data_);
}

std::vector<AdjacencyEdge> TopologySnapshot::adjacency_edges() const {
  return (data_ == nullptr) ? std::vector<AdjacencyEdge>{} : collect_adjacency(*data_);
}

std::vector<DomainAssociation> TopologySnapshot::associations() const {
  return (data_ == nullptr) ? std::vector<DomainAssociation>{} : collect_associations(*data_);
}

Result<std::vector<NodeId>> TopologySnapshot::traverse(const NodeId& root, TraversalOrder order,
                                                       TraversalLimits bounds) const {
  if (data_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "snapshot holds no topology");
  }
  return data_->traverse(root, order, bounds);
}

Result<bool> TopologySnapshot::is_descendant_of(const NodeId& node, const NodeId& ancestor) const {
  if (data_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "snapshot holds no topology");
  }
  FT_TRYV(require_node(*data_, node, "node"));
  FT_TRYV(require_node(*data_, ancestor, "ancestor"));
  return data_->is_descendant_of(node, ancestor);
}

Result<std::optional<NodeId>> TopologySnapshot::lowest_common_ancestor(const NodeId& lhs, const NodeId& rhs) const {
  if (data_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "snapshot holds no topology");
  }
  FT_TRYV(require_node(*data_, lhs, "node"));
  FT_TRYV(require_node(*data_, rhs, "node"));
  const std::optional<NodeId> lhs_facility = data_->facility_of(lhs);
  const std::optional<NodeId> rhs_facility = data_->facility_of(rhs);
  if (!lhs_facility.has_value() || !rhs_facility.has_value() || *lhs_facility != *rhs_facility) {
    return std::optional<NodeId>{};
  }
  // Both chains are root-first, so the deepest common ancestor is the last
  // element of their common prefix.
  std::vector<NodeId> lhs_chain = data_->ancestry(lhs);
  lhs_chain.push_back(lhs);
  std::vector<NodeId> rhs_chain = data_->ancestry(rhs);
  rhs_chain.push_back(rhs);

  std::optional<NodeId> best;
  const std::size_t limit = std::min(lhs_chain.size(), rhs_chain.size());
  for (std::size_t index = 0; index < limit; ++index) {
    if (!(lhs_chain[index] == rhs_chain[index])) {
      break;
    }
    best = lhs_chain[index];
  }
  return best;
}

// ---------------------------------------------------------------------------
// TopologyBuilder
// ---------------------------------------------------------------------------

TopologyBuilder::TopologyBuilder() : data_(std::make_unique<GraphData>()) {}

TopologyBuilder::TopologyBuilder(TopologyLimits limits) : data_(std::make_unique<GraphData>()) {
  data_->limits = limits;
}

TopologyBuilder::~TopologyBuilder() = default;
TopologyBuilder::TopologyBuilder(TopologyBuilder&&) noexcept = default;
TopologyBuilder& TopologyBuilder::operator=(TopologyBuilder&&) noexcept = default;

Result<TopologyBuilder> TopologyBuilder::from_snapshot(const TopologySnapshot& snapshot) {
  TopologyBuilder builder;
  if (snapshot.valid()) {
    builder.data_ = std::make_unique<GraphData>(*snapshot.data_);
  }
  return builder;
}

const TopologyLimits& TopologyBuilder::limits() const noexcept { return data_->limits; }

void TopologyBuilder::set_limits(TopologyLimits limits) noexcept { data_->limits = limits; }

const TopologyGeneration& TopologyBuilder::generation() const noexcept { return data_->generation; }

Result<void> TopologyBuilder::add_node(NodeRecord node) {
  FT_TRYV(require_identity(node.id, "node"));
  FT_TRYV(validate_provenance(node.provenance, data_->limits));
  FT_TRYV(validate_label(node.label, data_->limits.max_label_bytes, "node label"));
  if (data_->has_node(node.id)) {
    return Error(ErrorCode::AlreadyPresent, "a node with this identity already exists in this generation")
        .with_subject(node.id.str());
  }
  FT_TRYV(check_node_budget(*data_));
  if (node.kind == NodeKind::Zone) {
    if (!node.zone_member_kind.has_value()) {
      return Error(ErrorCode::MissingField,
                   "a zone node must declare the single node kind it groups (zone_member_kind)")
          .with_subject(node.id.str());
    }
    if (!node_kind_schema(*node.zone_member_kind).zonable) {
      return Error(ErrorCode::ZoneTargetKindInvalid,
                   "a zone can only group nodes of a zonable kind (hall, room, row, rack)")
          .with_subject(node.id.str());
    }
  } else if (node.zone_member_kind.has_value()) {
    return Error(ErrorCode::InvalidArgument, "only a zone node may declare zone_member_kind")
        .with_subject(node.id.str());
  }

  data_->kind_index[node.kind].insert(node.id);
  data_->nodes.emplace(node.id, std::move(node));
  return ok();
}

Result<void> TopologyBuilder::remove_node(const NodeId& id) {
  FT_TRYV(require_node(*data_, id, "node"));
  if (!data_->children_of(id).empty()) {
    return Error(ErrorCode::NodeHasChildren, "node still contains physical children").with_subject(id.str());
  }
  if (!data_->members_of(id).empty()) {
    return Error(ErrorCode::NodeHasChildren, "zone still groups members").with_subject(id.str());
  }
  if (!data_->neighbors_of(id).empty()) {
    return Error(ErrorCode::NodeReferenced, "node still has recorded physical adjacency").with_subject(id.str());
  }
  const auto associations = data_->node_associations.find(id);
  if (associations != data_->node_associations.end() && !associations->second.empty()) {
    return Error(ErrorCode::NodeReferenced, "node still references facility domains").with_subject(id.str());
  }

  const auto parent = data_->physical_parent.find(id);
  if (parent != data_->physical_parent.end()) {
    const NodeId parent_id = parent->second;
    data_->containment.erase(ContainmentKey{parent_id, id});
    data_->physical_parent.erase(parent);
    const auto siblings = data_->children.find(parent_id);
    if (siblings != data_->children.end()) {
      siblings->second.erase(id);
      if (siblings->second.empty()) {
        data_->children.erase(siblings);
      }
    }
  }
  const auto logical = data_->logical_parent.find(id);
  if (logical != data_->logical_parent.end()) {
    const NodeId zone_id = logical->second;
    data_->containment.erase(ContainmentKey{zone_id, id});
    data_->logical_parent.erase(logical);
    const auto members = data_->members.find(zone_id);
    if (members != data_->members.end()) {
      members->second.erase(id);
      if (members->second.empty()) {
        data_->members.erase(members);
      }
    }
  }

  const auto record = data_->nodes.find(id);
  if (record != data_->nodes.end()) {
    const auto kind_nodes = data_->kind_index.find(record->second.kind);
    if (kind_nodes != data_->kind_index.end()) {
      kind_nodes->second.erase(id);
      if (kind_nodes->second.empty()) {
        data_->kind_index.erase(kind_nodes);
      }
    }
    data_->nodes.erase(record);
  }
  data_->node_associations.erase(id);
  return ok();
}

Result<void> TopologyBuilder::set_node_label(const NodeId& id, std::string label, ProvenanceRecord provenance) {
  FT_TRYV(require_node(*data_, id, "node"));
  FT_TRYV(validate_label(label, data_->limits.max_label_bytes, "node label"));
  FT_TRYV(validate_provenance(provenance, data_->limits));
  NodeRecord& record = data_->nodes.at(id);
  record.label = std::move(label);
  record.provenance = std::move(provenance);
  return ok();
}

Result<void> TopologyBuilder::add_containment(ContainmentEdge edge) {
  FT_TRYV(require_node(*data_, edge.parent, "containment parent"));
  FT_TRYV(require_node(*data_, edge.child, "containment child"));
  FT_TRYV(validate_provenance(edge.provenance, data_->limits));
  if (edge.parent == edge.child) {
    return Error(ErrorCode::SelfEdge, "a node cannot contain itself").with_subject(edge.child.str());
  }
  if (data_->containment.find(ContainmentKey{edge.parent, edge.child}) != data_->containment.end()) {
    return Error(ErrorCode::AlreadyPresent, "this containment edge already exists").with_subject(edge.child.str());
  }

  const NodeKind parent_kind = data_->kind_of(edge.parent).value();
  const NodeKind child_kind = data_->kind_of(edge.child).value();
  if (!boundary_matches_parent(parent_kind, edge.boundary)) {
    return Error(ErrorCode::InvalidParentKind,
                 std::string("a containment edge from a ") + std::string(node_kind_token(parent_kind)) +
                     " must use the " +
                     (parent_kind == NodeKind::Zone ? "logical" : "physical") + " boundary")
        .with_subject(edge.child.str());
  }

  if (edge.boundary == BoundaryKind::Physical) {
    if (child_kind == NodeKind::Facility) {
      return Error(ErrorCode::FacilityMustBeRoot, "a facility is a physical root and cannot be contained")
          .with_subject(edge.child.str());
    }
    if (node_kind_schema(child_kind).root_eligible) {
      return Error(ErrorCode::InvalidParentKind, "this node kind is a root and cannot be contained")
          .with_subject(edge.child.str());
    }
    if (!schema_allows_physical_child(parent_kind, child_kind)) {
      return Error(ErrorCode::InvalidParentKind, container_kind_explanation(parent_kind, child_kind))
          .with_subject(edge.child.str());
    }
    const auto existing = data_->physical_parent.find(edge.child);
    if (existing != data_->physical_parent.end()) {
      if (existing->second == edge.parent) {
        return Error(ErrorCode::AlreadyAtParent, "node is already contained by this parent")
            .with_subject(edge.child.str());
      }
      return Error(ErrorCode::IdentityConflict,
                   "node already has a different physical parent; use a move to change containment")
          .with_subject(edge.child.str());
    }
    if (data_->is_descendant_of(edge.parent, edge.child)) {
      return Error(ErrorCode::ContainmentCycle, "containment would create a cycle").with_subject(edge.child.str());
    }
    // The depth guard only applies when the parent's own depth is already
    // known: a caller may add edges in any order, and a container that is not
    // yet attached to a facility is reported by whole-graph validation as an
    // orphan rather than being rejected here for the wrong reason.
    const std::optional<std::uint32_t> parent_depth = data_->depth_of(edge.parent);
    if (parent_depth.has_value() && (*parent_depth + 1U) > kMaxStructuralDepth) {
      return Error(ErrorCode::InvalidParentKind, "containment would exceed the maximum structural depth")
          .with_subject(edge.child.str());
    }
  } else {
    if (child_kind == NodeKind::Facility || child_kind == NodeKind::Zone) {
      return Error(ErrorCode::ZoneMemberKindInvalid, "a facility or zone cannot be a zone member")
          .with_subject(edge.child.str());
    }
    if (!node_kind_schema(child_kind).zonable) {
      return Error(ErrorCode::ZoneMemberKindInvalid, "this node kind cannot be a zone member")
          .with_subject(edge.child.str());
    }
    const NodeRecord& zone = data_->nodes.at(edge.parent);
    if (zone.zone_member_kind.has_value() && *zone.zone_member_kind != child_kind) {
      return Error(ErrorCode::ZoneTargetKindInvalid,
                   std::string("zone groups ") + std::string(node_kind_token(*zone.zone_member_kind)) +
                       " nodes but this member is a " + std::string(node_kind_token(child_kind)))
          .with_subject(edge.child.str());
    }
    const auto existing = data_->logical_parent.find(edge.child);
    if (existing != data_->logical_parent.end()) {
      if (existing->second == edge.parent) {
        return Error(ErrorCode::AlreadyAtParent, "node is already a member of this zone")
            .with_subject(edge.child.str());
      }
      return Error(ErrorCode::ZoneConflict, "node is already a member of a different zone")
          .with_subject(edge.child.str());
    }
    const std::optional<NodeId> zone_facility = data_->facility_of(edge.parent);
    const std::optional<NodeId> member_facility = data_->facility_of(edge.child);
    // As with the depth guard, this check is applied only when both facilities
    // are already resolvable so that edge insertion order never decides
    // validity; whole-graph validation settles the rest.
    if (zone_facility.has_value() && member_facility.has_value() && *zone_facility != *member_facility) {
      return Error(ErrorCode::ZoneConflict, "a zone may only group nodes of its own facility")
          .with_subject(edge.child.str());
    }
  }

  if (data_->containment.size() >= data_->limits.max_containment_edges) {
    return Error(ErrorCode::LimitExceeded, "containment budget exhausted");
  }
  if (edge.boundary == BoundaryKind::Physical) {
    data_->physical_parent[edge.child] = edge.parent;
    data_->children[edge.parent].insert(edge.child);
  } else {
    data_->logical_parent[edge.child] = edge.parent;
    data_->members[edge.parent].insert(edge.child);
  }
  data_->containment.emplace(ContainmentKey{edge.parent, edge.child}, std::move(edge));
  return ok();
}

Result<void> TopologyBuilder::remove_containment(const NodeId& parent, const NodeId& child) {
  FT_TRYV(require_node(*data_, parent, "containment parent"));
  FT_TRYV(require_node(*data_, child, "containment child"));
  const ContainmentKey key{parent, child};
  const auto found = data_->containment.find(key);
  if (found == data_->containment.end()) {
    return Error(ErrorCode::NotFound, "this containment edge does not exist").with_subject(child.str());
  }
  const BoundaryKind boundary = found->second.boundary;
  data_->containment.erase(found);
  if (boundary == BoundaryKind::Physical) {
    data_->physical_parent.erase(child);
    const auto siblings = data_->children.find(parent);
    if (siblings != data_->children.end()) {
      siblings->second.erase(child);
      if (siblings->second.empty()) {
        data_->children.erase(siblings);
      }
    }
  } else {
    data_->logical_parent.erase(child);
    const auto members = data_->members.find(parent);
    if (members != data_->members.end()) {
      members->second.erase(child);
      if (members->second.empty()) {
        data_->members.erase(members);
      }
    }
  }
  return ok();
}

Result<void> TopologyBuilder::move_node(const NodeId& child, const NodeId& new_parent, ProvenanceRecord provenance) {
  FT_TRYV(require_node(*data_, child, "node"));
  FT_TRYV(require_node(*data_, new_parent, "new parent"));
  FT_TRYV(validate_provenance(provenance, data_->limits));
  const NodeKind child_kind = data_->kind_of(child).value();
  if (node_kind_schema(child_kind).root_eligible) {
    return Error(ErrorCode::FacilityMustBeRoot, "a physical root cannot be moved").with_subject(child.str());
  }
  const auto current = data_->physical_parent.find(child);
  if (current == data_->physical_parent.end()) {
    return Error(ErrorCode::NonFacilityMustBeContained, "node has no physical parent to move from")
        .with_subject(child.str());
  }
  if (current->second == new_parent) {
    return Error(ErrorCode::AlreadyAtParent, "node is already contained by the requested parent")
        .with_subject(child.str());
  }
  if (new_parent == child) {
    return Error(ErrorCode::ContainmentCycle, "a node cannot be moved under itself").with_subject(child.str());
  }
  const NodeKind parent_kind = data_->kind_of(new_parent).value();
  if (!schema_allows_physical_child(parent_kind, child_kind)) {
    return Error(ErrorCode::InvalidParentKind, container_kind_explanation(parent_kind, child_kind))
        .with_subject(child.str());
  }
  if (data_->is_descendant_of(new_parent, child)) {
    return Error(ErrorCode::ContainmentCycle, "moving the node under its own descendant would create a cycle")
        .with_subject(child.str());
  }
  const std::optional<std::uint32_t> parent_depth = data_->depth_of(new_parent);
  if (parent_depth.has_value() && (*parent_depth + 1U) > kMaxStructuralDepth) {
    return Error(ErrorCode::InvalidParentKind, "the move would exceed the maximum structural depth")
        .with_subject(child.str());
  }

  const NodeId old_parent = current->second;
  data_->containment.erase(ContainmentKey{old_parent, child});
  const auto siblings = data_->children.find(old_parent);
  if (siblings != data_->children.end()) {
    siblings->second.erase(child);
    if (siblings->second.empty()) {
      data_->children.erase(siblings);
    }
  }
  data_->physical_parent[child] = new_parent;
  data_->children[new_parent].insert(child);
  ContainmentEdge edge;
  edge.parent = new_parent;
  edge.child = child;
  edge.boundary = BoundaryKind::Physical;
  edge.provenance = std::move(provenance);
  data_->containment.emplace(ContainmentKey{new_parent, child}, std::move(edge));

  // A move can take a zone to a different facility; its members would then be
  // outside the zone's facility, which the whole-graph validation rejects.
  return ok();
}

Result<void> TopologyBuilder::add_adjacency(AdjacencyEdge edge) {
  FT_TRYV(require_node(*data_, edge.first, "adjacency endpoint"));
  FT_TRYV(require_node(*data_, edge.second, "adjacency endpoint"));
  FT_TRYV(validate_provenance(edge.provenance, data_->limits));
  if (edge.first == edge.second) {
    return Error(ErrorCode::SelfEdge, "a node cannot be adjacent to itself").with_subject(edge.first.str());
  }
  const NodeKind first_kind = data_->kind_of(edge.first).value();
  const NodeKind second_kind = data_->kind_of(edge.second).value();
  if (!schema_allows_adjacency(first_kind, second_kind)) {
    return Error(ErrorCode::InvalidArgument,
                 "physical adjacency is only defined between physical nodes (building, hall, room, row, rack)")
        .with_subject(edge.first.str());
  }
  const std::optional<NodeId> first_facility = data_->facility_of(edge.first);
  const std::optional<NodeId> second_facility = data_->facility_of(edge.second);
  if (!first_facility.has_value() || !second_facility.has_value()) {
    return Error(ErrorCode::NonFacilityMustBeContained, "adjacency endpoints must be contained by a facility")
        .with_subject(edge.first.str());
  }
  if (*first_facility != *second_facility) {
    return Error(ErrorCode::AdjacencyCrossFacility,
                 "physical adjacency across facilities is not modelled by this repository")
        .with_subject(edge.first.str());
  }
  const AdjacencyKey key = make_adjacency_key(edge.first, edge.second);
  if (data_->adjacency.find(key) != data_->adjacency.end()) {
    return Error(ErrorCode::DuplicateEdge, "an adjacency between these nodes already exists")
        .with_subject(edge.first.str());
  }
  if (data_->adjacency.size() >= data_->limits.max_adjacency_edges) {
    return Error(ErrorCode::LimitExceeded, "adjacency budget exhausted");
  }
  edge.first = key.first;
  edge.second = key.second;
  data_->neighbor_index[key.first].insert(key.second);
  data_->neighbor_index[key.second].insert(key.first);
  data_->adjacency.emplace(key, std::move(edge));
  return ok();
}

Result<void> TopologyBuilder::remove_adjacency(const NodeId& first, const NodeId& second, AdjacencyKind kind) {
  FT_TRYV(require_node(*data_, first, "adjacency endpoint"));
  FT_TRYV(require_node(*data_, second, "adjacency endpoint"));
  const AdjacencyKey key = make_adjacency_key(first, second);
  const auto found = data_->adjacency.find(key);
  if (found == data_->adjacency.end()) {
    return Error(ErrorCode::NotFound, "no adjacency exists between these nodes").with_subject(first.str());
  }
  if (found->second.kind != kind) {
    return Error(ErrorCode::NotFound,
                 std::string("the adjacency exists with kind \"") +
                     std::string(adjacency_kind_token(found->second.kind)) + "\", not \"" +
                     std::string(adjacency_kind_token(kind)) + "\"")
        .with_subject(first.str());
  }
  data_->adjacency.erase(found);
  const auto first_neighbors = data_->neighbor_index.find(key.first);
  if (first_neighbors != data_->neighbor_index.end()) {
    first_neighbors->second.erase(key.second);
    if (first_neighbors->second.empty()) {
      data_->neighbor_index.erase(first_neighbors);
    }
  }
  const auto second_neighbors = data_->neighbor_index.find(key.second);
  if (second_neighbors != data_->neighbor_index.end()) {
    second_neighbors->second.erase(key.first);
    if (second_neighbors->second.empty()) {
      data_->neighbor_index.erase(second_neighbors);
    }
  }
  return ok();
}

Result<void> TopologyBuilder::declare_domain(DomainDeclaration declaration) {
  if (declaration.id.empty()) {
    return Error(ErrorCode::MissingField, "domain identity is empty");
  }
  FT_TRYV(validate_provenance(declaration.provenance, data_->limits));
  FT_TRYV(validate_label(declaration.label, data_->limits.max_label_bytes, "domain label"));
  const auto existing = data_->domains.find(declaration.id);
  if (existing != data_->domains.end()) {
    if (existing->second.kind != declaration.kind) {
      return Error(ErrorCode::IdentityConflict, "domain identity is already declared with a different kind")
          .with_subject(declaration.id.str());
    }
    return Error(ErrorCode::AlreadyPresent, "domain identity is already declared").with_subject(declaration.id.str());
  }
  if (data_->domains.size() >= data_->limits.max_domain_declarations) {
    return Error(ErrorCode::LimitExceeded, "domain declaration budget exhausted");
  }
  data_->domains.emplace(declaration.id, std::move(declaration));
  return ok();
}

Result<void> TopologyBuilder::retire_domain(const DomainId& id) {
  if (id.empty()) {
    return Error(ErrorCode::MissingField, "domain identity is empty");
  }
  const auto found = data_->domains.find(id);
  if (found == data_->domains.end()) {
    return Error(ErrorCode::NotFound, "domain is not declared in this generation").with_subject(id.str());
  }
  const auto members = data_->domain_members.find(id);
  if (members != data_->domain_members.end() && !members->second.empty()) {
    return Error(ErrorCode::NodeReferenced, "domain is still referenced by nodes").with_subject(id.str());
  }
  data_->domains.erase(found);
  return ok();
}

Result<void> TopologyBuilder::add_association(DomainAssociation association) {
  FT_TRYV(require_node(*data_, association.node, "association node"));
  FT_TRYV(validate_provenance(association.provenance, data_->limits));
  if (association.domain.empty()) {
    return Error(ErrorCode::MissingField, "association domain identity is empty");
  }
  const DomainDeclaration* declaration = data_->find_domain(association.domain);
  if (declaration == nullptr) {
    return Error(ErrorCode::NotFound, "domain is not declared in this generation")
        .with_subject(association.domain.str());
  }
  if (declaration->kind != association.kind) {
    return Error(ErrorCode::IdentityConflict,
                 std::string("domain is declared as \"") + std::string(domain_kind_token(declaration->kind)) +
                     "\" but the association claims \"" + std::string(domain_kind_token(association.kind)) + "\"")
        .with_subject(association.domain.str());
  }
  const AssociationKey key{association.node, association.domain};
  if (data_->associations.find(key) != data_->associations.end()) {
    return Error(ErrorCode::AlreadyPresent, "this node already references this domain")
        .with_subject(association.node.str());
  }
  if (data_->associations.size() >= data_->limits.max_domain_associations) {
    return Error(ErrorCode::LimitExceeded, "domain association budget exhausted");
  }
  data_->node_associations[key.node].insert(key);
  data_->domain_members[key.domain].insert(key.node);
  data_->associations.emplace(key, std::move(association));
  return ok();
}

Result<void> TopologyBuilder::remove_association(const NodeId& node, const DomainId& domain) {
  FT_TRYV(require_node(*data_, node, "association node"));
  const AssociationKey key{node, domain};
  const auto found = data_->associations.find(key);
  if (found == data_->associations.end()) {
    return Error(ErrorCode::NotFound, "this node does not reference this domain").with_subject(node.str());
  }
  data_->associations.erase(found);
  const auto node_refs = data_->node_associations.find(node);
  if (node_refs != data_->node_associations.end()) {
    node_refs->second.erase(key);
    if (node_refs->second.empty()) {
      data_->node_associations.erase(node_refs);
    }
  }
  const auto domain_refs = data_->domain_members.find(domain);
  if (domain_refs != data_->domain_members.end()) {
    domain_refs->second.erase(node);
    if (domain_refs->second.empty()) {
      data_->domain_members.erase(domain_refs);
    }
  }
  return ok();
}

// -- builder queries --------------------------------------------------------

const NodeRecord* TopologyBuilder::find_node(const NodeId& id) const { return data_->find_node(id); }
bool TopologyBuilder::contains_node(const NodeId& id) const { return data_->has_node(id); }
std::optional<NodeKind> TopologyBuilder::node_kind(const NodeId& id) const { return data_->kind_of(id); }

std::optional<NodeId> TopologyBuilder::physical_parent(const NodeId& id) const {
  const auto it = data_->physical_parent.find(id);
  return (it == data_->physical_parent.end()) ? std::nullopt : std::optional<NodeId>(it->second);
}

std::optional<NodeId> TopologyBuilder::zone_of(const NodeId& id) const {
  const auto it = data_->logical_parent.find(id);
  return (it == data_->logical_parent.end()) ? std::nullopt : std::optional<NodeId>(it->second);
}

std::vector<NodeId> TopologyBuilder::children(const NodeId& id) const {
  const std::set<NodeId>& set = data_->children_of(id);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologyBuilder::zone_members(const NodeId& zone) const {
  const std::set<NodeId>& set = data_->members_of(zone);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologyBuilder::ancestry(const NodeId& id) const { return data_->ancestry(id); }
std::vector<NodeId> TopologyBuilder::descendants(const NodeId& id) const { return data_->descendants(id); }

std::vector<NodeId> TopologyBuilder::neighbors(const NodeId& id) const {
  const std::set<NodeId>& set = data_->neighbors_of(id);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologyBuilder::facilities() const {
  const std::set<NodeId>& set = data_->nodes_of_kind(NodeKind::Facility);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<NodeId> TopologyBuilder::nodes_of_kind(NodeKind kind) const {
  const std::set<NodeId>& set = data_->nodes_of_kind(kind);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::optional<NodeId> TopologyBuilder::facility_of(const NodeId& id) const { return data_->facility_of(id); }
std::optional<std::uint32_t> TopologyBuilder::depth_of(const NodeId& id) const { return data_->depth_of(id); }
const DomainDeclaration* TopologyBuilder::find_domain(const DomainId& id) const { return data_->find_domain(id); }

std::vector<NodeId> TopologyBuilder::nodes_in_domain(const DomainId& domain) const {
  const std::set<NodeId>& set = data_->nodes_in_domain(domain);
  return std::vector<NodeId>(set.begin(), set.end());
}

std::vector<DomainAssociation> TopologyBuilder::associations_of(const NodeId& node) const {
  std::vector<DomainAssociation> out;
  const auto it = data_->node_associations.find(node);
  if (it == data_->node_associations.end()) {
    return out;
  }
  for (const AssociationKey& key : it->second) {
    const auto found = data_->associations.find(key);
    if (found != data_->associations.end()) {
      out.push_back(found->second);
    }
  }
  return out;
}

std::size_t TopologyBuilder::node_count() const { return data_->nodes.size(); }
std::size_t TopologyBuilder::containment_count() const { return data_->containment.size(); }
std::size_t TopologyBuilder::adjacency_count() const { return data_->adjacency.size(); }
std::size_t TopologyBuilder::domain_count() const { return data_->domains.size(); }
std::size_t TopologyBuilder::association_count() const { return data_->associations.size(); }
std::vector<NodeId> TopologyBuilder::node_ids() const { return collect_node_ids(*data_); }
std::vector<ContainmentEdge> TopologyBuilder::containment_edges() const { return collect_containment(*data_); }
std::vector<AdjacencyEdge> TopologyBuilder::adjacency_edges() const { return collect_adjacency(*data_); }
std::vector<DomainAssociation> TopologyBuilder::associations() const { return collect_associations(*data_); }
std::vector<DomainDeclaration> TopologyBuilder::domains() const { return collect_domains(*data_); }
std::vector<NodeRecord> TopologyBuilder::nodes() const { return collect_nodes(*data_); }

Result<std::vector<NodeId>> TopologyBuilder::traverse(const NodeId& root, TraversalOrder order,
                                                      TraversalLimits limits) const {
  return data_->traverse(root, order, limits);
}

Result<bool> TopologyBuilder::is_descendant_of(const NodeId& node, const NodeId& ancestor) const {
  FT_TRYV(require_node(*data_, node, "node"));
  FT_TRYV(require_node(*data_, ancestor, "ancestor"));
  return data_->is_descendant_of(node, ancestor);
}

ValidationReport TopologyBuilder::validate() const { return validate_graph(*data_, data_->limits); }

TopologyStats TopologyBuilder::stats() const { return data_->stats(); }

Result<TopologySnapshot> TopologyBuilder::build(const TopologyGeneration& generation) const {
  const ValidationReport report = validate();
  if (!report.valid) {
    const ValidationIssue& first = report.issues.front();
    return Error(first.code, "candidate generation failed validation: " + first.explanation).with_subject(first.subject);
  }
  auto frozen = std::make_shared<GraphData>(*data_);
  frozen->generation = generation;
  return snapshot_from_data(std::move(frozen));
}

Result<TopologySnapshot> assemble_topology(std::vector<NodeRecord> nodes, std::vector<ContainmentEdge> containment,
                                           std::vector<AdjacencyEdge> adjacency,
                                           std::vector<DomainDeclaration> domains,
                                           std::vector<DomainAssociation> associations,
                                           const TopologyGeneration& generation, TopologyLimits limits) {
  std::string explanation;
  if (!validate_limits(limits, explanation)) {
    return Error(ErrorCode::InvalidArgument, "invalid topology limits: " + explanation);
  }
  TopologyBuilder builder(limits);
  for (NodeRecord& node : nodes) {
    FT_TRYV(builder.add_node(std::move(node)));
  }
  for (ContainmentEdge& edge : containment) {
    FT_TRYV(builder.add_containment(std::move(edge)));
  }
  for (AdjacencyEdge& edge : adjacency) {
    FT_TRYV(builder.add_adjacency(std::move(edge)));
  }
  for (DomainDeclaration& declaration : domains) {
    FT_TRYV(builder.declare_domain(std::move(declaration)));
  }
  for (DomainAssociation& association : associations) {
    FT_TRYV(builder.add_association(std::move(association)));
  }
  return builder.build(generation);
}

// ---------------------------------------------------------------------------
// PublishedTopology
// ---------------------------------------------------------------------------

PublishedTopology::PublishedTopology() noexcept : current_(), publishing_(false), publish_count_(0) {}

PublishedTopology::~PublishedTopology() = default;

std::shared_ptr<const TopologySnapshot> PublishedTopology::current() const noexcept { return current_.load(); }

bool PublishedTopology::publish(const TopologySnapshot& snapshot) noexcept {
  if (!snapshot.valid()) {
    return false;
  }
  // The shared handle is created before the publisher flag is taken so that a
  // failure to allocate cannot leave the flag set with nothing published.
  auto shared = std::make_shared<const TopologySnapshot>(snapshot);
  bool expected = false;
  if (!publishing_.compare_exchange_strong(expected, true)) {
    return false;
  }
  current_.store(std::move(shared));
  publish_count_.fetch_add(1);
  publishing_.store(false);
  return true;
}

std::uint64_t PublishedTopology::publish_count() const noexcept { return publish_count_.load(); }

}  // namespace dccp::facility_topology
