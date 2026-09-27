// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Seeded property tests.
//
// Every case is generated from the run seed (default 20260201, overridable
// with --seed=N) and the seed is printed with any failure, so a failing case is
// reproducible exactly.

#include <algorithm>
#include <cstddef>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_topology/canonical.hpp"
#include "dccp/facility_topology/diff.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "test_rng.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

const std::vector<NodeKind> kPhysicalKinds = {NodeKind::Building, NodeKind::Hall, NodeKind::Room, NodeKind::Row,
                                              NodeKind::Rack};
const std::vector<NodeKind> kZonableKinds = {NodeKind::Hall, NodeKind::Room, NodeKind::Row, NodeKind::Rack};

/// A synthetically generated facility plan. The topology it describes is
/// synthetic: it is not derived from any real site.
struct GraphPlan {
  std::vector<NodeRecord> nodes;
  std::vector<ContainmentEdge> containment;
  std::vector<AdjacencyEdge> adjacency;
  std::vector<DomainDeclaration> domains;
  std::vector<DomainAssociation> associations;
};

GraphPlan plan_facility(Rng& rng, std::size_t target_nodes) {
  GraphPlan plan;
  plan.nodes.push_back(make_node("fac-1", NodeKind::Facility, "Synthetic facility"));
  std::vector<std::pair<std::string, NodeKind>> placeable = {{"fac-1", NodeKind::Facility}};
  std::size_t counter = 0;
  std::size_t guard = 0;
  while (plan.nodes.size() < target_nodes && guard++ < target_nodes * 8) {
    std::vector<std::size_t> parents;
    for (std::size_t index = 0; index < placeable.size(); ++index) {
      if (node_kind_schema(placeable[index].second).allowed_child_kinds != 0) {
        parents.push_back(index);
      }
    }
    if (parents.empty()) {
      break;
    }
    const std::pair<std::string, NodeKind>& parent = placeable[parents[rng.index(parents.size())]];

    std::vector<NodeKind> allowed;
    for (const NodeKind kind : kPhysicalKinds) {
      if (schema_allows_physical_child(parent.second, kind)) {
        allowed.push_back(kind);
      }
    }
    if (allowed.empty()) {
      // A leaf: drop it from the placeable list so the loop makes progress.
      placeable.erase(std::remove_if(placeable.begin(), placeable.end(),
                                     [&parent](const std::pair<std::string, NodeKind>& entry) {
                                       return entry.first == parent.first;
                                     }),
                      placeable.end());
      continue;
    }
    const NodeKind child_kind = allowed[rng.index(allowed.size())];
    const std::string id = "n" + std::to_string(counter++);
    plan.nodes.push_back(make_node(id, child_kind, "Synthetic " + id));
    plan.containment.push_back(physical(parent.first, id));
    placeable.emplace_back(id, child_kind);
  }

  // Zones group existing nodes of one declared kind. Each zone has a distinct
  // scope: a node may belong to at most one zone, so two zones over the same
  // kind would contend for the same members.
  std::vector<NodeKind> scopes = kZonableKinds;
  std::shuffle(scopes.begin(), scopes.end(), std::mt19937(static_cast<std::mt19937::result_type>(rng.next())));
  const std::size_t zone_count = rng.below(3);
  for (std::size_t index = 0; index < zone_count && index < scopes.size(); ++index) {
    const NodeKind scope = scopes[index];
    std::vector<std::string> members;
    for (const NodeRecord& node : plan.nodes) {
      if (node.kind == scope) {
        members.push_back(node.id.str());
      }
    }
    if (members.empty()) {
      continue;
    }
    const std::string zone_id = "z" + std::to_string(index);
    plan.nodes.push_back(make_node(zone_id, NodeKind::Zone, "Synthetic " + zone_id, scope));
    plan.containment.push_back(physical("fac-1", zone_id));
    const std::size_t take = 1 + rng.index(std::min<std::size_t>(members.size(), 3));
    for (std::size_t member = 0; member < take; ++member) {
      plan.containment.push_back(logical(zone_id, members[member]));
    }
  }

  // Physical adjacency between physical nodes of the same facility.
  std::vector<std::string> physical;
  for (const NodeRecord& node : plan.nodes) {
    if (schema_allows_adjacency(node.kind, node.kind) && node.kind != NodeKind::Building) {
      physical.push_back(node.id.str());
    }
  }
  const std::size_t edge_count = rng.below(6);
  std::set<std::pair<std::string, std::string>> seen;
  for (std::size_t index = 0; index < edge_count && physical.size() >= 2; ++index) {
    const std::string& first = physical[rng.index(physical.size())];
    const std::string& second = physical[rng.index(physical.size())];
    if (first == second) {
      continue;
    }
    // The relation is symmetric: the canonical key is the ordered pair.
    const std::pair<std::string, std::string> key =
        (first < second) ? std::make_pair(first, second) : std::make_pair(second, first);
    if (!seen.insert(key).second) {
      continue;
    }
    plan.adjacency.push_back(adjacent(key.first, key.second));
  }

  // Domain references.
  const std::size_t domain_count = 1 + rng.below(2);
  for (std::size_t index = 0; index < domain_count; ++index) {
    plan.domains.push_back(declare_domain("pwr-" + std::to_string(index), DomainKind::Power, "Synthetic feed"));
    plan.domains.push_back(declare_domain("cool-" + std::to_string(index), DomainKind::Cooling, "Synthetic loop"));
  }
  for (const NodeRecord& node : plan.nodes) {
    if (!node_kind_schema(node.kind).zonable) {
      continue;
    }
    if (!rng.chance(1, 3)) {
      continue;
    }
    const DomainDeclaration& domain = plan.domains[rng.index(plan.domains.size())];
    plan.associations.push_back(associate(node.id.str(), domain.id.str(), domain.kind));
  }
  return plan;
}

