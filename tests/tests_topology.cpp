// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/facility_topology/topology.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

std::vector<std::string> names(const std::vector<NodeId>& ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const NodeId& id : ids) {
    out.push_back(id.str());
  }
  return out;
}

}  // namespace

FT_TEST(topology, assembles_a_valid_facility) {
  const TopologySnapshot snapshot = small_facility();
  FT_CHECK(snapshot.valid());
  FT_CHECK_EQ(snapshot.generation().value(), std::uint64_t{1});
  const TopologyStats stats = snapshot.stats();
  FT_CHECK_EQ(stats.nodes, std::size_t{11});
  FT_CHECK_EQ(stats.facilities, std::size_t{1});
  FT_CHECK_EQ(stats.physical_containment, std::size_t{10});
  FT_CHECK_EQ(stats.logical_containment, std::size_t{2});
  FT_CHECK_EQ(stats.adjacency, std::size_t{3});
  FT_CHECK_EQ(stats.domains, std::size_t{2});
  FT_CHECK_EQ(stats.associations, std::size_t{3});
  FT_CHECK_EQ(static_cast<int>(stats.max_depth), 5);
}

FT_TEST(topology, query_surface_is_canonical_and_deterministic) {
  const TopologySnapshot snapshot = small_facility();
  FT_CHECK_EQ(names(snapshot.facilities()), std::vector<std::string>{"fac-1"});
  FT_CHECK_EQ(names(snapshot.children(node_id("row-1"))), (std::vector<std::string>{"rack-01", "rack-02"}));
  FT_CHECK_EQ(names(snapshot.zone_members(node_id("zone-a"))), (std::vector<std::string>{"rack-01", "rack-02"}));
  FT_CHECK_EQ(names(snapshot.ancestry(node_id("rack-01"))),
              (std::vector<std::string>{"fac-1", "bld-a", "hall-1", "room-1", "row-1"}));
  FT_CHECK_EQ(names(snapshot.neighbors(node_id("rack-02"))), (std::vector<std::string>{"rack-01", "rack-03"}));
  FT_CHECK_EQ(names(snapshot.nodes_in_domain(domain_id("pwr-a"))),
              (std::vector<std::string>{"rack-01", "rack-02"}));
  FT_CHECK_EQ(names(snapshot.nodes_of_kind(NodeKind::Rack)),
              (std::vector<std::string>{"rack-01", "rack-02", "rack-03"}));

  FT_CHECK(snapshot.physical_parent(node_id("row-1")).value() == node_id("room-1"));
  FT_CHECK(!snapshot.physical_parent(node_id("fac-1")).has_value());
  FT_CHECK(snapshot.zone_of(node_id("rack-01")).value() == node_id("zone-a"));
  FT_CHECK(!snapshot.zone_of(node_id("rack-03")).has_value());
  FT_CHECK(snapshot.facility_of(node_id("rack-03")).value() == node_id("fac-1"));
  FT_CHECK_EQ(snapshot.depth_of(node_id("rack-01")).value(), std::uint32_t{5});
  FT_CHECK_EQ(snapshot.depth_of(node_id("fac-1")).value(), std::uint32_t{0});
  FT_CHECK(snapshot.find_node(node_id("fac-1")) != nullptr);
  FT_CHECK(snapshot.find_node(node_id("nope")) == nullptr);
  FT_CHECK(!snapshot.contains_node(node_id("nope")));

  FT_CHECK(snapshot.is_descendant_of(node_id("rack-01"), node_id("fac-1")).value());
  FT_CHECK(!snapshot.is_descendant_of(node_id("fac-1"), node_id("rack-01")).value());
  FT_CHECK(!snapshot.is_descendant_of(node_id("rack-01"), node_id("rack-01")).value());
  FT_CHECK(snapshot.lowest_common_ancestor(node_id("rack-01"), node_id("rack-03")).value().value() ==
           node_id("bld-a"));
  FT_CHECK(snapshot.lowest_common_ancestor(node_id("rack-01"), node_id("rack-02")).value().value() ==
           node_id("row-1"));
  FT_CHECK_ERROR(snapshot.is_descendant_of(node_id("nope"), node_id("fac-1")), ErrorCode::NotFound);

  const std::vector<NodeRecord> nodes = snapshot.nodes();
  FT_REQUIRE(nodes.size() == 11);
  for (std::size_t index = 1; index < nodes.size(); ++index) {
    FT_CHECK(nodes[index - 1].id < nodes[index].id);
  }
  const std::vector<ContainmentEdge> edges = snapshot.containment_edges();
  for (std::size_t index = 1; index < edges.size(); ++index) {
    const bool ordered = edges[index - 1].parent < edges[index].parent ||
                         (edges[index - 1].parent == edges[index].parent &&
                          edges[index - 1].child < edges[index].child);
    FT_CHECK(ordered);
  }
  const std::vector<AdjacencyEdge> adjacency = snapshot.adjacency_edges();
  for (std::size_t index = 1; index < adjacency.size(); ++index) {
    const bool ordered = adjacency[index - 1].first < adjacency[index].first ||
                         (adjacency[index - 1].first == adjacency[index].first &&
                          adjacency[index - 1].second < adjacency[index].second);
    FT_CHECK(ordered);
    FT_CHECK(adjacency[index].first < adjacency[index].second);
  }
}

