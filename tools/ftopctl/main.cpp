// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// ftopctl - read-only inspection and narrowly scoped mutation for Facility
// Topology stores.
//
// The tool never bypasses the library's authority model: every mutation goes
// through TopologyStore::commit, so generation preconditions, writer epochs,
// idempotency and atomic publication behave exactly as they do for any other
// consumer. Read-only commands never take mutation authority.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/canonical.hpp"
#include "dccp/facility_topology/diff.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "dccp/facility_topology/store.hpp"
#include "dccp/facility_topology/text.hpp"
#include "dccp/facility_topology/version.hpp"
#include "script.hpp"

namespace {

using namespace dccp::facility_topology;

constexpr int kExitOk = 0;
constexpr int kExitRejected = 1;
constexpr int kExitUsage = 2;

void print_error(const Error& error) {
  std::cout << "error-code=" << error_code_name(error.code()) << "\n";
  std::cout << "error-category=" << error_category_name(error.category()) << "\n";
  std::cout << "error-message=" << error.message() << "\n";
  if (!error.subject().empty()) {
    std::cout << "error-subject=" << error.subject() << "\n";
  }
}

/// Parsed command line: options are `--name value` or `--flag`.
class Arguments {
 public:
  explicit Arguments(std::vector<std::string> values) : values_(std::move(values)) {}

  bool empty() const { return values_.empty(); }
  const std::string& front() const { return values_.front(); }

  bool has(std::string_view name) const {
    return std::find(values_.begin() + 1, values_.end(), name) != values_.end();
  }

  std::optional<std::string> value(std::string_view name) const {
    for (std::size_t index = 1; index + 1 < values_.size(); ++index) {
      if (values_[index] == name) {
        return values_[index + 1];
      }
    }
    return std::nullopt;
  }

  /// Positional arguments after the command, excluding option values.
  std::vector<std::string> positional() const {
    std::vector<std::string> out;
    for (std::size_t index = 1; index < values_.size(); ++index) {
      if (is_option(values_[index])) {
        ++index;  // skip the option value
        continue;
      }
      if (index > 1 && is_option(values_[index - 1])) {
        continue;  // this is an option value
      }
      out.push_back(values_[index]);
    }
    return out;
  }

 private:
  static bool is_option(std::string_view token) {
    return token.size() > 2 && token[0] == '-' && token[1] == '-';
  }

