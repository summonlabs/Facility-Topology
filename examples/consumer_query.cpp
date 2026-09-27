// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example: a downstream consumer reading Facility Topology state.
//
// This example is written the way a later DCCP repository consumes this one:
// it opens a store read-only, works only with immutable snapshots, and never
// touches persistence structures directly. It also shows the shape of a
// deterministic export handed to a capacity, placement, failure-domain or
// observability consumer without implementing any of those consumers.

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/facility_topology/diff.hpp"
#include "dccp/facility_topology/store.hpp"

using namespace dccp::facility_topology;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: consumer_query <store-directory>\n";
    return 2;
  }
  const std::filesystem::path directory = argv[1];

  auto store = TopologyStore::open(directory, OpenMode::ReadOnly);
  if (!store.has_value()) {
    std::cerr << "cannot open store: " << store.error().to_string() << "\n";
    return 1;
  }

  auto head = store->head();
  if (!head.has_value()) {
    std::cerr << "cannot read head generation: " << head.error().to_string() << "\n";
    return 1;
  }
  const TopologySnapshot& snapshot = *head;
  std::cout << "generation=" << snapshot.generation().value() << "\n";
  std::cout << "stats=" << snapshot.stats().to_string() << "\n";

  // 1. Facility roots and their immediate structure.
  for (const NodeId& facility : snapshot.facilities()) {
    const NodeRecord* record = snapshot.find_node(facility);
    std::cout << "facility=" << facility.str() << " label=\""
              << (record != nullptr ? record->label : std::string()) << "\"\n";
    std::cout << "  children=" << snapshot.children(facility).size() << "\n";
    std::cout << "  descendants=" << snapshot.descendants(facility).size() << "\n";
  }

  // 2. A placement consumer asks which racks sit under a container. This
  //    repository answers with structure only: it never decides placement.
  std::vector<NodeId> racks = snapshot.nodes_of_kind(NodeKind::Rack);
  std::size_t contained = 0;
  for (const NodeId& rack : racks) {
    if (snapshot.physical_parent(rack).has_value()) {
      ++contained;
    }
  }
  std::cout << "racks=" << racks.size() << " contained=" << contained << "\n";

  // 3. A failure-domain consumer asks which racks reference each declared
  //    domain. The domain itself belongs to the power or cooling control
  //    plane; this repository only holds the reference.
  for (const DomainDeclaration& declaration : snapshot.domains()) {
    const std::vector<NodeId> members = snapshot.nodes_in_domain(declaration.id);
    std::cout << "domain=" << declaration.id.str() << " kind=" << domain_kind_token(declaration.kind)
              << " members=" << members.size() << "\n";
    for (const NodeId& member : members) {
      std::cout << "  member=" << member.str() << "\n";
    }
  }

  // 4. An observability consumer asks for the deterministic export of one
  //    generation: canonical bytes, stable digest, ordered records.
  auto document = store->head_document();
  if (!document.has_value()) {
    std::cerr << "cannot read head document: " << document.error().to_string() << "\n";
    return 1;
  }
  auto canonical = serialize_generation(document->snapshot, document->manifest);
  if (!canonical.has_value()) {
    std::cerr << "cannot serialize head generation: " << canonical.error().to_string() << "\n";
    return 1;
  }
  std::cout << "canonical-bytes=" << canonical->size() << "\n";
  std::cout << "document-digest=" << digest_tagged_hex(document_digest(*canonical)) << "\n";
  std::cout << "content-digest=" << digest_tagged_hex(snapshot.content_digest()) << "\n";

  // 5. A change consumer compares the retained generations deterministically.
  auto numbers = store->generation_numbers();
  if (numbers.has_value() && numbers->size() >= 2) {
    auto previous = store->load_generation((*numbers)[numbers->size() - 2]);
    if (previous.has_value()) {
      auto changes = diff(*previous, snapshot);
      if (changes.has_value()) {
        std::cout << "changes-since=" << (*numbers)[numbers->size() - 2].value()
                  << " count=" << changes->change_count() << "\n";
      }
    }
  }
  return 0;
}