FT_TEST(topology, traversal_orders_are_exact_and_bounded) {
  const TopologySnapshot snapshot = small_facility();
  TraversalLimits bounds;
  bounds.max_depth = 32;
  bounds.max_nodes = 1024;

  auto bfs = snapshot.traverse(node_id("bld-a"), TraversalOrder::BreadthFirst, bounds);
  FT_REQUIRE(bfs.has_value());
  FT_CHECK_EQ(names(*bfs), (std::vector<std::string>{"hall-1", "hall-2", "room-1", "row-2", "row-1", "rack-03",
                                                     "rack-01", "rack-02"}));

  auto dfs = snapshot.traverse(node_id("bld-a"), TraversalOrder::DepthFirstPreOrder, bounds);
  FT_REQUIRE(dfs.has_value());
  FT_CHECK_EQ(names(*dfs), (std::vector<std::string>{"hall-1", "room-1", "row-1", "rack-01", "rack-02", "hall-2",
                                                     "row-2", "rack-03"}));

  TraversalLimits shallow;
  shallow.max_depth = 1;
  shallow.max_nodes = 1024;
  // A traversal is complete or it fails: a bound below the real depth is
  // reported rather than silently truncating the result.
  FT_CHECK_ERROR(snapshot.traverse(node_id("hall-1"), TraversalOrder::BreadthFirst, shallow),
                 ErrorCode::TraversalDepthExceeded);
  auto leaf_only = snapshot.traverse(node_id("rack-01"), TraversalOrder::BreadthFirst, shallow);
  FT_REQUIRE(leaf_only.has_value());
  FT_CHECK(leaf_only->empty());

  auto too_shallow = snapshot.traverse(node_id("fac-1"), TraversalOrder::BreadthFirst, shallow);
  FT_CHECK_ERROR(too_shallow, ErrorCode::TraversalDepthExceeded);

  TraversalLimits tiny;
  tiny.max_depth = 32;
  tiny.max_nodes = 2;
  auto too_many = snapshot.traverse(node_id("fac-1"), TraversalOrder::BreadthFirst, tiny);
  FT_CHECK_ERROR(too_many, ErrorCode::LimitExceeded);

  auto missing = snapshot.traverse(node_id("nope"), TraversalOrder::BreadthFirst, bounds);
  FT_CHECK_ERROR(missing, ErrorCode::NotFound);

  auto leaf = snapshot.traverse(node_id("rack-01"), TraversalOrder::BreadthFirst, bounds);
  FT_REQUIRE(leaf.has_value());
  FT_CHECK(leaf->empty());

  FT_CHECK(traversal_order_parse("bfs").value() == TraversalOrder::BreadthFirst);
  FT_CHECK(traversal_order_parse("dfs").value() == TraversalOrder::DepthFirstPreOrder);
  FT_CHECK_ERROR(traversal_order_parse("spiral"), ErrorCode::UnknownEnumToken);
}