  std::vector<std::string> values_;
};

Result<TopologyGeneration> parse_generation(std::string_view text, std::string_view what) {
  const std::optional<std::uint64_t> value = text::parse_u64(text);
  if (!value.has_value() || *value == 0) {
    return Error(ErrorCode::InvalidArgument, std::string(what) + " must be a positive generation number")
        .with_subject(std::string(text));
  }
  return TopologyGeneration(*value);
}

std::string format_optional(const std::optional<NodeId>& id) { return id.has_value() ? id->str() : std::string("-"); }

void print_usage() {
  std::cout <<
      "ftopctl " << kLibraryVersion << " - Facility Topology inspection and operations\n"
      "\n"
      "usage: ftopctl <command> [options]\n"
      "\n"
      "read-only commands:\n"
      "  version                                  print library and schema version\n"
      "  status <dir>                             report store status\n"
      "  generations <dir>                        list committed generations\n"
      "  show <dir> [--generation N]              summarise one generation\n"
      "  nodes <dir> [--kind K] [--generation N]  list node identities\n"
      "  node <dir> --id ID [--generation N]      describe one node\n"
      "  ancestry <dir> --id ID [--generation N]  physical ancestry, root first\n"
      "  descendants <dir> --id ID [--order O]    physical descendants (bfs|dfs)\n"
      "  neighbors <dir> --id ID [--generation N] physical adjacency neighbours\n"
      "  domain <dir> --id DOMAIN [--generation N] nodes referencing a domain\n"
      "  diff <dir> --from N --to M               structural difference\n"
      "  validate <file>                          validate a canonical document\n"
      "\n"
      "store commands:\n"
      "  demo <dir> [--racks N]                   create a synthetic facility store\n"
      "  import <dir> --file F --actor A [--source S] [--reason R] [--mutation-id ID]\n"
      "  apply <dir> --actor A [--source S] [--reason R] [--mutation-id ID]\n"
      "              [--base N] [--script F]\n"
      "  export <dir> --out FILE                  export the head generation\n"
      "  recover <dir>                            repair and re-point the head marker\n"
      "\n"
      "exit codes: 0 success, 1 rejected, 2 usage error\n";
}

std::string read_all(std::istream& stream) {
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// -- read-only helpers ------------------------------------------------------

Result<TopologySnapshot> load_snapshot(const TopologyStore& store, const Arguments& arguments) {
  const std::optional<std::string> requested = arguments.value("--generation");
  if (!requested.has_value()) {
    return store.head();
  }
  FT_TRY(generation, parse_generation(*requested, "--generation"));
  return store.load_generation(generation);
}

int command_version() {
  std::cout << "library-version=" << kLibraryVersion << "\n";
  std::cout << "canonical-schema=" << kCanonicalSchemaVersion << "\n";
  std::cout << "canonical-banner=" << kCanonicalBanner << "\n";
  std::cout << "node-kinds=";
  for (std::size_t index = 0; index < kNodeKindCount; ++index) {
    if (index != 0) {
      std::cout << ",";
    }
    std::cout << node_kind_token(static_cast<NodeKind>(index));
  }
  std::cout << "\n";
  return kExitOk;
}

int command_status(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=status requires a store directory\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto status = store->status();
  if (!status.has_value()) {
    print_error(status.error());
    return kExitRejected;
  }
  std::cout << "initialized=" << (status->initialized ? "yes" : "no") << "\n";
  std::cout << "writable=no\n";
  std::cout << "head=" << status->head.value() << "\n";
  std::cout << "max-committed=" << status->max_committed.value() << "\n";
  std::cout << "generations=" << status->generation_count << "\n";
  std::cout << "pending=" << status->pending_count << "\n";
  std::cout << "quarantine=" << status->quarantine_count << "\n";
  std::cout << "needs-recovery=" << (status->needs_recovery ? "yes" : "no") << "\n";
  std::cout << "explanation=" << status->explanation << "\n";
  return kExitOk;
}

int command_generations(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=generations requires a store directory\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto numbers = store->generation_numbers();
  if (!numbers.has_value()) {
    print_error(numbers.error());
    return kExitRejected;
  }
  for (const TopologyGeneration& generation : *numbers) {
    std::cout << "generation=" << generation.value() << "\n";
  }
  return kExitOk;
}

int command_show(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=show requires a store directory\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto document = store->head_document();
  if (!document.has_value()) {
    print_error(document.error());
    return kExitRejected;
  }
  auto selected = arguments.value("--generation");
  if (selected.has_value()) {
    auto generation = parse_generation(*selected, "--generation");
    if (!generation.has_value()) {
      print_error(generation.error());
      return kExitRejected;
    }
    auto other = store->load_document(generation.value());
    if (!other.has_value()) {
      print_error(other.error());
      return kExitRejected;
    }
    document = std::move(other);
  }

  const TopologySnapshot& snapshot = document->snapshot;
  const TopologyStats stats = snapshot.stats();
  std::cout << "generation=" << snapshot.generation().value() << "\n";
  std::cout << "parent=" << document->manifest.parent_generation.value() << "\n";
  std::cout << "authority-epoch=" << document->manifest.authority_epoch.value() << "\n";
  std::cout << "actor=" << document->manifest.actor.str() << "\n";
  std::cout << "source=" << document->manifest.source << "\n";
  std::cout << "recorded-at=" << document->manifest.recorded_at << "\n";
  std::cout << "content-digest=" << digest_tagged_hex(snapshot.content_digest()) << "\n";
  std::cout << "stats=" << stats.to_string() << "\n";
  return kExitOk;
}

int command_nodes(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=nodes requires a store directory\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto snapshot = load_snapshot(*store, arguments);
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }

  std::optional<NodeKind> filter;
  auto kind_option = arguments.value("--kind");
  if (kind_option.has_value()) {
    auto parsed = node_kind_parse(*kind_option);
    if (!parsed.has_value()) {
      print_error(parsed.error());
      return kExitRejected;
    }
    filter = parsed.value();
  }

  for (const NodeRecord& node : snapshot->nodes()) {
    if (filter.has_value() && node.kind != *filter) {
      continue;
    }
    std::cout << "node=" << node.id.str() << " kind=" << node_kind_token(node.kind)
              << " parent=" << format_optional(snapshot->physical_parent(node.id))
              << " zone=" << format_optional(snapshot->zone_of(node.id))
              << " label=\"" << text::escape_quoted(node.label) << "\"\n";
  }
  return kExitOk;
}

int command_node(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto id_option = arguments.value("--id");
  if (positional.size() != 1 || !id_option.has_value()) {
    std::cout << "error-message=node requires a store directory and --id\n";
    return kExitUsage;
  }
  auto id = NodeId::parse(*id_option);
  if (!id.has_value()) {
    print_error(id.error());
    return kExitRejected;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto snapshot = load_snapshot(*store, arguments);
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }
  const NodeRecord* record = snapshot->find_node(*id);
  if (record == nullptr) {
    print_error(Error(ErrorCode::NotFound, "node does not exist in this generation").with_subject(id->str()));
    return kExitRejected;
  }
  std::cout << "node=" << record->id.str() << "\n";
  std::cout << "kind=" << node_kind_token(record->kind) << "\n";
  std::cout << "label=\"" << text::escape_quoted(record->label) << "\"\n";
  if (record->zone_member_kind.has_value()) {
    std::cout << "zone-member-kind=" << node_kind_token(*record->zone_member_kind) << "\n";
  }
  std::cout << "physical-parent=" << format_optional(snapshot->physical_parent(record->id)) << "\n";
  std::cout << "zone=" << format_optional(snapshot->zone_of(record->id)) << "\n";
  std::cout << "facility=" << format_optional(snapshot->facility_of(record->id)) << "\n";
  std::cout << "depth=";
  const std::optional<std::uint32_t> depth = snapshot->depth_of(record->id);
  if (depth.has_value()) {
    std::cout << *depth;
  } else {
    std::cout << "-";
  }
  std::cout << "\n";
  std::cout << "provenance-actor=" << record->provenance.actor.str() << "\n";
  std::cout << "provenance-source=" << record->provenance.source << "\n";
  std::cout << "provenance-recorded-at=" << record->provenance.recorded_at << "\n";
  std::cout << "children=";
  const std::vector<NodeId> children = snapshot->children(record->id);
  for (std::size_t index = 0; index < children.size(); ++index) {
    if (index != 0) {
      std::cout << ",";
    }
    std::cout << children[index].str();
  }
  std::cout << "\n";
  std::cout << "zone-members=";
  const std::vector<NodeId> members = snapshot->zone_members(record->id);
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (index != 0) {
      std::cout << ",";
    }
    std::cout << members[index].str();
  }
  std::cout << "\n";
  for (const DomainAssociation& association : snapshot->associations_of(record->id)) {
    std::cout << "domain=" << association.domain.str() << " kind=" << domain_kind_token(association.kind) << "\n";
  }
  return kExitOk;
}