TopologySnapshot build_plan(const GraphPlan& plan, const TopologyGeneration& generation) {
  auto snapshot = assemble_topology(plan.nodes, plan.containment, plan.adjacency, plan.domains, plan.associations,
                                    generation);
  FT_REQUIRE(snapshot.has_value());
  return std::move(*snapshot);
}

std::string describe_batch(const MutationBatch& batch) { return serialize_batch(batch); }

Digest describe_batch_digest(const MutationBatch& batch) { return batch_digest(batch); }

/// True when no node in the snapshot is its own ancestor.
bool no_node_is_its_own_ancestor(const TopologySnapshot& snapshot) {
  for (const NodeId& id : snapshot.node_ids()) {
    for (const NodeId& ancestor : snapshot.ancestry(id)) {
      if (ancestor == id) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

FT_TEST(property, generated_graphs_are_always_valid_and_canonical) {
  for (int iteration = 0; iteration < 40; ++iteration) {
    Rng rng(current_seed() + static_cast<std::uint64_t>(iteration) * 7919ULL);
    set_current_case_context("property.generated_graphs_are_always_valid_and_canonical#" + std::to_string(iteration));
    const GraphPlan plan = plan_facility(rng, 2 + rng.below(40));
    auto snapshot = assemble_topology(plan.nodes, plan.containment, plan.adjacency, plan.domains, plan.associations,
                                      TopologyGeneration(1));
    FT_REQUIRE(snapshot.has_value());

    // Every generated graph is structurally valid.
    FT_CHECK(snapshot->stats().facilities >= 1);
    FT_CHECK_EQ(snapshot->stats().nodes, plan.nodes.size());
    FT_CHECK(no_node_is_its_own_ancestor(*snapshot));

    // Canonical serialization round trips byte-exactly.
    GenerationManifest manifest;
    manifest.generation = TopologyGeneration(1);
    manifest.authority_epoch = WriterEpoch(1);
    manifest.actor = actor_id("tester");
    manifest.source = "test";
    manifest.recorded_at = std::string(kFixtureTimestamp);
    manifest.retention_floor = 1;
    auto text = serialize_generation(*snapshot, manifest);
    FT_REQUIRE(text.has_value());
    auto parsed = parse_generation(*text, TopologyLimits{});
    FT_REQUIRE(parsed.has_value());
    FT_CHECK(parsed->snapshot.same_structure_as(*snapshot));
    auto again = serialize_generation(parsed->snapshot, parsed->manifest);
    FT_REQUIRE(again.has_value());
    FT_CHECK_EQ(*again, *text);

    // Iteration order is canonical everywhere it is observable.
    const std::vector<NodeId> ids = snapshot->node_ids();
    for (std::size_t index = 1; index < ids.size(); ++index) {
      FT_CHECK(ids[index - 1] < ids[index]);
    }
    const std::vector<ContainmentEdge> edges = snapshot->containment_edges();
    for (std::size_t index = 1; index < edges.size(); ++index) {
      const bool ordered = edges[index - 1].parent < edges[index].parent ||
                           (edges[index - 1].parent == edges[index].parent &&
                            edges[index - 1].child < edges[index].child);
      FT_CHECK(ordered);
    }
    const std::vector<AdjacencyEdge> adjacency = snapshot->adjacency_edges();
    for (const AdjacencyEdge& edge : adjacency) {
      FT_CHECK(edge.first < edge.second);
    }

    // Ancestry never contains a repeat, and every non-root node has a parent.
    for (const NodeId& id : ids) {
      const std::vector<NodeId> chain = snapshot->ancestry(id);
      FT_CHECK(chain.size() <= kMaxStructuralDepth);
      std::set<std::string> unique;
      for (const NodeId& ancestor : chain) {
        FT_CHECK(unique.insert(ancestor.str()).second);
      }
      const NodeRecord* record = snapshot->find_node(id);
      FT_REQUIRE(record != nullptr);
      if (record->kind != NodeKind::Facility) {
        FT_CHECK(snapshot->physical_parent(id).has_value());
      } else {
        FT_CHECK(!snapshot->physical_parent(id).has_value());
        FT_CHECK(chain.empty());
      }
    }
  }
}

FT_TEST(property, traversals_are_deterministic_and_complete) {
  for (int iteration = 0; iteration < 30; ++iteration) {
    Rng rng(current_seed() + 104729ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("property.traversals_are_deterministic_and_complete#" + std::to_string(iteration));
    const GraphPlan plan = plan_facility(rng, 3 + rng.below(30));
    const TopologySnapshot snapshot = build_plan(plan, TopologyGeneration(1));

    TraversalLimits bounds;
    bounds.max_depth = 32;
    bounds.max_nodes = 10000;

    auto bfs_first = snapshot.traverse(node_id("fac-1"), TraversalOrder::BreadthFirst, bounds);
    auto bfs_second = snapshot.traverse(node_id("fac-1"), TraversalOrder::BreadthFirst, bounds);
    FT_REQUIRE(bfs_first.has_value());
    FT_REQUIRE(bfs_second.has_value());
    FT_CHECK(*bfs_first == *bfs_second);

    auto dfs_first = snapshot.traverse(node_id("fac-1"), TraversalOrder::DepthFirstPreOrder, bounds);
    auto dfs_second = snapshot.traverse(node_id("fac-1"), TraversalOrder::DepthFirstPreOrder, bounds);
    FT_REQUIRE(dfs_first.has_value());
    FT_REQUIRE(dfs_second.has_value());
    FT_CHECK(*dfs_first == *dfs_second);

    // Both orders visit exactly the physical descendants (excluding members
    // that hang under a zone, which is a logical boundary).
    const std::vector<NodeId> descendants = snapshot.descendants(node_id("fac-1"));
    std::set<std::string> expected;
    for (const NodeId& id : descendants) {
      expected.insert(id.str());
    }
    std::set<std::string> breadth;
    for (const NodeId& id : *bfs_first) {
      breadth.insert(id.str());
    }
    FT_CHECK_EQ(breadth.size(), expected.size());
    FT_CHECK_EQ(bfs_first->size(), dfs_first->size());

    // A depth bound below the real depth is rejected instead of truncated.
    TraversalLimits shallow;
    shallow.max_depth = 1;
    shallow.max_nodes = 10000;
    const auto bounded = snapshot.traverse(node_id("fac-1"), TraversalOrder::BreadthFirst, shallow);
    if (snapshot.stats().max_depth > 1) {
      FT_CHECK_ERROR(bounded, ErrorCode::TraversalDepthExceeded);
    }
  }
}

FT_TEST(property, diffs_are_deterministic_and_invertible) {
  for (int iteration = 0; iteration < 30; ++iteration) {
    Rng rng(current_seed() + 15485863ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("property.diffs_are_deterministic_and_invertible#" + std::to_string(iteration));
    const GraphPlan first_plan = plan_facility(rng, 3 + rng.below(25));
    const GraphPlan second_plan = plan_facility(rng, 3 + rng.below(25));
    const TopologySnapshot first = build_plan(first_plan, TopologyGeneration(1));
    const TopologySnapshot second = build_plan(second_plan, TopologyGeneration(2));

    auto forward = diff(first, second);
    auto forward_again = diff(first, second);
    FT_REQUIRE(forward.has_value());
    FT_REQUIRE(forward_again.has_value());
    FT_CHECK_EQ(forward->to_string(), forward_again->to_string());

    auto backward = diff(second, first);
    FT_REQUIRE(backward.has_value());
    FT_CHECK_EQ(forward->nodes_added.size(), backward->nodes_removed.size());
    FT_CHECK_EQ(forward->nodes_removed.size(), backward->nodes_added.size());
    FT_CHECK_EQ(forward->containment_added.size(), backward->containment_removed.size());
    FT_CHECK_EQ(forward->adjacency_added.size(), backward->adjacency_removed.size());
    FT_CHECK_EQ(forward->associations_added.size(), backward->associations_removed.size());

    auto self = diff(first, first);
    FT_REQUIRE(self.has_value());
    FT_CHECK(self->empty());
  }
}

FT_TEST(property, batch_application_is_atomic_and_deterministic) {
  for (int iteration = 0; iteration < 40; ++iteration) {
    Rng rng(current_seed() + 32452843ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("property.batch_application_is_atomic_and_deterministic#" + std::to_string(iteration));
    const GraphPlan plan = plan_facility(rng, 5 + rng.below(20));
    const TopologySnapshot base = build_plan(plan, TopologyGeneration(4));

    // A mixture of plausible and implausible mutations.
    std::vector<Mutation> mutations;
    const std::size_t count = 1 + rng.below(6);
    for (std::size_t index = 0; index < count; ++index) {
      const std::size_t pick = rng.below(10);
      const std::string id = "x" + std::to_string(index);
      if (pick == 0) {
        mutations.push_back(add(id, NodeKind::Rack, "Synthetic " + id));
      } else if (pick == 1) {
        mutations.push_back(contain("fac-1", id));
      } else if (pick == 2) {
        mutations.push_back(RemoveNode{node_id("n" + std::to_string(rng.below(20)))});
      } else if (pick == 3) {
        mutations.push_back(RemoveContainment{node_id("fac-1"), node_id("n" + std::to_string(rng.below(20)))});
      } else if (pick == 4) {
        mutations.push_back(AddAdjacency{adjacent("n" + std::to_string(rng.below(20)),
                                                  "n" + std::to_string(rng.below(20)))});
      } else if (pick == 5) {
        mutations.push_back(RemoveAdjacency{node_id("n" + std::to_string(rng.below(20))),
                                            node_id("n" + std::to_string(rng.below(20))), AdjacencyKind::SharedBoundary});
      } else if (pick == 6) {
        mutations.push_back(DeclareDomain{declare_domain("d" + std::to_string(index), DomainKind::Power)});
      } else if (pick == 7) {
        mutations.push_back(RetireDomain{domain_id("pwr-" + std::to_string(rng.below(2)))});
      } else if (pick == 8) {
        mutations.push_back(SetNodeLabel{node_id("n" + std::to_string(rng.below(20))), "label " + id, provenance()});
      } else {
        mutations.push_back(MoveNode{node_id("n" + std::to_string(rng.below(20))),
                                     node_id("fac-1"), provenance()});
      }
    }

    const MutationBatch batch = make_batch("prop-" + std::to_string(iteration), base.generation(), mutations);
    const BatchApplication first = apply_batch_explained(base, batch, TopologyLimits{});
    const BatchApplication second = apply_batch_explained(base, batch, TopologyLimits{});

    // Deterministic outcome: same base and same batch, same result.
    FT_CHECK_EQ(first.outcome.to_string(), second.outcome.to_string());
    FT_CHECK_EQ(first.outcome.explanation, second.outcome.explanation);
    FT_CHECK_EQ(first.accepted(), second.accepted());

    if (first.accepted()) {
      FT_CHECK_EQ(first.candidate.generation().value(), base.generation().value() + 1);
      FT_CHECK(first.candidate.content_digest() == second.candidate.content_digest());
      FT_CHECK(no_node_is_its_own_ancestor(first.candidate));
      // The candidate survives a serialization round trip.
      GenerationManifest manifest;
      manifest.generation = first.candidate.generation();
      manifest.parent_generation = base.generation();
      manifest.parent_digest = digest_of("parent");
      manifest.authority_epoch = WriterEpoch(1);
      manifest.actor = actor_id("tester");
      manifest.source = "test";
      manifest.recorded_at = std::string(kFixtureTimestamp);
      manifest.retention_floor = 1;
      auto text = serialize_generation(first.candidate, manifest);
      FT_REQUIRE(text.has_value());
      auto parsed = parse_generation(*text, TopologyLimits{});
      FT_REQUIRE(parsed.has_value());
      FT_CHECK(parsed->snapshot.same_structure_as(first.candidate));
    } else {
      FT_CHECK(!first.candidate.valid());
      FT_CHECK(!first.outcome.dispositions.empty());
      std::size_t rejected = 0;
      for (const MutationDisposition& disposition : first.outcome.dispositions) {
        if (!disposition.accepted()) {
          ++rejected;
          FT_CHECK(!disposition.explanation.empty());
        }
      }
      FT_CHECK(rejected >= 1);
    }

    // The batch encoding is deterministic and content-addressed.
    FT_CHECK_EQ(serialize_batch(batch), describe_batch(batch));
    FT_CHECK(batch_digest(batch) == describe_batch_digest(batch));

    // The base generation is untouched either way.
    FT_CHECK_EQ(base.generation().value(), std::uint64_t{4});
    FT_CHECK_EQ(base.stats().nodes, plan.nodes.size());
  }
}

FT_TEST(property, containment_cycles_are_always_rejected) {
  for (int iteration = 0; iteration < 25; ++iteration) {
    Rng rng(current_seed() + 49979687ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("property.containment_cycles_are_always_rejected#" + std::to_string(iteration));
    const GraphPlan plan = plan_facility(rng, 6 + rng.below(15));
    auto snapshot = assemble_topology(plan.nodes, plan.containment, plan.adjacency, plan.domains, plan.associations,
                                      TopologyGeneration(1));
    FT_REQUIRE(snapshot.has_value());
    FT_CHECK(no_node_is_its_own_ancestor(*snapshot));

    // A node can never be reparented under itself.
    for (const NodeId& id : snapshot->node_ids()) {
      const NodeRecord* record = snapshot->find_node(id);
      FT_REQUIRE(record != nullptr);
      if (record->kind == NodeKind::Facility) {
        continue;
      }
      auto builder = TopologyBuilder::from_snapshot(*snapshot);
      FT_REQUIRE(builder.has_value());
      const Result<void> moved = builder->move_node(id, id, provenance());
      FT_CHECK_ERROR(moved, ErrorCode::ContainmentCycle);
    }

    // No attempt to reparent a node under one of its own descendants may ever
    // succeed, whichever rejection reason the schema produces first.
    for (const NodeId& container : snapshot->node_ids()) {
      for (const NodeId& descendant : snapshot->descendants(container)) {
        auto builder = TopologyBuilder::from_snapshot(*snapshot);
        FT_REQUIRE(builder.has_value());
        const Result<void> moved = builder->move_node(container, descendant, provenance());
        if (moved.has_value()) {
          FT_FAIL("a move of a container under its own descendant was accepted");
          continue;
        }
        // Whatever the reason, the result must never be a cycle.
        auto rebuilt = builder->build(snapshot->generation());
        if (rebuilt.has_value()) {
          FT_CHECK(no_node_is_its_own_ancestor(*rebuilt));
        }
      }
    }

    // The same containment added as a new edge is rejected as well.
    for (const ContainmentEdge& edge : plan.containment) {
      for (const NodeId& descendant : snapshot->descendants(edge.child)) {
        auto builder = TopologyBuilder::from_snapshot(*snapshot);
        FT_REQUIRE(builder.has_value());
        const Result<void> added = builder->add_containment(physical(edge.child.str(), descendant.str()));
        FT_CHECK(!added.has_value());
      }
    }
  }
}

FT_TEST(property, orphans_are_always_rejected) {
  // Assembling a graph that omits a physical containment edge leaves the child
  // uncontained; the graph must be rejected rather than accepted as a forest
  // with a detached fragment.
  for (int iteration = 0; iteration < 20; ++iteration) {
    Rng rng(current_seed() + 86028121ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("property.orphans_are_always_rejected#" + std::to_string(iteration));
    const GraphPlan plan = plan_facility(rng, 6 + rng.below(15));

    // Only a physical edge carries containment; dropping a zone membership
    // leaves a perfectly valid generation, which is itself worth asserting.
    std::vector<ContainmentEdge> physical_edges;
    std::vector<ContainmentEdge> logical_edges;
    for (const ContainmentEdge& edge : plan.containment) {
      (edge.boundary == BoundaryKind::Physical ? physical_edges : logical_edges).push_back(edge);
    }
    FT_REQUIRE(physical_edges.size() >= 2);

    const std::size_t victim = 1 + rng.index(physical_edges.size() - 1);
    std::vector<ContainmentEdge> dropped = plan.containment;
    const std::string removed_parent = physical_edges[victim].parent.str();
    const std::string removed_child = physical_edges[victim].child.str();
    dropped.erase(std::remove_if(dropped.begin(), dropped.end(),
                                 [&removed_parent, &removed_child](const ContainmentEdge& edge) {
                                   return edge.parent.str() == removed_parent && edge.child.str() == removed_child;
                                 }),
                  dropped.end());

    auto broken = assemble_topology(plan.nodes, dropped, plan.adjacency, plan.domains, plan.associations,
                                    TopologyGeneration(1));
    FT_CHECK(!broken.has_value());
    if (!broken.has_value()) {
      // The rejection must be a structural one, not an incidental complaint.
      const ErrorCategory category = broken.error().category();
      FT_CHECK(category == ErrorCategory::Structure || category == ErrorCategory::Argument);
    }

    if (!logical_edges.empty()) {
      std::vector<ContainmentEdge> without_membership = plan.containment;
      const std::string member_parent = logical_edges.front().parent.str();
      const std::string member_child = logical_edges.front().child.str();
      without_membership.erase(std::remove_if(without_membership.begin(), without_membership.end(),
                                              [&member_parent, &member_child](const ContainmentEdge& edge) {
                                                return edge.parent.str() == member_parent &&
                                                       edge.child.str() == member_child;
                                              }),
                               without_membership.end());
      auto still_valid = assemble_topology(plan.nodes, without_membership, plan.adjacency, plan.domains,
                                           plan.associations, TopologyGeneration(1));
      FT_CHECK(still_valid.has_value());
    }
  }
}

FT_TEST(property, generation_content_digest_ignores_lineage) {
  Rng rng(current_seed());
  const GraphPlan plan = plan_facility(rng, 12);
  auto first = assemble_topology(plan.nodes, plan.containment, plan.adjacency, plan.domains, plan.associations,
                                 TopologyGeneration(1));
  auto second = assemble_topology(plan.nodes, plan.containment, plan.adjacency, plan.domains, plan.associations,
                                  TopologyGeneration(99));
  FT_REQUIRE(first.has_value());
  FT_REQUIRE(second.has_value());
  FT_CHECK(first->content_digest() == second->content_digest());
  FT_CHECK(first->same_structure_as(*second));
  FT_CHECK(first->generation() != second->generation());

  // Reversing the input order does not change the content digest.
  std::vector<NodeRecord> reversed_nodes = plan.nodes;
  std::reverse(reversed_nodes.begin(), reversed_nodes.end());
  std::vector<ContainmentEdge> reversed_edges = plan.containment;
  std::reverse(reversed_edges.begin(), reversed_edges.end());
  auto third = assemble_topology(reversed_nodes, reversed_edges, plan.adjacency, plan.domains, plan.associations,
                                 TopologyGeneration(7));
  FT_REQUIRE(third.has_value());
  FT_CHECK(third->content_digest() == first->content_digest());
}
