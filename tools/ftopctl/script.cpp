// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "script.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/facility_topology/model.hpp"
#include "dccp/facility_topology/strong_id.hpp"
#include "dccp/facility_topology/text.hpp"

namespace ftopctl {

using namespace dccp::facility_topology;

namespace {

Result<std::string_view> next_token(std::string_view line, std::size_t& cursor) {
  while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
    ++cursor;
  }
  if (cursor >= line.size()) {
    return Error(ErrorCode::MissingField, "missing field");
  }
  if (line[cursor] == '"') {
    const std::size_t start = cursor;
    ++cursor;
    while (cursor < line.size()) {
      if (line[cursor] == '\\') {
        cursor += 2;
        continue;
      }
      if (line[cursor] == '"') {
        ++cursor;
        return line.substr(start, cursor - start);
      }
      ++cursor;
    }
    return Error(ErrorCode::MalformedRecord, "unterminated quoted field");
  }
  const std::size_t start = cursor;
  while (cursor < line.size() && line[cursor] != ' ' && line[cursor] != '\t') {
    ++cursor;
  }
  return line.substr(start, cursor - start);
}

bool at_end(std::string_view line, std::size_t cursor) {
  while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
    ++cursor;
  }
  return cursor >= line.size();
}

Result<std::string> decode_label(std::string_view token, std::string_view what) {
  if (token.size() < 2 || token.front() != '"' || token.back() != '"') {
    return Error(ErrorCode::MalformedRecord, std::string(what) + " must be a quoted string");
  }
  return text::unescape_quoted(token.substr(1, token.size() - 2), 1U << 16);
}

Result<NodeId> parse_node(std::string_view token) { return NodeId::parse(token); }
Result<DomainId> parse_domain(std::string_view token) { return DomainId::parse(token); }

ProvenanceRecord script_provenance(std::string_view actor, std::string_view source) {
  ProvenanceRecord record;
  record.actor = ActorId::parse(actor).value();
  record.source = std::string(source);
  record.reason = "mutation script";
  return record;
}

}  // namespace