int command_ancestry(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto id_option = arguments.value("--id");
  if (positional.size() != 1 || !id_option.has_value()) {
    std::cout << "error-message=ancestry requires a store directory and --id\n";
    return kExitUsage;
  }
  auto id = NodeId::parse(*id_option);
  if (!id.has_value()) {
    print_error(id.error());
    return kExitRejected;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto snapshot = load_snapshot(*store, arguments);
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }
  if (!snapshot->contains_node(*id)) {
    print_error(Error(ErrorCode::NotFound, "node does not exist in this generation").with_subject(id->str()));
    return kExitRejected;
  }
  std::size_t depth = 0;
  for (const NodeId& ancestor : snapshot->ancestry(*id)) {
    std::cout << "ancestor=" << ancestor.str() << " depth=" << depth++ << "\n";
  }
  return kExitOk;
}

int command_descendants(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto id_option = arguments.value("--id");
  if (positional.size() != 1 || !id_option.has_value()) {
    std::cout << "error-message=descendants requires a store directory and --id\n";
    return kExitUsage;
  }
  auto id = NodeId::parse(*id_option);
  if (!id.has_value()) {
    print_error(id.error());
    return kExitRejected;
  }
  TraversalOrder order = TraversalOrder::BreadthFirst;
  auto order_option = arguments.value("--order");
  if (order_option.has_value()) {
    auto parsed = traversal_order_parse(*order_option);
    if (!parsed.has_value()) {
      print_error(parsed.error());
      return kExitRejected;
    }
    order = parsed.value();
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto snapshot = load_snapshot(*store, arguments);
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }
  TraversalLimits bounds;
  bounds.max_depth = 64;
  bounds.max_nodes = 1'000'000;
  auto visited = snapshot->traverse(*id, order, bounds);
  if (!visited.has_value()) {
    print_error(visited.error());
    return kExitRejected;
  }
  std::cout << "order=" << traversal_order_token(order) << "\n";
  std::size_t index = 0;
  for (const NodeId& node : *visited) {
    std::cout << "descendant=" << node.str() << " index=" << index++ << "\n";
  }
  return kExitOk;
}