FT_TEST(topology, builder_enforces_parent_child_rules) {
  TopologyBuilder builder;
  FT_CHECK_OK(builder.add_node(make_node("fac-1", NodeKind::Facility)));
  FT_CHECK_OK(builder.add_node(make_node("hall-1", NodeKind::Hall)));
  FT_CHECK_OK(builder.add_node(make_node("row-1", NodeKind::Row)));
  FT_CHECK_OK(builder.add_node(make_node("rack-1", NodeKind::Rack)));

  FT_CHECK_ERROR(builder.add_containment(physical("hall-1", "fac-1")), ErrorCode::FacilityMustBeRoot);
  FT_CHECK_ERROR(builder.add_containment(physical("rack-1", "row-1")), ErrorCode::InvalidParentKind);
  FT_CHECK_ERROR(builder.add_containment(physical("row-1", "hall-1")), ErrorCode::InvalidParentKind);
  FT_CHECK_ERROR(builder.add_containment(physical("row-1", "row-1")), ErrorCode::SelfEdge);
  FT_CHECK_ERROR(builder.add_containment(logical("row-1", "rack-1")), ErrorCode::InvalidParentKind);

  FT_CHECK_OK(builder.add_containment(physical("fac-1", "hall-1")));
  FT_CHECK_OK(builder.add_containment(physical("hall-1", "row-1")));
  FT_CHECK_OK(builder.add_containment(physical("row-1", "rack-1")));
  FT_CHECK_ERROR(builder.add_containment(physical("fac-1", "rack-1")), ErrorCode::IdentityConflict);
  FT_CHECK_ERROR(builder.add_containment(physical("row-1", "rack-1")), ErrorCode::AlreadyPresent);
  FT_CHECK_OK(builder.build(TopologyGeneration(1)));
}

FT_TEST(topology, builder_rejects_duplicate_and_malformed_nodes) {
  TopologyBuilder builder;
  FT_CHECK_OK(builder.add_node(make_node("fac-1", NodeKind::Facility)));
  FT_CHECK_ERROR(builder.add_node(make_node("fac-1", NodeKind::Facility)), ErrorCode::AlreadyPresent);

  // Identities can only be built by parsing, so a malformed identity cannot
  // reach the builder at all; an absent one still must be rejected.
  NodeRecord empty_identity;
  empty_identity.kind = NodeKind::Rack;
  empty_identity.provenance = provenance();
  FT_CHECK_ERROR(builder.add_node(empty_identity), ErrorCode::MissingField);

  NodeRecord no_scope = make_node("zone-1", NodeKind::Zone);
  FT_CHECK_ERROR(builder.add_node(no_scope), ErrorCode::MissingField);

  NodeRecord bad_scope = make_node("zone-2", NodeKind::Zone, "zone", NodeKind::Facility);
  FT_CHECK_ERROR(builder.add_node(bad_scope), ErrorCode::ZoneTargetKindInvalid);

  NodeRecord stray_scope = make_node("rack-1", NodeKind::Rack, "rack", NodeKind::Rack);
  FT_CHECK_ERROR(builder.add_node(stray_scope), ErrorCode::InvalidArgument);

  NodeRecord bad_label = make_node("rack-2", NodeKind::Rack, "line\nbreak");
  FT_CHECK_ERROR(builder.add_node(bad_label), ErrorCode::MalformedRecord);

  NodeRecord bad_provenance = make_node("rack-3", NodeKind::Rack);
  bad_provenance.provenance.actor = ActorId();
  FT_CHECK_ERROR(builder.add_node(bad_provenance), ErrorCode::MissingField);
}

