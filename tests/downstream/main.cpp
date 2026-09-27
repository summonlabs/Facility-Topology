// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent downstream consumer of the installed Facility Topology package.
//
// This program is built outside the Facility Topology source tree against an
// installed package, exactly as a later DCCP repository would consume it. It
// exercises the documented public API only: build a generation, validate it,
// serialize it, parse it back, publish it durably, read it through a second
// handle and compare two generations.

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <dccp/facility_topology/canonical.hpp>
#include <dccp/facility_topology/diff.hpp>
#include <dccp/facility_topology/mutation.hpp>
#include <dccp/facility_topology/store.hpp>
#include <dccp/facility_topology/topology.hpp>
#include <dccp/facility_topology/version.hpp>

namespace {

using namespace dccp::facility_topology;

int fail(const std::string& message) {
  std::cerr << "downstream consumer failed: " << message << "\n";
  return 1;
}

ProvenanceRecord provenance() {
  ProvenanceRecord record;
  record.actor = ActorId::parse("downstream").value();
  record.source = "downstream";
  record.reason = "consumer check";
  record.recorded_at = "2026-02-01T00:00:00Z";
  return record;
}

NodeRecord node(std::string_view id, NodeKind kind, std::string label) {
  NodeRecord record;
  record.id = NodeId::parse(id).value();
  record.kind = kind;
  record.label = std::move(label);
  record.provenance = provenance();
  return record;
}

ContainmentEdge containment(std::string_view parent, std::string_view child) {
  ContainmentEdge edge;
  edge.parent = NodeId::parse(parent).value();
  edge.child = NodeId::parse(child).value();
  edge.boundary = BoundaryKind::Physical;
  edge.provenance = provenance();
  return edge;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "facility topology version: " << kLibraryVersion << "\n";
  std::cout << "canonical banner: " << kCanonicalBanner << "\n";

  // 1. Assemble and validate a generation.
  std::vector<NodeRecord> nodes = {node("fac-1", NodeKind::Facility, "Consumer facility"),
                                   node("hall-1", NodeKind::Hall, "Hall 1"),
                                   node("row-1", NodeKind::Row, "Row 1"),
                                   node("rack-01", NodeKind::Rack, "Rack 01"),
                                   node("rack-02", NodeKind::Rack, "Rack 02")};
  std::vector<ContainmentEdge> edges = {containment("fac-1", "hall-1"), containment("hall-1", "row-1"),
                                        containment("row-1", "rack-01"), containment("row-1", "rack-02")};
  std::vector<DomainDeclaration> domains;
  DomainDeclaration power;
  power.id = DomainId::parse("pwr-a").value();
  power.kind = DomainKind::Power;
  power.label = "Feed A";
  power.provenance = provenance();
  domains.push_back(power);

  auto generation = assemble_topology(nodes, edges, {}, domains, {}, TopologyGeneration(1));
  if (!generation.has_value()) {
    return fail(generation.error().to_string());
  }
  std::cout << "generation 1: " << generation->stats().to_string() << "\n";

  // 2. Query the immutable snapshot.
  const NodeId rack = NodeId::parse("rack-01").value();
  std::cout << "ancestry:";
  for (const NodeId& ancestor : generation->ancestry(rack)) {
    std::cout << " " << ancestor.str();
  }
  std::cout << "\n";

  // 3. Canonical serialization round trip.
  GenerationManifest manifest;
  manifest.generation = TopologyGeneration(1);
  manifest.authority_epoch = WriterEpoch(1);
  manifest.actor = ActorId::parse("downstream").value();
  manifest.source = "downstream";
  manifest.recorded_at = "2026-02-01T00:00:00Z";
  manifest.retention_floor = 1;
  auto text = serialize_generation(*generation, manifest);
  if (!text.has_value()) {
    return fail(text.error().to_string());
  }
  auto parsed = parse_generation(*text, TopologyLimits{});
  if (!parsed.has_value()) {
    return fail(parsed.error().to_string());
  }
  if (!parsed->snapshot.same_structure_as(*generation)) {
    return fail("canonical round trip changed the structure");
  }
  std::cout << "canonical bytes: " << text->size()
            << " digest=" << digest_tagged_hex(document_digest(*text)) << "\n";

  // 4. Durable publication through a real store directory.
  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(error) / "facility_topology_downstream_store";
  std::filesystem::remove_all(directory, error);

  auto store = TopologyStore::open(directory, OpenMode::ReadWrite);
  if (!store.has_value()) {
    return fail(store.error().to_string());
  }
  CreateOptions options;
  options.actor = ActorId::parse("downstream").value();
  options.source = "downstream";
  options.reason = "consumer check";
  options.mutation_id = MutationId::parse("downstream-create").value();
  auto created = store->create(*generation, options);
  if (!created.has_value()) {
    return fail(created.error().to_string());
  }
  std::cout << "created: " << created->to_string() << "\n";

  // 5. Evolve the topology through a validated batch.
  MutationBatch batch;
  batch.base_generation = TopologyGeneration(1);
  batch.authority_epoch = store->epoch();
  batch.actor = ActorId::parse("downstream").value();
  batch.source = "downstream";
  batch.reason = "add a rack";
  batch.mutation_id = MutationId::parse("downstream-commit-1").value();
  NodeRecord extra = node("rack-03", NodeKind::Rack, "Rack 03");
  batch.mutations.push_back(AddNode{extra});
  batch.mutations.push_back(AddContainment{containment("row-1", "rack-03")});
  auto committed = store->commit(batch);
  if (!committed.has_value()) {
    return fail(committed.error().to_string());
  }
  if (committed->new_generation.value() != 2) {
    return fail("expected generation 2 after the commit");
  }

  // 6. A second, read-only handle sees the same authoritative state.
  auto reader = TopologyStore::open(directory, OpenMode::ReadOnly);
  if (!reader.has_value()) {
    return fail(reader.error().to_string());
  }
  auto head = reader->head();
  if (!head.has_value()) {
    return fail(head.error().to_string());
  }
  if (head->stats().nodes != 6) {
    return fail("expected six nodes after the commit");
  }

  // 7. Compare generations deterministically.
  auto previous = reader->load_generation(TopologyGeneration(1));
  if (!previous.has_value()) {
    return fail(previous.error().to_string());
  }
  auto changes = diff(*previous, *head);
  if (!changes.has_value()) {
    return fail(changes.error().to_string());
  }
  std::cout << "changes: " << changes->change_count() << "\n";

  // 8. Errors are stable and machine readable.
  auto rejected = store->commit(batch);
  if (rejected.has_value() && !rejected->replayed) {
    return fail("expected the repeated batch to be recognised as a replay");
  }
  auto stale = store->commit(MutationBatch{});
  if (stale.has_value()) {
    return fail("expected an empty batch without authority to be refused");
  }
  std::cout << "refusal code: " << error_code_name(stale.error().code()) << "\n";

  if (argc > 1) {
    std::cout << "argument: " << argv[1] << "\n";
  }
  std::cout << "downstream consumer: ok\n";
  std::filesystem::remove_all(directory, error);
  return 0;
}