int command_neighbors(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto id_option = arguments.value("--id");
  if (positional.size() != 1 || !id_option.has_value()) {
    std::cout << "error-message=neighbors requires a store directory and --id\n";
    return kExitUsage;
  }
  auto id = NodeId::parse(*id_option);
  if (!id.has_value()) {
    print_error(id.error());
    return kExitRejected;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto snapshot = load_snapshot(*store, arguments);
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }
  if (!snapshot->contains_node(*id)) {
    print_error(Error(ErrorCode::NotFound, "node does not exist in this generation").with_subject(id->str()));
    return kExitRejected;
  }
  for (const AdjacencyEdge& edge : snapshot->adjacency_of(*id)) {
    const NodeId& other = (edge.first == *id) ? edge.second : edge.first;
    std::cout << "neighbor=" << other.str() << " kind=" << adjacency_kind_token(edge.kind) << "\n";
  }
  return kExitOk;
}

int command_domain(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto id_option = arguments.value("--id");
  if (positional.size() != 1 || !id_option.has_value()) {
    std::cout << "error-message=domain requires a store directory and --id\n";
    return kExitUsage;
  }
  auto id = DomainId::parse(*id_option);
  if (!id.has_value()) {
    print_error(id.error());
    return kExitRejected;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto snapshot = load_snapshot(*store, arguments);
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }
  const DomainDeclaration* declaration = snapshot->find_domain(*id);
  std::cout << "declared=" << (declaration != nullptr ? "yes" : "no") << "\n";
  if (declaration != nullptr) {
    std::cout << "kind=" << domain_kind_token(declaration->kind) << "\n";
    std::cout << "label=\"" << text::escape_quoted(declaration->label) << "\"\n";
  }
  for (const NodeId& node : snapshot->nodes_in_domain(*id)) {
    std::cout << "member=" << node.str() << "\n";
  }
  return kExitOk;
}