Result<std::vector<Mutation>> parse_script(std::string_view script, const TopologyLimits& limits) {
  std::vector<Mutation> mutations;
  std::size_t line_number = 0;
  std::size_t position = 0;
  while (position <= script.size()) {
    if (position == script.size()) {
      break;
    }
    const std::size_t newline = script.find('\n', position);
    const std::string_view raw =
        (newline == std::string_view::npos) ? script.substr(position) : script.substr(position, newline - position);
    position = (newline == std::string_view::npos) ? script.size() : newline + 1;
    ++line_number;

    const std::string_view line = text::trim(raw);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (line.size() > limits.max_line_bytes) {
      return Error(ErrorCode::LimitExceeded, "script line exceeds the configured maximum length")
          .with_subject(std::to_string(line_number));
    }
    if (mutations.size() >= limits.max_mutations_per_batch) {
      return Error(ErrorCode::LimitExceeded, "script exceeds the configured maximum number of mutations");
    }

    std::size_t cursor = 0;
    FT_TRY(command_token, next_token(line, cursor));
    const std::string_view command = command_token;
    const ProvenanceRecord provenance = script_provenance("cli", "cli");

    const auto fail = [line_number](std::string message) {
      return Error(ErrorCode::MalformedRecord, "script line " + std::to_string(line_number) + ": " + std::move(message));
    };
    const auto expect_end = [&](std::string_view what) -> Result<void> {
      if (!at_end(line, cursor)) {
        return fail("unexpected trailing content after " + std::string(what));
      }
      return ok();
    };

    if (command == "add-node") {
      FT_TRY(id_token, next_token(line, cursor));
      FT_TRY(id, parse_node(id_token));
      FT_TRY(kind_token, next_token(line, cursor));
      FT_TRY(kind, node_kind_parse(kind_token));
      NodeRecord record;
      record.id = id;
      record.kind = kind;
      record.provenance = provenance;
      if (!at_end(line, cursor)) {
        FT_TRY(label_token, next_token(line, cursor));
        FT_TRY(label, decode_label(label_token, "node label"));
        record.label = std::move(label);
      }
      if (!at_end(line, cursor)) {
        FT_TRY(scope_token, next_token(line, cursor));
        FT_TRY(scope, node_kind_parse(scope_token));
        record.zone_member_kind = scope;
      }
      FT_TRYV(expect_end("add-node"));
      mutations.push_back(AddNode{std::move(record)});
      continue;
    }
    if (command == "remove-node") {
      FT_TRY(id_token, next_token(line, cursor));
      FT_TRY(id, parse_node(id_token));
      FT_TRYV(expect_end("remove-node"));
      mutations.push_back(RemoveNode{id});
      continue;
    }
    if (command == "move-node") {
      FT_TRY(id_token, next_token(line, cursor));
      FT_TRY(id, parse_node(id_token));
      FT_TRY(parent_token, next_token(line, cursor));
      FT_TRY(parent, parse_node(parent_token));
      FT_TRYV(expect_end("move-node"));
      mutations.push_back(MoveNode{id, parent, provenance});
      continue;
    }
    if (command == "add-containment") {
      FT_TRY(parent_token, next_token(line, cursor));
      FT_TRY(parent, parse_node(parent_token));
      FT_TRY(child_token, next_token(line, cursor));
      FT_TRY(child, parse_node(child_token));
      BoundaryKind boundary = BoundaryKind::Physical;
      if (!at_end(line, cursor)) {
        FT_TRY(boundary_token, next_token(line, cursor));
        FT_TRY(parsed, boundary_kind_parse(boundary_token));
        boundary = parsed;
      }
      FT_TRYV(expect_end("add-containment"));
      ContainmentEdge edge;
      edge.parent = parent;
      edge.child = child;
      edge.boundary = boundary;
      edge.provenance = provenance;
      mutations.push_back(AddContainment{std::move(edge)});
      continue;
    }
    if (command == "remove-containment") {
      FT_TRY(parent_token, next_token(line, cursor));
      FT_TRY(parent, parse_node(parent_token));
      FT_TRY(child_token, next_token(line, cursor));
      FT_TRY(child, parse_node(child_token));
      FT_TRYV(expect_end("remove-containment"));
      mutations.push_back(RemoveContainment{parent, child});
      continue;
    }
    if (command == "set-label") {
      FT_TRY(id_token, next_token(line, cursor));
      FT_TRY(id, parse_node(id_token));
      FT_TRY(label_token, next_token(line, cursor));
      FT_TRY(label, decode_label(label_token, "node label"));
      FT_TRYV(expect_end("set-label"));
      mutations.push_back(SetNodeLabel{id, std::move(label), provenance});
      continue;
    }
    if (command == "add-adjacency" || command == "remove-adjacency") {
      FT_TRY(first_token, next_token(line, cursor));
      FT_TRY(first, parse_node(first_token));
      FT_TRY(second_token, next_token(line, cursor));
      FT_TRY(second, parse_node(second_token));
      AdjacencyKind kind = AdjacencyKind::SharedBoundary;
      if (!at_end(line, cursor)) {
        FT_TRY(kind_token, next_token(line, cursor));
        FT_TRY(parsed, adjacency_kind_parse(kind_token));
        kind = parsed;
      }
      FT_TRYV(expect_end(command));
      if (command == "add-adjacency") {
        AdjacencyEdge edge;
        edge.first = first;
        edge.second = second;
        edge.kind = kind;
        edge.provenance = provenance;
        // Endpoint order is normalized by the library, which rejects the
        // reversed spelling in canonical documents.
        mutations.push_back(AddAdjacency{std::move(edge)});
      } else {
        mutations.push_back(RemoveAdjacency{first, second, kind});
      }
      continue;
    }
    if (command == "declare-domain") {
      FT_TRY(id_token, next_token(line, cursor));
      FT_TRY(id, parse_domain(id_token));
      FT_TRY(kind_token, next_token(line, cursor));
      FT_TRY(kind, domain_kind_parse(kind_token));
      DomainDeclaration declaration;
      declaration.id = id;
      declaration.kind = kind;
      declaration.provenance = provenance;
      if (!at_end(line, cursor)) {
        FT_TRY(label_token, next_token(line, cursor));
        FT_TRY(label, decode_label(label_token, "domain label"));
        declaration.label = std::move(label);
      }
      FT_TRYV(expect_end("declare-domain"));
      mutations.push_back(DeclareDomain{std::move(declaration)});
      continue;
    }
    if (command == "retire-domain") {
      FT_TRY(id_token, next_token(line, cursor));
      FT_TRY(id, parse_domain(id_token));
      FT_TRYV(expect_end("retire-domain"));
      mutations.push_back(RetireDomain{id});
      continue;
    }
    if (command == "add-association") {
      FT_TRY(node_token, next_token(line, cursor));
      FT_TRY(node, parse_node(node_token));
      FT_TRY(domain_token, next_token(line, cursor));
      FT_TRY(domain, parse_domain(domain_token));
      FT_TRY(kind_token, next_token(line, cursor));
      FT_TRY(kind, domain_kind_parse(kind_token));
      FT_TRYV(expect_end("add-association"));
      DomainAssociation association;
      association.node = node;
      association.domain = domain;
      association.kind = kind;
      association.provenance = provenance;
      mutations.push_back(AddAssociation{std::move(association)});
      continue;
    }
    if (command == "remove-association") {
      FT_TRY(node_token, next_token(line, cursor));
      FT_TRY(node, parse_node(node_token));
      FT_TRY(domain_token, next_token(line, cursor));
      FT_TRY(domain, parse_domain(domain_token));
      FT_TRYV(expect_end("remove-association"));
      mutations.push_back(RemoveAssociation{node, domain});
      continue;
    }
    return fail("unknown command \"" + std::string(command) + "\"");
  }
  return mutations;
}

}  // namespace ftopctl
