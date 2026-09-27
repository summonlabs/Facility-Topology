// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example: build, validate, query and evolve an in-memory facility topology.
//
// This example uses only the public library surface. The topology it builds is
// synthetic: it describes no real site.

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/facility_topology/diff.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "dccp/facility_topology/topology.hpp"

using namespace dccp::facility_topology;

namespace {

ProvenanceRecord provenance(std::string_view actor, std::string_view reason) {
  ProvenanceRecord record;
  record.actor = ActorId::parse(actor).value();
  record.source = "example";
  record.reason = std::string(reason);
  record.recorded_at = "2026-02-01T00:00:00Z";
  return record;
}

NodeRecord node(std::string_view id, NodeKind kind, std::string label,
                std::optional<NodeKind> scope = std::nullopt) {
  NodeRecord record;
  record.id = NodeId::parse(id).value();
  record.kind = kind;
  record.label = std::move(label);
  record.zone_member_kind = scope;
  record.provenance = provenance("example", "initial topology");
  return record;
}

ContainmentEdge contain(std::string_view parent, std::string_view child) {
  ContainmentEdge edge;
  edge.parent = NodeId::parse(parent).value();
  edge.child = NodeId::parse(child).value();
  edge.boundary = BoundaryKind::Physical;
  edge.provenance = provenance("example", "initial topology");
  return edge;
}

}  // namespace

int main() {
  // 1. Assemble generation 1 from records. The library validates the whole
  //    graph before it hands back a snapshot.
  std::vector<NodeRecord> nodes = {
      node("fac-1", NodeKind::Facility, "Example Facility"),
      node("bld-1", NodeKind::Building, "Building 1"),
      node("hall-1", NodeKind::Hall, "Hall 1"),
      node("room-1", NodeKind::Room, "Room 1"),
      node("row-1", NodeKind::Row, "Row 1"),
      node("rack-01", NodeKind::Rack, "Rack 01"),
      node("rack-02", NodeKind::Rack, "Rack 02"),
      node("zone-a", NodeKind::Zone, "Zone A", NodeKind::Rack),
  };
  std::vector<ContainmentEdge> containment = {
      contain("fac-1", "bld-1"),   contain("bld-1", "hall-1"), contain("hall-1", "room-1"),
      contain("room-1", "row-1"),  contain("row-1", "rack-01"), contain("row-1", "rack-02"),
      contain("fac-1", "zone-a"),
  };
  ContainmentEdge membership;
  membership.parent = NodeId::parse("zone-a").value();
  membership.child = NodeId::parse("rack-01").value();
  membership.boundary = BoundaryKind::Logical;
  membership.provenance = provenance("example", "zone membership");
  containment.push_back(membership);

  std::vector<AdjacencyEdge> adjacency;
  AdjacencyEdge neighbours;
  neighbours.first = NodeId::parse("rack-01").value();
  neighbours.second = NodeId::parse("rack-02").value();
  neighbours.kind = AdjacencyKind::StructuralNeighbor;
  neighbours.provenance = provenance("example", "row layout");
  adjacency.push_back(neighbours);

  std::vector<DomainDeclaration> domains;
  DomainDeclaration power;
  power.id = DomainId::parse("pwr-a").value();
  power.kind = DomainKind::Power;
  power.label = "Power feed A";
  power.provenance = provenance("example", "domain reference");
  domains.push_back(power);

  std::vector<DomainAssociation> associations;
  DomainAssociation feed;
  feed.node = NodeId::parse("rack-01").value();
  feed.domain = DomainId::parse("pwr-a").value();
  feed.kind = DomainKind::Power;
  feed.provenance = provenance("example", "domain reference");
  associations.push_back(feed);

  auto generation_one = assemble_topology(nodes, containment, adjacency, domains, associations, TopologyGeneration(1));
  if (!generation_one.has_value()) {
    std::cerr << "assembly failed: " << generation_one.error().to_string() << "\n";
    return 1;
  }
  std::cout << "generation 1: " << generation_one->stats().to_string() << "\n";
  std::cout << "content digest: " << digest_tagged_hex(generation_one->content_digest()) << "\n";

  // 2. Query the immutable snapshot.
  const NodeId rack01 = NodeId::parse("rack-01").value();
  std::cout << "ancestry of rack-01:";
  for (const NodeId& ancestor : generation_one->ancestry(rack01)) {
    std::cout << " " << ancestor.str();
  }
  std::cout << "\n";
  std::cout << "neighbours of rack-01:";
  for (const NodeId& neighbour : generation_one->neighbors(rack01)) {
    std::cout << " " << neighbour.str();
  }
  std::cout << "\n";

  TraversalLimits bounds;
  bounds.max_depth = 16;
  auto visited = generation_one->traverse(NodeId::parse("hall-1").value(), TraversalOrder::DepthFirstPreOrder, bounds);
  if (!visited.has_value()) {
    std::cerr << "traversal failed: " << visited.error().to_string() << "\n";
    return 1;
  }
  std::cout << "depth-first from hall-1:";
  for (const NodeId& id : *visited) {
    std::cout << " " << id.str();
  }
  std::cout << "\n";

  // 3. Evolve the topology through a validated mutation batch.
  MutationBatch batch;
  batch.base_generation = generation_one->generation();
  batch.actor = ActorId::parse("example").value();
  batch.source = "example";
  batch.reason = "add a rack to row 1";
  batch.mutation_id = MutationId::parse("example-0001").value();
  batch.recorded_at = "2026-02-01T00:01:00Z";

  NodeRecord rack03 = node("rack-03", NodeKind::Rack, "Rack 03");
  rack03.provenance = provenance("example", "add a rack to row 1");
  batch.mutations.push_back(AddNode{rack03});
  batch.mutations.push_back(AddContainment{contain("row-1", "rack-03")});

  const BatchApplication application = apply_batch_explained(*generation_one, batch, TopologyLimits{});
  std::cout << application.outcome.explain() << "\n";
  if (!application.accepted()) {
    return 1;
  }

  // 4. Compare the two generations deterministically.
  auto changes = diff(*generation_one, application.candidate);
  if (!changes.has_value()) {
    std::cerr << "diff failed: " << changes.error().to_string() << "\n";
    return 1;
  }
  std::cout << changes->to_string();

  // 5. A rejected batch leaves the base generation untouched.
  MutationBatch rejected;
  rejected.base_generation = generation_one->generation();
  rejected.actor = ActorId::parse("example").value();
  rejected.source = "example";
  rejected.reason = "attempt to create a containment cycle";
  rejected.mutation_id = MutationId::parse("example-0002").value();
  rejected.mutations.push_back(MoveNode{NodeId::parse("hall-1").value(), NodeId::parse("rack-01").value(),
                                        provenance("example", "attempt to create a containment cycle")});
  const BatchApplication refused = apply_batch_explained(*generation_one, rejected, TopologyLimits{});
  std::cout << refused.outcome.explain() << "\n";
  std::cout << "base generation still has " << generation_one->stats().nodes << " nodes\n";
  return 0;
}
