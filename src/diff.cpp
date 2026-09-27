// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/diff.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "canonical_internal.hpp"
#include "graph_data.hpp"

namespace dccp::facility_topology {
namespace {

/// Walks two canonically ordered maps in lockstep, collecting the entries that
/// exist on only one side. Both output vectors are therefore in canonical
/// order by construction.
template <class Map>
void diff_ordered(const Map& from, const Map& to, std::vector<typename Map::mapped_type>& removed,
                  std::vector<typename Map::mapped_type>& added) {
  auto lhs = from.begin();
  auto rhs = to.begin();
  while (lhs != from.end() || rhs != to.end()) {
    if (lhs == from.end()) {
      added.push_back(rhs->second);
      ++rhs;
      continue;
    }
    if (rhs == to.end()) {
      removed.push_back(lhs->second);
      ++lhs;
      continue;
    }
    if (lhs->first < rhs->first) {
      removed.push_back(lhs->second);
      ++lhs;
      continue;
    }
    if (rhs->first < lhs->first) {
      added.push_back(rhs->second);
      ++rhs;
      continue;
    }
    ++lhs;
    ++rhs;
  }
}

void append_node_list(std::string& out, std::string_view label, const std::vector<NodeId>& ids) {
  out.append(label);
  out.push_back(' ');
  out.append(std::to_string(ids.size()));
  for (const NodeId& id : ids) {
    out.push_back(' ');
    out.append(id.str());
  }
  out.push_back('\n');
}

}  // namespace

bool TopologyDiff::empty() const noexcept { return change_count() == 0; }

std::size_t TopologyDiff::change_count() const noexcept {
  return nodes_added.size() + nodes_removed.size() + nodes_relabeled.size() + node_scope_changed.size() +
         containment_added.size() + containment_removed.size() + adjacency_added.size() +
         adjacency_removed.size() + domains_added.size() + domains_removed.size() + associations_added.size() +
         associations_removed.size();
}

std::string TopologyDiff::to_string() const {
  std::string out;
  out.append("diff from=");
  out.append(text::format_u64(from.value()));
  out.append(" to=");
  out.append(text::format_u64(to.value()));
  out.append(" changes=");
  out.append(std::to_string(change_count()));
  out.push_back('\n');

  append_node_list(out, "nodes-added", nodes_added);
  append_node_list(out, "nodes-removed", nodes_removed);

  out.append("nodes-relabeled ");
  out.append(std::to_string(nodes_relabeled.size()));
  for (const NodeLabelChange& change : nodes_relabeled) {
    out.push_back(' ');
    out.append(change.id.str());
    out.append("=\"");
    out.append(text::escape_quoted(change.before));
    out.append("\"->\"");
    out.append(text::escape_quoted(change.after));
    out.push_back('"');
  }
  out.push_back('\n');

  out.append("node-scope-changed ");
  out.append(std::to_string(node_scope_changed.size()));
  for (const NodeZoneScopeChange& change : node_scope_changed) {
    out.push_back(' ');
    out.append(change.id.str());
    out.push_back('=');
    out.append(node_kind_token(change.before));
    out.append("->");
    out.append(node_kind_token(change.after));
  }
  out.push_back('\n');

  out.append("containment-added ");
  out.append(std::to_string(containment_added.size()));
  for (const ContainmentEdge& edge : containment_added) {
    out.push_back(' ');
    out.append(edge.parent.str());
    out.push_back('>');
    out.append(edge.child.str());
    out.push_back('/');
    out.append(boundary_kind_token(edge.boundary));
  }
  out.push_back('\n');

  out.append("containment-removed ");
  out.append(std::to_string(containment_removed.size()));
  for (const ContainmentEdge& edge : containment_removed) {
    out.push_back(' ');
    out.append(edge.parent.str());
    out.push_back('>');
    out.append(edge.child.str());
    out.push_back('/');
    out.append(boundary_kind_token(edge.boundary));
  }
  out.push_back('\n');

  out.append("adjacency-added ");
  out.append(std::to_string(adjacency_added.size()));
  for (const AdjacencyEdge& edge : adjacency_added) {
    out.push_back(' ');
    out.append(edge.first.str());
    out.push_back('~');
    out.append(edge.second.str());
    out.push_back('/');
    out.append(adjacency_kind_token(edge.kind));
  }
  out.push_back('\n');

  out.append("adjacency-removed ");
  out.append(std::to_string(adjacency_removed.size()));
  for (const AdjacencyEdge& edge : adjacency_removed) {
    out.push_back(' ');
    out.append(edge.first.str());
    out.push_back('~');
    out.append(edge.second.str());
    out.push_back('/');
    out.append(adjacency_kind_token(edge.kind));
  }
  out.push_back('\n');

  out.append("domains-added ");
  out.append(std::to_string(domains_added.size()));
  for (const DomainDeclaration& declaration : domains_added) {
    out.push_back(' ');
    out.append(declaration.id.str());
    out.push_back('/');
    out.append(domain_kind_token(declaration.kind));
  }
  out.push_back('\n');

  out.append("domains-removed ");
  out.append(std::to_string(domains_removed.size()));
  for (const DomainDeclaration& declaration : domains_removed) {
    out.push_back(' ');
    out.append(declaration.id.str());
    out.push_back('/');
    out.append(domain_kind_token(declaration.kind));
  }
  out.push_back('\n');

  out.append("associations-added ");
  out.append(std::to_string(associations_added.size()));
  for (const DomainAssociation& association : associations_added) {
    out.push_back(' ');
    out.append(association.node.str());
    out.push_back('@');
    out.append(association.domain.str());
    out.push_back('/');
    out.append(domain_kind_token(association.kind));
  }
  out.push_back('\n');

  out.append("associations-removed ");
  out.append(std::to_string(associations_removed.size()));
  for (const DomainAssociation& association : associations_removed) {
    out.push_back(' ');
    out.append(association.node.str());
    out.push_back('@');
    out.append(association.domain.str());
    out.push_back('/');
    out.append(domain_kind_token(association.kind));
  }
  out.push_back('\n');
  return out;
}

Result<TopologyDiff> diff(const TopologySnapshot& from, const TopologySnapshot& to) {
  const GraphData* lhs = SnapshotAccess::data(from);
  const GraphData* rhs = SnapshotAccess::data(to);
  if (lhs == nullptr || rhs == nullptr) {
    return Error(ErrorCode::NotInitialized, "diff requires two snapshots that hold topology");
  }

  TopologyDiff result;
  result.from = lhs->generation;
  result.to = rhs->generation;

  diff_ordered(lhs->containment, rhs->containment, result.containment_removed, result.containment_added);
  diff_ordered(lhs->adjacency, rhs->adjacency, result.adjacency_removed, result.adjacency_added);
  diff_ordered(lhs->domains, rhs->domains, result.domains_removed, result.domains_added);
  diff_ordered(lhs->associations, rhs->associations, result.associations_removed, result.associations_added);

  auto lhs_node = lhs->nodes.begin();
  auto rhs_node = rhs->nodes.begin();
  while (lhs_node != lhs->nodes.end() || rhs_node != rhs->nodes.end()) {
    if (lhs_node == lhs->nodes.end()) {
      result.nodes_added.push_back(rhs_node->first);
      ++rhs_node;
      continue;
    }
    if (rhs_node == rhs->nodes.end()) {
      result.nodes_removed.push_back(lhs_node->first);
      ++lhs_node;
      continue;
    }
    if (lhs_node->first < rhs_node->first) {
      result.nodes_removed.push_back(lhs_node->first);
      ++lhs_node;
      continue;
    }
    if (rhs_node->first < lhs_node->first) {
      result.nodes_added.push_back(rhs_node->first);
      ++rhs_node;
      continue;
    }
    const NodeRecord& before = lhs_node->second;
    const NodeRecord& after = rhs_node->second;
    if (before.kind != after.kind) {
      // A node that changed kind is not the same structural element: report it
      // as a removal plus an addition rather than inventing a retype.
      result.nodes_removed.push_back(lhs_node->first);
      result.nodes_added.push_back(rhs_node->first);
    } else {
      if (before.label != after.label) {
        result.nodes_relabeled.push_back(NodeLabelChange{before.id, before.label, after.label});
      }
      if (before.zone_member_kind != after.zone_member_kind) {
        result.node_scope_changed.push_back(NodeZoneScopeChange{
            before.id,
            before.zone_member_kind.value_or(NodeKind::Facility),
            after.zone_member_kind.value_or(NodeKind::Facility)});
      }
    }
    ++lhs_node;
    ++rhs_node;
  }
  return result;
}

}  // namespace dccp::facility_topology
