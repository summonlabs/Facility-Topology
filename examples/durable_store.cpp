// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example: durable store lifecycle.
//
// Demonstrates create, commit, export, reopen, integrity-checked recovery and
// stale-writer rejection against a real store directory. The topology is
// synthetic.

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/facility_topology/mutation.hpp"
#include "dccp/facility_topology/store.hpp"

using namespace dccp::facility_topology;

namespace {

TopologySnapshot synthetic_facility() {
  std::vector<NodeRecord> nodes;
  std::vector<ContainmentEdge> containment;
  std::vector<DomainDeclaration> domains;
  std::vector<DomainAssociation> associations;

  const ProvenanceRecord record = [] {
    ProvenanceRecord value;
    value.actor = ActorId::parse("example").value();
    value.source = "example";
    value.reason = "durable store example";
    return value;
  }();
  const auto node = [&record](std::string_view id, NodeKind kind, std::string label) {
    NodeRecord value;
    value.id = NodeId::parse(id).value();
    value.kind = kind;
    value.label = std::move(label);
    value.provenance = record;
    return value;
  };
  const auto link = [&record](std::string_view parent, std::string_view child) {
    ContainmentEdge value;
    value.parent = NodeId::parse(parent).value();
    value.child = NodeId::parse(child).value();
    value.boundary = BoundaryKind::Physical;
    value.provenance = record;
    return value;
  };

  nodes.push_back(node("fac-1", NodeKind::Facility, "Example Facility"));
  nodes.push_back(node("hall-1", NodeKind::Hall, "Hall 1"));
  nodes.push_back(node("row-1", NodeKind::Row, "Row 1"));
  nodes.push_back(node("rack-01", NodeKind::Rack, "Rack 01"));
  containment.push_back(link("fac-1", "hall-1"));
  containment.push_back(link("hall-1", "row-1"));
  containment.push_back(link("row-1", "rack-01"));
  domains.push_back(DomainDeclaration{DomainId::parse("pwr-a").value(), DomainKind::Power, "Power feed A", record});
  associations.push_back(DomainAssociation{NodeId::parse("rack-01").value(), DomainId::parse("pwr-a").value(),
                                           DomainKind::Power, record});
  return assemble_topology(nodes, containment, {}, domains, associations, TopologyGeneration(1)).value();
}

int fail(const Error& error) {
  std::cerr << "example failed: " << error.to_string() << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path directory =
      (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "facility_topology_example";

  // 1. Create generation 1.
  auto store = TopologyStore::open(directory, OpenMode::ReadWrite);
  if (!store.has_value()) {
    return fail(store.error());
  }
  CreateOptions options;
  options.actor = ActorId::parse("example").value();
  options.source = "example";
  options.reason = "initial topology";
  options.mutation_id = MutationId::parse("example-create").value();
  auto created = store->create(synthetic_facility(), options);
  if (!created.has_value()) {
    return fail(created.error());
  }
  std::cout << "created: " << created->to_string() << "\n";
  std::cout << "writer epoch: " << store->epoch().value() << "\n";

  // 2. Commit generation 2 through the normal authority path.
  MutationBatch batch;
  batch.base_generation = TopologyGeneration(1);
  batch.authority_epoch = store->epoch();
  batch.actor = ActorId::parse("example").value();
  batch.source = "example";
  batch.reason = "add a rack";
  batch.mutation_id = MutationId::parse("example-commit-1").value();

  NodeRecord rack;
  rack.id = NodeId::parse("rack-02").value();
  rack.kind = NodeKind::Rack;
  rack.label = "Rack 02";
  rack.provenance.actor = ActorId::parse("example").value();
  rack.provenance.source = "example";
  rack.provenance.reason = "add a rack";
  batch.mutations.push_back(AddNode{rack});

  ContainmentEdge link;
  link.parent = NodeId::parse("row-1").value();
  link.child = NodeId::parse("rack-02").value();
  link.boundary = BoundaryKind::Physical;
  link.provenance = rack.provenance;
  batch.mutations.push_back(AddContainment{link});

  auto committed = store->commit(batch);
  if (!committed.has_value()) {
    return fail(committed.error());
  }
  std::cout << "committed: " << committed->to_string() << "\n";

  // 3. Retrying the same batch identity is an idempotent no-op.
  auto replay = store->commit(batch);
  if (!replay.has_value()) {
    return fail(replay.error());
  }
  std::cout << "replay: " << replay->to_string() << " (" << replay->explanation << ")\n";

  // 4. Export the head generation.
  const std::filesystem::path exported = directory / "exported.ftop";
  auto exported_result = store->export_head_to(exported);
  if (!exported_result.has_value()) {
    return fail(exported_result.error());
  }
  std::cout << "exported to " << exported.string() << "\n";

  // 5. Reopen read-only: the same authoritative state is visible.
  auto reader = TopologyStore::open(directory, OpenMode::ReadOnly);
  if (!reader.has_value()) {
    return fail(reader.error());
  }
  auto head = reader->head();
  if (!head.has_value()) {
    return fail(head.error());
  }
  std::cout << "reopened head generation " << head->generation().value() << ": " << head->stats().to_string() << "\n";

  // 6. A read-only handle cannot mutate.
  auto refused = reader->commit(batch);
  if (refused.has_value()) {
    std::cerr << "example failed: a read-only handle accepted a commit\n";
    return 1;
  }
  std::cout << "read-only commit refused: " << error_code_name(refused.error().code()) << "\n";

  // 7. Reopening for mutation fences the previous writer epoch.
  const WriterEpoch first_epoch = store->epoch();
  auto second_writer = TopologyStore::open(directory, OpenMode::ReadWrite);
  if (!second_writer.has_value()) {
    return fail(second_writer.error());
  }
  std::cout << "second writer epoch " << second_writer->epoch().value() << " > " << first_epoch.value() << "\n";
  auto fenced = store->commit(batch);
  if (fenced.has_value()) {
    std::cerr << "example failed: a fenced writer published\n";
    return 1;
  }
  std::cout << "stale writer refused: " << error_code_name(fenced.error().code()) << "\n";

  // 8. Recovery on a healthy store reports that nothing needs repair.
  auto report = second_writer->recover();
  if (!report.has_value()) {
    return fail(report.error());
  }
  std::cout << "recovery: " << report->explanation << "\n";
  return 0;
}