int command_diff(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto from_option = arguments.value("--from");
  auto to_option = arguments.value("--to");
  if (positional.size() != 1 || !from_option.has_value() || !to_option.has_value()) {
    std::cout << "error-message=diff requires a store directory, --from and --to\n";
    return kExitUsage;
  }
  auto from_generation = parse_generation(*from_option, "--from");
  if (!from_generation.has_value()) {
    print_error(from_generation.error());
    return kExitRejected;
  }
  auto to_generation = parse_generation(*to_option, "--to");
  if (!to_generation.has_value()) {
    print_error(to_generation.error());
    return kExitRejected;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto before = store->load_generation(from_generation.value());
  if (!before.has_value()) {
    print_error(before.error());
    return kExitRejected;
  }
  auto after = store->load_generation(to_generation.value());
  if (!after.has_value()) {
    print_error(after.error());
    return kExitRejected;
  }
  auto changes = diff(*before, *after);
  if (!changes.has_value()) {
    print_error(changes.error());
    return kExitRejected;
  }
  std::cout << changes->to_string();
  return kExitOk;
}

int command_validate(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=validate requires a document path\n";
    return kExitUsage;
  }
  auto document = TopologyStore::import_from(positional[0]);
  if (!document.has_value()) {
    print_error(document.error());
    return kExitRejected;
  }
  std::cout << "valid=yes\n";
  std::cout << "generation=" << document->manifest.generation.value() << "\n";
  std::cout << "stats=" << document->snapshot.stats().to_string() << "\n";
  std::cout << "content-digest=" << digest_tagged_hex(document->snapshot.content_digest()) << "\n";
  return kExitOk;
}

// -- store commands ---------------------------------------------------------

/// Builds a synthetic facility. The topology is synthetic: it describes no real
/// site and claims no hardware integration.
Result<TopologySnapshot> synthetic_facility(std::size_t racks_per_row, const TopologyGeneration& generation) {
  std::vector<NodeRecord> nodes;
  std::vector<ContainmentEdge> containment;
  std::vector<AdjacencyEdge> adjacency;
  std::vector<DomainDeclaration> domains;
  std::vector<DomainAssociation> associations;

  const ProvenanceRecord provenance_record = [] {
    ProvenanceRecord record;
    record.actor = ActorId::parse("cli").value();
    record.source = "cli";
    record.reason = "synthetic demo topology";
    return record;
  }();
  const auto node = [&provenance_record](std::string id, NodeKind kind, std::string label,
                                         std::optional<NodeKind> scope = std::nullopt) {
    NodeRecord record;
    record.id = NodeId::parse(id).value();
    record.kind = kind;
    record.label = std::move(label);
    record.zone_member_kind = scope;
    record.provenance = provenance_record;
    return record;
  };
  const auto link = [&provenance_record](std::string parent, std::string child, BoundaryKind boundary) {
    ContainmentEdge edge;
    edge.parent = NodeId::parse(parent).value();
    edge.child = NodeId::parse(child).value();
    edge.boundary = boundary;
    edge.provenance = provenance_record;
    return edge;
  };
  const auto adjacent = [&provenance_record](std::string first, std::string second, AdjacencyKind kind) {
    AdjacencyEdge edge;
    edge.first = NodeId::parse(first).value();
    edge.second = NodeId::parse(second).value();
    edge.kind = kind;
    edge.provenance = provenance_record;
    return edge;
  };

  nodes.push_back(node("fac-demo", NodeKind::Facility, "Synthetic Facility"));
  domains.push_back(DomainDeclaration{DomainId::parse("pwr-a").value(), DomainKind::Power, "Synthetic feed A",
                                      provenance_record});
  domains.push_back(DomainDeclaration{DomainId::parse("cool-a").value(), DomainKind::Cooling, "Synthetic loop A",
                                      provenance_record});

  for (int building = 1; building <= 2; ++building) {
    const std::string building_id = "bld-" + std::to_string(building);
    nodes.push_back(node(building_id, NodeKind::Building, "Building " + std::to_string(building)));
    containment.push_back(link("fac-demo", building_id, BoundaryKind::Physical));
    for (int hall = 1; hall <= 2; ++hall) {
      const std::string hall_id = building_id + "-hall-" + std::to_string(hall);
      nodes.push_back(node(hall_id, NodeKind::Hall, "Hall " + std::to_string(hall)));
      containment.push_back(link(building_id, hall_id, BoundaryKind::Physical));
      associations.push_back(DomainAssociation{NodeId::parse(hall_id).value(), DomainId::parse("cool-a").value(),
                                               DomainKind::Cooling, provenance_record});
      const std::string room_id = hall_id + "-room";
      nodes.push_back(node(room_id, NodeKind::Room, "Room " + std::to_string(hall)));
      containment.push_back(link(hall_id, room_id, BoundaryKind::Physical));
      for (int row = 1; row <= 4; ++row) {
        const std::string row_id = room_id + "-row-" + std::to_string(row);
        nodes.push_back(node(row_id, NodeKind::Row, "Row " + std::to_string(row)));
        containment.push_back(link(room_id, row_id, BoundaryKind::Physical));
        for (std::size_t rack = 1; rack <= racks_per_row; ++rack) {
          const std::string rack_id = row_id + "-rack-" + std::to_string(rack);
          nodes.push_back(node(rack_id, NodeKind::Rack, "Rack " + std::to_string(rack)));
          containment.push_back(link(row_id, rack_id, BoundaryKind::Physical));
          associations.push_back(DomainAssociation{NodeId::parse(rack_id).value(), DomainId::parse("pwr-a").value(),
                                                   DomainKind::Power, provenance_record});
          if (rack > 1) {
            adjacency.push_back(adjacent(row_id + "-rack-" + std::to_string(rack - 1), rack_id,
                                         AdjacencyKind::StructuralNeighbor));
          }
        }
      }
    }
  }
  return assemble_topology(std::move(nodes), std::move(containment), std::move(adjacency), std::move(domains),
                           std::move(associations), generation);
}

