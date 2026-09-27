// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared fixtures for the Facility Topology test suite.

#ifndef FACILITY_TOPOLOGY_TESTS_TEST_SUPPORT_HPP
#define FACILITY_TOPOLOGY_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/facility_topology/canonical.hpp"
#include "dccp/facility_topology/diff.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "dccp/facility_topology/store.hpp"
#include "dccp/facility_topology/topology.hpp"
#include "test_framework.hpp"

namespace ftest {

using namespace dccp::facility_topology;

/// Canonical timestamp used by every fixture so documents are reproducible.
inline constexpr std::string_view kFixtureTimestamp = "2026-02-01T00:00:00Z";

inline ProvenanceRecord provenance(std::string_view actor = "tester", std::string_view source = "test",
                                   std::string_view reason = "fixture") {
  ProvenanceRecord record;
  record.actor = ActorId::parse(actor).value();
  record.source = std::string(source);
  record.reason = std::string(reason);
  record.recorded_at = std::string(kFixtureTimestamp);
  return record;
}

inline NodeId node_id(std::string_view text) { return NodeId::parse(text).value(); }
inline DomainId domain_id(std::string_view text) { return DomainId::parse(text).value(); }
inline ActorId actor_id(std::string_view text) { return ActorId::parse(text).value(); }
inline MutationId mutation_id(std::string_view text) { return MutationId::parse(text).value(); }

inline NodeRecord make_node(std::string_view id, NodeKind kind, std::string label = {},
                            std::optional<NodeKind> zone_scope = std::nullopt) {
  NodeRecord record;
  record.id = node_id(id);
  record.kind = kind;
  record.label = std::move(label);
  record.zone_member_kind = zone_scope;
  record.provenance = provenance();
  return record;
}

inline ContainmentEdge physical(std::string_view parent, std::string_view child) {
  ContainmentEdge edge;
  edge.parent = node_id(parent);
  edge.child = node_id(child);
  edge.boundary = BoundaryKind::Physical;
  edge.provenance = provenance();
  return edge;
}

inline ContainmentEdge logical(std::string_view zone, std::string_view member) {
  ContainmentEdge edge;
  edge.parent = node_id(zone);
  edge.child = node_id(member);
  edge.boundary = BoundaryKind::Logical;
  edge.provenance = provenance();
  return edge;
}

inline AdjacencyEdge adjacent(std::string_view first, std::string_view second,
                              AdjacencyKind kind = AdjacencyKind::SharedBoundary) {
  AdjacencyEdge edge;
  edge.first = node_id(first);
  edge.second = node_id(second);
  edge.kind = kind;
  edge.provenance = provenance();
  return edge;
}

inline DomainDeclaration declare_domain(std::string_view id, DomainKind kind, std::string label = {}) {
  DomainDeclaration declaration;
  declaration.id = domain_id(id);
  declaration.kind = kind;
  declaration.label = std::move(label);
  declaration.provenance = provenance();
  return declaration;
}

inline DomainAssociation associate(std::string_view node, std::string_view domain, DomainKind kind) {
  DomainAssociation association;
  association.node = node_id(node);
  association.domain = domain_id(domain);
  association.kind = kind;
  association.provenance = provenance();
  return association;
}

/// A small but complete facility: one facility, one building, two halls, rooms,
/// rows, racks, a zone, adjacency, and power/cooling domain references.
inline TopologySnapshot small_facility(const TopologyGeneration& generation = TopologyGeneration(1)) {
  const std::vector<NodeRecord> nodes = {
      make_node("fac-1", NodeKind::Facility, "Main Facility"),
      make_node("bld-a", NodeKind::Building, "Building A"),
      make_node("hall-1", NodeKind::Hall, "Hall 1"),
      make_node("hall-2", NodeKind::Hall, "Hall 2"),
      make_node("room-1", NodeKind::Room, "Room 1"),
      make_node("row-1", NodeKind::Row, "Row 1"),
      make_node("row-2", NodeKind::Row, "Row 2"),
      make_node("rack-01", NodeKind::Rack, "Rack 01"),
      make_node("rack-02", NodeKind::Rack, "Rack 02"),
      make_node("rack-03", NodeKind::Rack, "Rack 03"),
      make_node("zone-a", NodeKind::Zone, "Zone A", NodeKind::Rack),
  };
  const std::vector<ContainmentEdge> containment = {
      physical("fac-1", "bld-a"),   physical("bld-a", "hall-1"), physical("bld-a", "hall-2"),
      physical("hall-1", "room-1"), physical("room-1", "row-1"), physical("row-1", "rack-01"),
      physical("row-1", "rack-02"), physical("row-2", "rack-03"), physical("hall-2", "row-2"),
      physical("fac-1", "zone-a"),  logical("zone-a", "rack-01"), logical("zone-a", "rack-02"),
  };
  const std::vector<AdjacencyEdge> adjacency = {
      adjacent("rack-01", "rack-02", AdjacencyKind::StructuralNeighbor),
      adjacent("rack-02", "rack-03", AdjacencyKind::SharedBoundary),
      adjacent("hall-1", "hall-2", AdjacencyKind::ServiceAisle),
  };
  const std::vector<DomainDeclaration> domains = {
      declare_domain("pwr-a", DomainKind::Power, "Power feed A"),
      declare_domain("cool-a", DomainKind::Cooling, "Cooling loop A"),
  };
  const std::vector<DomainAssociation> associations = {
      associate("rack-01", "pwr-a", DomainKind::Power),
      associate("rack-02", "pwr-a", DomainKind::Power),
      associate("hall-1", "cool-a", DomainKind::Cooling),
  };
  auto snapshot = assemble_topology(nodes, containment, adjacency, domains, associations, generation);
  FT_REQUIRE(snapshot.has_value());
  return std::move(snapshot).value();
}

/// A unique temporary directory for one test.
class TempDirectory {
 public:
  explicit TempDirectory(std::string_view label) {
    static std::uint64_t counter = 0;
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    path_ = base / ("facility_topology_test_" + std::string(label) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path operator/(std::string_view name) const { return path_ / std::string(name); }

 private:
  std::filesystem::path path_;
};

/// Store configuration with a fixed clock so documents are byte-reproducible.
inline StoreConfig deterministic_config(std::int64_t unix_seconds = 1770000000) {
  static FixedClock clock(unix_seconds);
  StoreConfig config;
  config.clock = &clock;
  return config;
}

inline MutationBatch make_batch(std::string_view id, const TopologyGeneration& base,
                                std::vector<Mutation> mutations) {
  MutationBatch batch;
  batch.base_generation = base;
  batch.actor = actor_id("tester");
  batch.source = "test";
  batch.reason = "unit test";
  batch.mutation_id = mutation_id(id);
  batch.recorded_at = std::string(kFixtureTimestamp);
  batch.mutations = std::move(mutations);
  return batch;
}

inline AddNode add(std::string_view id, NodeKind kind, std::string label = {},
                   std::optional<NodeKind> scope = std::nullopt) {
  return AddNode{make_node(id, kind, std::move(label), scope)};
}

inline AddContainment contain(std::string_view parent, std::string_view child,
                              BoundaryKind boundary = BoundaryKind::Physical) {
  return AddContainment{boundary == BoundaryKind::Physical ? physical(parent, child) : logical(parent, child)};
}

}  // namespace ftest

#endif  // FACILITY_TOPOLOGY_TESTS_TEST_SUPPORT_HPP