FT_TEST(topology, containment_cycles_are_impossible_to_create) {
  TopologyBuilder builder;
  FT_CHECK_OK(builder.add_node(make_node("fac-1", NodeKind::Facility)));
  FT_CHECK_OK(builder.add_node(make_node("hall-1", NodeKind::Hall)));
  FT_CHECK_OK(builder.add_node(make_node("row-1", NodeKind::Row)));
  FT_CHECK_OK(builder.add_containment(physical("fac-1", "hall-1")));
  FT_CHECK_OK(builder.add_containment(physical("hall-1", "row-1")));

  // Moving a container under its own descendant must be rejected, not applied.
  FT_CHECK_ERROR(builder.move_node(node_id("hall-1"), node_id("row-1"), provenance()), ErrorCode::InvalidParentKind);
  FT_CHECK_ERROR(builder.move_node(node_id("fac-1"), node_id("hall-1"), provenance()), ErrorCode::FacilityMustBeRoot);
  FT_CHECK_ERROR(builder.move_node(node_id("row-1"), node_id("row-1"), provenance()), ErrorCode::ContainmentCycle);
  FT_CHECK_ERROR(builder.move_node(node_id("row-1"), node_id("nope"), provenance()), ErrorCode::NotFound);

  const ValidationReport report = builder.validate();
  FT_CHECK(report.valid);
  auto snapshot = builder.build(TopologyGeneration(1));
  FT_REQUIRE(snapshot.has_value());
  FT_CHECK(names(snapshot->ancestry(node_id("row-1"))) == (std::vector<std::string>{"fac-1", "hall-1"}));
}

FT_TEST(topology, moves_preserve_identity_and_rewrite_containment) {
  TopologyBuilder builder = TopologyBuilder::from_snapshot(small_facility()).value();
  FT_CHECK_OK(builder.move_node(node_id("rack-03"), node_id("row-1"), provenance("mover", "test", "rebalance")));
  FT_CHECK(builder.physical_parent(node_id("rack-03")).value() == node_id("row-1"));
  FT_CHECK_EQ(names(builder.children(node_id("row-2"))).size(), std::size_t{0});
  FT_CHECK(builder.find_node(node_id("rack-03")) != nullptr);
  FT_CHECK_EQ(builder.find_node(node_id("rack-03"))->label, std::string("Rack 03"));

  auto snapshot = builder.build(TopologyGeneration(2));
  FT_REQUIRE(snapshot.has_value());
  const std::vector<ContainmentEdge> edges = snapshot->containment_edges();
  const ContainmentEdge* moved = nullptr;
  for (const ContainmentEdge& edge : edges) {
    if (edge.child == node_id("rack-03")) {
      moved = &edge;
    }
  }
  FT_REQUIRE(moved != nullptr);
  FT_CHECK(moved->parent == node_id("row-1"));
  FT_CHECK_EQ(moved->provenance.source, std::string("test"));
  FT_CHECK_EQ(moved->provenance.reason, std::string("rebalance"));
}

FT_TEST(topology, removal_requires_a_detached_node) {
  TopologyBuilder builder = TopologyBuilder::from_snapshot(small_facility()).value();
  FT_CHECK_ERROR(builder.remove_node(node_id("row-1")), ErrorCode::NodeHasChildren);
  FT_CHECK_ERROR(builder.remove_node(node_id("zone-a")), ErrorCode::NodeHasChildren);
  FT_CHECK_ERROR(builder.remove_node(node_id("rack-01")), ErrorCode::NodeReferenced);
  FT_CHECK_ERROR(builder.remove_node(node_id("nope")), ErrorCode::NotFound);

  FT_CHECK_OK(builder.remove_adjacency(node_id("rack-01"), node_id("rack-02"), AdjacencyKind::StructuralNeighbor));
  FT_CHECK_OK(builder.remove_association(node_id("rack-01"), domain_id("pwr-a")));
  FT_CHECK_OK(builder.remove_containment(node_id("zone-a"), node_id("rack-01")));
  FT_CHECK_OK(builder.remove_node(node_id("rack-01")));
  FT_CHECK(!builder.contains_node(node_id("rack-01")));
  FT_CHECK(!builder.physical_parent(node_id("rack-01")).has_value());
  FT_CHECK(builder.build(TopologyGeneration(2)).has_value());

  FT_CHECK_ERROR(builder.remove_containment(node_id("row-1"), node_id("rack-01")), ErrorCode::NotFound);
  FT_CHECK_ERROR(builder.remove_adjacency(node_id("rack-01"), node_id("rack-02"), AdjacencyKind::SharedBoundary),
                 ErrorCode::NotFound);
  FT_CHECK_ERROR(builder.remove_adjacency(node_id("rack-02"), node_id("rack-03"), AdjacencyKind::StructuralNeighbor),
                 ErrorCode::NotFound);
}