int command_demo(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=demo requires a store directory\n";
    return kExitUsage;
  }
  std::size_t racks_per_row = 4;
  auto racks_option = arguments.value("--racks");
  if (racks_option.has_value()) {
    auto parsed = text::parse_u64(*racks_option);
    if (!parsed.has_value() || *parsed == 0 || *parsed > 64) {
      std::cout << "error-message=--racks must be between 1 and 64\n";
      return kExitUsage;
    }
    racks_per_row = static_cast<std::size_t>(*parsed);
  }

  auto store = TopologyStore::open(positional[0], OpenMode::ReadWrite, StoreConfig{});
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  CreateOptions options;
  options.actor = ActorId::parse("cli").value();
  options.source = "cli";
  options.reason = "synthetic demo topology (not a real site)";
  options.mutation_id = MutationId::parse("demo-initial").value();
  auto snapshot = synthetic_facility(racks_per_row, TopologyGeneration(1));
  if (!snapshot.has_value()) {
    print_error(snapshot.error());
    return kExitRejected;
  }
  auto outcome = store->create(*snapshot, options);
  if (!outcome.has_value()) {
    print_error(outcome.error());
    return kExitRejected;
  }
  std::cout << "generation=" << outcome->new_generation.value() << "\n";
  std::cout << "stats=" << snapshot->stats().to_string() << "\n";
  std::cout << "content-digest=" << digest_tagged_hex(snapshot->content_digest()) << "\n";
  std::cout << "topology=synthetic\n";
  std::cout << "store=" << positional[0] << "\n";
  return kExitOk;
}

struct BatchMetadata {
  ActorId actor;
  std::string source;
  std::string reason;
  MutationId mutation_id;
};

