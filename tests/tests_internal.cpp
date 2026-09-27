// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// White-box tests of the internal structural storage.
//
// These tests reach past the public API on purpose: they prove that the
// whole-graph validator really does detect the defects the builder makes
// unreachable, so the invariant is backed by a check and not only by the
// construction rules.

#include <string>
#include <vector>

#include "canonical_internal.hpp"
#include "graph_data.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

GraphData copy_of(const TopologySnapshot& snapshot) {
  const GraphData* data = SnapshotAccess::data(snapshot);
  FT_REQUIRE(data != nullptr);
  return *data;
}

}  // namespace

FT_TEST(internal, validator_detects_an_injected_containment_cycle) {
  const TopologySnapshot snapshot = small_facility();
  GraphData broken = copy_of(snapshot);

  // Close the loop fac-1 -> bld-a -> hall-1 -> room-1 -> row-1 by making the
  // facility a child of the row. The builder cannot produce this; if such
  // storage ever appeared, validation must still refuse it.
  broken.containment[ContainmentKey{node_id("row-1"), node_id("fac-1")}] = ContainmentEdge{
      node_id("row-1"), node_id("fac-1"), BoundaryKind::Physical, provenance()};

  const ValidationReport report = validate_graph(broken, TopologyLimits{});
  FT_CHECK(!report.valid);
  FT_CHECK(report.count(ErrorCode::ContainmentCycle) >= 1);
  FT_CHECK(report.to_string().find("CONTAINMENT_CYCLE") != std::string::npos);
}

FT_TEST(internal, validator_detects_a_two_node_cycle) {
  const TopologySnapshot snapshot = small_facility();
  GraphData broken = copy_of(snapshot);
  broken.containment[ContainmentKey{node_id("row-2"), node_id("rack-03")}] = ContainmentEdge{
      node_id("row-2"), node_id("rack-03"), BoundaryKind::Physical, provenance()};
  broken.containment[ContainmentKey{node_id("rack-03"), node_id("row-2")}] = ContainmentEdge{
      node_id("rack-03"), node_id("row-2"), BoundaryKind::Physical, provenance()};

  const ValidationReport report = validate_graph(broken, TopologyLimits{});
  FT_CHECK(!report.valid);
  FT_CHECK(report.count(ErrorCode::ContainmentCycle) >= 1);
}

FT_TEST(internal, validator_detects_index_drift) {
  const TopologySnapshot snapshot = small_facility();

  {
    GraphData broken = copy_of(snapshot);
    broken.physical_parent[node_id("rack-03")] = node_id("row-1");
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InternalError) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.children[node_id("row-1")].insert(node_id("rack-03"));
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InternalError) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.kind_index[NodeKind::Rack].erase(node_id("rack-01"));
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InternalError) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.neighbor_index[node_id("rack-01")].insert(node_id("rack-03"));
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InternalError) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.domain_members[domain_id("pwr-a")].insert(node_id("rack-03"));
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InternalError) >= 1);
  }
}

FT_TEST(internal, validator_detects_missing_and_dangling_endpoints) {
  const TopologySnapshot snapshot = small_facility();
  {
    GraphData broken = copy_of(snapshot);
    broken.containment[ContainmentKey{node_id("row-1"), node_id("ghost")}] =
        ContainmentEdge{node_id("row-1"), node_id("ghost"), BoundaryKind::Physical, provenance()};
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::MissingEndpoint) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.adjacency[make_adjacency_key(node_id("rack-01"), node_id("ghost"))] =
        AdjacencyEdge{node_id("rack-01"), node_id("ghost"), AdjacencyKind::SharedBoundary, provenance()};
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::MissingEndpoint) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.associations[AssociationKey{node_id("rack-03"), domain_id("cool-a")}] =
        DomainAssociation{node_id("rack-03"), domain_id("cool-a"), DomainKind::Cooling, provenance()};
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InternalError) >= 1);
  }
}

FT_TEST(internal, validator_detects_domain_and_scope_violations) {
  const TopologySnapshot snapshot = small_facility();
  {
    GraphData broken = copy_of(snapshot);
    broken.associations[AssociationKey{node_id("rack-02"), domain_id("cool-a")}] =
        DomainAssociation{node_id("rack-02"), domain_id("cool-a"), DomainKind::Power, provenance()};
    broken.node_associations[node_id("rack-02")].insert(AssociationKey{node_id("rack-02"), domain_id("cool-a")});
    broken.domain_members[domain_id("cool-a")].insert(node_id("rack-02"));
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::IdentityConflict) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.containment[ContainmentKey{node_id("zone-a"), node_id("row-1")}] =
        ContainmentEdge{node_id("zone-a"), node_id("row-1"), BoundaryKind::Logical, provenance()};
    broken.logical_parent[node_id("row-1")] = node_id("zone-a");
    broken.members[node_id("zone-a")].insert(node_id("row-1"));
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::ZoneTargetKindInvalid) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.nodes.at(node_id("zone-a")).zone_member_kind = std::nullopt;
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::MissingField) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.nodes.at(node_id("rack-01")).zone_member_kind = NodeKind::Rack;
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::InvalidArgument) >= 1);
  }
}