FT_TEST(topology, zone_membership_rules) {
  TopologyBuilder builder = TopologyBuilder::from_snapshot(small_facility()).value();
  // zone-a groups racks, so a third rack joins it while a row does not.
  FT_CHECK_OK(builder.add_containment(logical("zone-a", "rack-03")));
  FT_CHECK_ERROR(builder.add_containment(logical("zone-a", "row-2")), ErrorCode::ZoneTargetKindInvalid);
  FT_CHECK_ERROR(builder.add_containment(logical("zone-a", "fac-1")), ErrorCode::ZoneMemberKindInvalid);
  FT_CHECK_ERROR(builder.add_containment(logical("zone-a", "rack-02")), ErrorCode::AlreadyPresent);

  FT_CHECK_OK(builder.add_node(make_node("zone-rows", NodeKind::Zone, "Row zone", NodeKind::Row)));
  FT_CHECK_OK(builder.add_containment(physical("fac-1", "zone-rows")));
  FT_CHECK_OK(builder.add_containment(logical("zone-rows", "row-1")));
  FT_CHECK_ERROR(builder.add_containment(logical("zone-rows", "rack-03")), ErrorCode::ZoneTargetKindInvalid);
  FT_CHECK_ERROR(builder.add_containment(logical("zone-rows", "hall-1")), ErrorCode::ZoneTargetKindInvalid);

  FT_CHECK_OK(builder.add_node(make_node("zone-b", NodeKind::Zone, "Second zone", NodeKind::Row)));
  FT_CHECK_OK(builder.add_containment(physical("fac-1", "zone-b")));
  FT_CHECK_ERROR(builder.add_containment(logical("zone-b", "row-1")), ErrorCode::ZoneConflict);
  FT_CHECK_ERROR(builder.add_containment(physical("row-1", "zone-b")), ErrorCode::InvalidParentKind);

  // A zone may not group a node that belongs to another facility.
  FT_CHECK_OK(builder.add_node(make_node("fac-2", NodeKind::Facility)));
  FT_CHECK_OK(builder.add_node(make_node("rack-99", NodeKind::Rack)));
  FT_CHECK_OK(builder.add_containment(physical("fac-2", "rack-99")));
  FT_CHECK_ERROR(builder.add_containment(logical("zone-a", "rack-99")), ErrorCode::ZoneConflict);
}