Result<BatchMetadata> batch_metadata(const Arguments& arguments) {
  auto actor_option = arguments.value("--actor");
  if (!actor_option.has_value()) {
    return Error(ErrorCode::MissingField, "--actor is required for a mutation");
  }
  FT_TRY(actor, ActorId::parse(*actor_option));
  const auto source_option = arguments.value("--source");
  const auto reason_option = arguments.value("--reason");
  const auto id_option = arguments.value("--mutation-id");

  BatchMetadata metadata;
  metadata.actor = actor;
  metadata.source = source_option.has_value() ? *source_option : std::string("cli");
  metadata.reason = reason_option.has_value() ? *reason_option : std::string();
  if (id_option.has_value()) {
    FT_TRY(id, MutationId::parse(*id_option));
    metadata.mutation_id = id;
  } else {
    // The identity is what makes a durable commit retryable; the tool refuses
    // to invent one because an invented identity has no meaning to the caller.
    return Error(ErrorCode::MissingField, "--mutation-id is required for a durable mutation");
  }
  return metadata;
}

int command_import(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto file_option = arguments.value("--file");
  if (positional.size() != 1 || !file_option.has_value()) {
    std::cout << "error-message=import requires a store directory and --file\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadWrite, StoreConfig{});
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto document = TopologyStore::import_from(*file_option, store->limits());
  if (!document.has_value()) {
    print_error(document.error());
    return kExitRejected;
  }
  CreateOptions options;
  options.actor = ActorId::parse("cli").value();
  options.source = "cli";
  options.reason = "imported canonical document";
  auto actor_option = arguments.value("--actor");
  if (actor_option.has_value()) {
    auto actor = ActorId::parse(*actor_option);
    if (!actor.has_value()) {
      print_error(actor.error());
      return kExitRejected;
    }
    options.actor = actor.value();
  }
  auto source_option = arguments.value("--source");
  if (source_option.has_value()) {
    options.source = *source_option;
  }
  auto reason_option = arguments.value("--reason");
  if (reason_option.has_value()) {
    options.reason = *reason_option;
  }
  auto id_option = arguments.value("--mutation-id");
  if (!id_option.has_value()) {
    std::cout << "error-message=--mutation-id is required for a durable mutation\n";
    return kExitUsage;
  }
  auto identity = MutationId::parse(*id_option);
  if (!identity.has_value()) {
    print_error(identity.error());
    return kExitRejected;
  }
  options.mutation_id = identity.value();

  auto outcome = store->create(document->snapshot, options);
  if (!outcome.has_value()) {
    print_error(outcome.error());
    return kExitRejected;
  }
  std::cout << "generation=" << outcome->new_generation.value() << "\n";
  std::cout << "source-generation=" << document->manifest.generation.value() << "\n";
  std::cout << "stats=" << document->snapshot.stats().to_string() << "\n";
  return kExitOk;
}

int command_apply(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=apply requires a store directory\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadWrite, StoreConfig{});
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto metadata = batch_metadata(arguments);
  if (!metadata.has_value()) {
    print_error(metadata.error());
    return kExitRejected;
  }

  std::string script_text;
  auto script_option = arguments.value("--script");
  if (script_option.has_value()) {
    std::ifstream stream(*script_option, std::ios::binary);
    if (!stream) {
      print_error(Error(ErrorCode::StoreNotFound, "cannot open script file").with_subject(*script_option));
      return kExitRejected;
    }
    script_text = read_all(stream);
    if (script_text.size() > store->limits().max_document_bytes) {
      print_error(Error(ErrorCode::LimitExceeded, "script exceeds the configured maximum size"));
      return kExitRejected;
    }
  }
  auto mutations = ftopctl::parse_script(script_text, store->limits());
  if (!mutations.has_value()) {
    print_error(mutations.error());
    return kExitRejected;
  }

  TopologyGeneration base;
  auto base_option = arguments.value("--base");
  if (base_option.has_value()) {
    auto parsed = parse_generation(*base_option, "--base");
    if (!parsed.has_value()) {
      print_error(parsed.error());
      return kExitRejected;
    }
    base = parsed.value();
  } else {
    auto head = store->head_generation();
    if (!head.has_value()) {
      print_error(head.error());
      return kExitRejected;
    }
    base = head.value();
  }

  MutationBatch batch;
  batch.base_generation = base;
  batch.authority_epoch = store->epoch();
  batch.actor = metadata->actor;
  batch.source = metadata->source;
  batch.reason = metadata->reason;
  batch.mutation_id = metadata->mutation_id;
  batch.mutations = std::move(*mutations);

  auto outcome = store->commit(batch);
  if (!outcome.has_value()) {
    print_error(outcome.error());
    return kExitRejected;
  }
  std::cout << "outcome=" << outcome->to_string() << "\n";
  std::cout << "explanation=" << outcome->explanation << "\n";
  return kExitOk;
}