FT_TEST(internal, validator_enforces_budgets_and_timestamps) {
  const TopologySnapshot snapshot = small_facility();
  {
    GraphData broken = copy_of(snapshot);
    TopologyLimits limits = TopologyLimits{};
    limits.max_nodes = 2;
    limits.max_containment_edges = 2;
    const ValidationReport report = validate_graph(broken, limits);
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::LimitExceeded) >= 2);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.nodes.at(node_id("rack-01")).provenance.recorded_at = "not a timestamp";
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::MalformedRecord) >= 1);
  }
  {
    GraphData broken = copy_of(snapshot);
    broken.nodes.at(node_id("rack-01")).provenance.actor = ActorId();
    const ValidationReport report = validate_graph(broken, TopologyLimits{});
    FT_CHECK(!report.valid);
    FT_CHECK(report.count(ErrorCode::MissingField) >= 1);
  }
}

FT_TEST(internal, structure_digest_is_content_addressed) {
  const TopologySnapshot first = small_facility(TopologyGeneration(1));
  const TopologySnapshot second = small_facility(TopologyGeneration(7));
  const GraphData* lhs = SnapshotAccess::data(first);
  const GraphData* rhs = SnapshotAccess::data(second);
  FT_REQUIRE(lhs != nullptr);
  FT_REQUIRE(rhs != nullptr);
  FT_CHECK(digest_equal(structure_digest(*lhs), structure_digest(*rhs)));

  GraphData changed = *lhs;
  changed.nodes.at(node_id("rack-01")).label = "different";
  FT_CHECK(!digest_equal(structure_digest(*lhs), structure_digest(changed)));

  // Provenance is deliberately not part of the content digest.
  GraphData reprovenanced = *lhs;
  reprovenanced.nodes.at(node_id("rack-01")).provenance.actor = actor_id("someone-else");
  reprovenanced.nodes.at(node_id("rack-01")).provenance.recorded_at = "2030-01-01T00:00:00Z";
  FT_CHECK(digest_equal(structure_digest(*lhs), structure_digest(reprovenanced)));
  FT_CHECK(lhs->same_structure_as(reprovenanced));
}

FT_TEST(internal, snapshots_share_storage_and_never_mutate_it) {
  const TopologySnapshot original = small_facility();
  const TopologySnapshot copy = original;
  FT_CHECK(SnapshotAccess::data(original) == SnapshotAccess::data(copy));

  auto builder = TopologyBuilder::from_snapshot(original);
  FT_REQUIRE(builder.has_value());
  FT_REQUIRE(builder->remove_adjacency(node_id("rack-01"), node_id("rack-02"),
                                       AdjacencyKind::StructuralNeighbor)
                 .has_value());
  auto rebuilt = builder->build(TopologyGeneration(2));
  FT_REQUIRE(rebuilt.has_value());
  FT_CHECK(SnapshotAccess::data(*rebuilt) != SnapshotAccess::data(original));
  FT_CHECK_EQ(SnapshotAccess::data(original)->adjacency.size(), std::size_t{3});
  FT_CHECK_EQ(SnapshotAccess::data(*rebuilt)->adjacency.size(), std::size_t{2});
}

FT_TEST(internal, bounded_ancestry_walk_terminates_on_a_cycle) {
  const TopologySnapshot snapshot = small_facility();
  GraphData broken = copy_of(snapshot);
  // A pure cycle with no root at all: the bounded walk must terminate, and no
  // facility may be attributed to a node that has no facility root.
  broken.physical_parent[node_id("fac-1")] = node_id("rack-01");
  const std::vector<NodeId> chain = broken.ancestry(node_id("fac-1"));
  FT_CHECK_EQ(chain.size(), kMaxAncestryWalk);
  FT_CHECK(std::find(chain.begin(), chain.end(), node_id("fac-1")) != chain.end());
  FT_CHECK(!broken.depth_of(node_id("fac-1")).has_value());
  FT_CHECK(!broken.facility_of(node_id("fac-1")).has_value());
  FT_CHECK(!broken.facility_of(node_id("rack-01")).has_value());
  FT_CHECK(!broken.facility_of(node_id("rack-02")).has_value());
  FT_CHECK(broken.facility_of(node_id("nope")) == std::nullopt);
}