FT_TEST(topology, adjacency_rules) {
  TopologyBuilder builder = TopologyBuilder::from_snapshot(small_facility()).value();
  FT_CHECK_ERROR(builder.add_adjacency(adjacent("rack-01", "rack-01")), ErrorCode::SelfEdge);
  FT_CHECK_ERROR(builder.add_adjacency(adjacent("rack-01", "rack-02")), ErrorCode::DuplicateEdge);
  FT_CHECK_ERROR(builder.add_adjacency(adjacent("rack-01", "rack-02", AdjacencyKind::ServiceAisle)),
                 ErrorCode::DuplicateEdge);
  FT_CHECK_ERROR(builder.add_adjacency(adjacent("rack-01", "nope")), ErrorCode::NotFound);
  FT_CHECK_ERROR(builder.add_adjacency(adjacent("fac-1", "hall-1")), ErrorCode::InvalidArgument);
  FT_CHECK_ERROR(builder.add_adjacency(adjacent("zone-a", "rack-03")), ErrorCode::InvalidArgument);
  FT_CHECK_OK(builder.add_adjacency(adjacent("rack-01", "rack-03", AdjacencyKind::ServiceAisle)));
  FT_CHECK_OK(builder.add_adjacency(adjacent("row-1", "row-2", AdjacencyKind::ServiceAisle)));
  // Adjacency is a relation between the endpoints only: a row is adjacent to
  // the other row, not to the racks that happen to sit inside it.
  FT_CHECK(names(builder.neighbors(node_id("row-1"))) == (std::vector<std::string>{"row-2"}));
  FT_CHECK(names(builder.neighbors(node_id("rack-01"))) ==
           (std::vector<std::string>{"rack-02", "rack-03"}));
  FT_CHECK(names(builder.neighbors(node_id("rack-03"))) ==
           (std::vector<std::string>{"rack-01", "rack-02"}));
  FT_CHECK(names(builder.children(node_id("row-1"))) == (std::vector<std::string>{"rack-01", "rack-02"}));
}

FT_TEST(topology, adjacency_across_facilities_is_rejected) {
  std::vector<NodeRecord> nodes = {make_node("fac-1", NodeKind::Facility), make_node("rack-1", NodeKind::Rack),
                                   make_node("fac-2", NodeKind::Facility), make_node("rack-2", NodeKind::Rack)};
  std::vector<ContainmentEdge> containment = {physical("fac-1", "rack-1"), physical("fac-2", "rack-2")};
  auto snapshot = assemble_topology(nodes, containment, {adjacent("rack-1", "rack-2")}, {}, {}, TopologyGeneration(1));
  FT_CHECK_ERROR(snapshot, ErrorCode::AdjacencyCrossFacility);
}

FT_TEST(topology, domain_associations_require_a_matching_declaration) {
  TopologyBuilder builder = TopologyBuilder::from_snapshot(small_facility()).value();
  FT_CHECK_ERROR(builder.add_association(associate("rack-03", "pwr-b", DomainKind::Power)), ErrorCode::NotFound);
  FT_CHECK_ERROR(builder.add_association(associate("rack-01", "pwr-a", DomainKind::Power)),
                 ErrorCode::AlreadyPresent);
  FT_CHECK_ERROR(builder.add_association(associate("rack-01", "cool-a", DomainKind::Power)),
                 ErrorCode::IdentityConflict);
  FT_CHECK_ERROR(builder.add_association(associate("nope", "pwr-a", DomainKind::Power)), ErrorCode::NotFound);

  FT_CHECK_ERROR(builder.retire_domain(domain_id("pwr-a")), ErrorCode::NodeReferenced);
  FT_CHECK_ERROR(builder.retire_domain(domain_id("nope")), ErrorCode::NotFound);
  FT_CHECK_ERROR(builder.declare_domain(declare_domain("pwr-a", DomainKind::Cooling)), ErrorCode::IdentityConflict);
  FT_CHECK_ERROR(builder.declare_domain(declare_domain("pwr-a", DomainKind::Power)), ErrorCode::AlreadyPresent);
  FT_CHECK_OK(builder.remove_association(node_id("rack-01"), domain_id("pwr-a")));
  FT_CHECK_OK(builder.remove_association(node_id("rack-02"), domain_id("pwr-a")));
  FT_CHECK_OK(builder.retire_domain(domain_id("pwr-a")));
  FT_CHECK(builder.find_domain(domain_id("pwr-a")) == nullptr);
}