int command_export(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  auto out_option = arguments.value("--out");
  if (positional.size() != 1 || !out_option.has_value()) {
    std::cout << "error-message=export requires a store directory and --out\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadOnly);
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto result = store->export_head_to(*out_option);
  if (!result.has_value()) {
    print_error(result.error());
    return kExitRejected;
  }
  auto head = store->head_generation();
  std::cout << "exported=" << *out_option << "\n";
  if (head.has_value()) {
    std::cout << "generation=" << head->value() << "\n";
  }
  return kExitOk;
}

int command_recover(const Arguments& arguments) {
  const std::vector<std::string> positional = arguments.positional();
  if (positional.size() != 1) {
    std::cout << "error-message=recover requires a store directory\n";
    return kExitUsage;
  }
  auto store = TopologyStore::open(positional[0], OpenMode::ReadWrite, StoreConfig{});
  if (!store.has_value()) {
    print_error(store.error());
    return kExitRejected;
  }
  auto report = store->recover();
  if (!report.has_value()) {
    print_error(report.error());
    return kExitRejected;
  }
  std::cout << "repaired=" << (report->repaired ? "yes" : "no") << "\n";
  std::cout << "recovered=" << (report->recovered ? "yes" : "no") << "\n";
  std::cout << "head-before=" << report->head_before.value() << "\n";
  std::cout << "head-after=" << report->head_after.value() << "\n";
  std::cout << "pending-discarded=" << report->pending_files_discarded << "\n";
  std::cout << "quarantined=" << report->corrupt_files_quarantined << "\n";
  std::cout << "pruned=" << report->generations_pruned << "\n";
  for (const TopologyGeneration& generation : report->retained) {
    std::cout << "retained=" << generation.value() << "\n";
  }
  std::cout << "explanation=" << report->explanation << "\n";
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> values;
  values.reserve(static_cast<std::size_t>(argc));
  for (int index = 1; index < argc; ++index) {
    values.emplace_back(argv[index]);
  }
  if (values.empty()) {
    print_usage();
    return kExitUsage;
  }
  const Arguments arguments(std::move(values));
  const std::string command = arguments.front();

  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return kExitOk;
  }
  if (command == "version") {
    return command_version();
  }
  if (command == "status") {
    return command_status(arguments);
  }
  if (command == "generations") {
    return command_generations(arguments);
  }
  if (command == "show") {
    return command_show(arguments);
  }
  if (command == "nodes") {
    return command_nodes(arguments);
  }
  if (command == "node") {
    return command_node(arguments);
  }
  if (command == "ancestry") {
    return command_ancestry(arguments);
  }
  if (command == "descendants") {
    return command_descendants(arguments);
  }
  if (command == "neighbors") {
    return command_neighbors(arguments);
  }
  if (command == "domain") {
    return command_domain(arguments);
  }
  if (command == "diff") {
    return command_diff(arguments);
  }
  if (command == "validate") {
    return command_validate(arguments);
  }
  if (command == "demo") {
    return command_demo(arguments);
  }
  if (command == "import") {
    return command_import(arguments);
  }
  if (command == "apply") {
    return command_apply(arguments);
  }
  if (command == "export") {
    return command_export(arguments);
  }
  if (command == "recover") {
    return command_recover(arguments);
  }

  std::cout << "error-message=unknown command \"" << command << "\"\n";
  print_usage();
  return kExitUsage;
}