FT_TEST(topology, whole_graph_validation_catches_structural_defects) {
  // A generation with no facility root is not publishable.
  auto empty = assemble_topology({make_node("rack-1", NodeKind::Rack)}, {physical("fac-1", "rack-1")}, {}, {}, {},
                                 TopologyGeneration(1));
  FT_CHECK_ERROR(empty, ErrorCode::NotFound);

  auto no_facility = assemble_topology({make_node("rack-1", NodeKind::Rack)}, {}, {}, {}, {}, TopologyGeneration(1));
  FT_CHECK_ERROR(no_facility, ErrorCode::NonFacilityMustBeContained);

  auto only_facility = assemble_topology({make_node("fac-1", NodeKind::Facility)}, {}, {}, {}, {},
                                         TopologyGeneration(1));
  FT_CHECK(only_facility.has_value());
  FT_CHECK(only_facility->stats().facilities == 1);
}

FT_TEST(topology, labels_and_provenance_survive_a_build) {
  TopologyBuilder builder = TopologyBuilder::from_snapshot(small_facility()).value();
  FT_CHECK_OK(builder.set_node_label(node_id("rack-01"), "Rack 01 (renamed)", provenance("ops", "cli", "rename")));
  auto snapshot = builder.build(TopologyGeneration(2));
  FT_REQUIRE(snapshot.has_value());
  const NodeRecord* record = snapshot->find_node(node_id("rack-01"));
  FT_REQUIRE(record != nullptr);
  FT_CHECK_EQ(record->label, std::string("Rack 01 (renamed)"));
  FT_CHECK_EQ(record->provenance.actor.str(), std::string("ops"));
  FT_CHECK_ERROR(builder.set_node_label(node_id("rack-01"), " leading", provenance()), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(builder.set_node_label(node_id("nope"), "x", provenance()), ErrorCode::NotFound);
}

FT_TEST(topology, snapshots_are_immutable_values) {
  const TopologySnapshot original = small_facility();
  TopologySnapshot copy = original;
  FT_CHECK(copy.same_structure_as(original));
  FT_CHECK_EQ(names(copy.children(node_id("row-1"))), names(original.children(node_id("row-1"))));

  TopologyBuilder builder = TopologyBuilder::from_snapshot(original).value();
  FT_CHECK_OK(builder.remove_adjacency(node_id("rack-01"), node_id("rack-02"), AdjacencyKind::StructuralNeighbor));
  auto next = builder.build(TopologyGeneration(2));
  FT_REQUIRE(next.has_value());
  FT_CHECK(!next->same_structure_as(original));
  // The original snapshot is untouched by the builder that was derived from it.
  FT_CHECK_EQ(original.stats().adjacency, std::size_t{3});
  FT_CHECK_EQ(original.generation().value(), std::uint64_t{1});
  FT_CHECK_EQ(copy.stats().adjacency, std::size_t{3});
  FT_CHECK(original.content_digest() == copy.content_digest());
}

FT_TEST(topology, empty_snapshot_is_safe_to_query) {
  const TopologySnapshot empty;
  FT_CHECK(!empty.valid());
  FT_CHECK(!empty.generation().published());
  FT_CHECK(empty.nodes().empty());
  FT_CHECK(empty.facilities().empty());
  FT_CHECK(empty.find_node(node_id("fac-1")) == nullptr);
  FT_CHECK(!empty.physical_parent(node_id("fac-1")).has_value());
  FT_CHECK(empty.stats().nodes == 0);
  FT_CHECK(digest_is_zero(empty.content_digest()));
  FT_CHECK_ERROR(empty.traverse(node_id("fac-1"), TraversalOrder::BreadthFirst, TraversalLimits{}),
                 ErrorCode::NotInitialized);
  FT_CHECK_ERROR(serialize_generation(empty, GenerationManifest{}), ErrorCode::NotInitialized);
}

FT_TEST(topology, generation_counters_are_monotonic_across_generations) {
  TopologySnapshot snapshot = small_facility(TopologyGeneration(41));
  FT_CHECK_EQ(snapshot.generation().value(), std::uint64_t{41});
  auto next = snapshot.generation().next();
  FT_REQUIRE(next.has_value());
  FT_CHECK_EQ(next->value(), std::uint64_t{42});
}
