// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/model.hpp"

#include <array>
#include <cstddef>
#include <string>

#include "dccp/facility_topology/clock.hpp"

namespace dccp::facility_topology {
namespace {

struct TokenName {
  std::string_view token;
  std::uint8_t value;
};

template <std::size_t N>
Result<std::uint8_t> parse_token(const TokenName (&table)[N], std::string_view token, std::string_view domain) {
  for (const TokenName& entry : table) {
    if (entry.token == token) {
      return entry.value;
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown " + std::string(domain) + " token")
      .with_subject(std::string(token.substr(0, 64)));
}

constexpr TokenName kNodeKindTokens[] = {
    {"facility", static_cast<std::uint8_t>(NodeKind::Facility)},
    {"building", static_cast<std::uint8_t>(NodeKind::Building)},
    {"hall", static_cast<std::uint8_t>(NodeKind::Hall)},
    {"room", static_cast<std::uint8_t>(NodeKind::Room)},
    {"row", static_cast<std::uint8_t>(NodeKind::Row)},
    {"rack", static_cast<std::uint8_t>(NodeKind::Rack)},
    {"zone", static_cast<std::uint8_t>(NodeKind::Zone)},
};

constexpr TokenName kBoundaryKindTokens[] = {
    {"physical", static_cast<std::uint8_t>(BoundaryKind::Physical)},
    {"logical", static_cast<std::uint8_t>(BoundaryKind::Logical)},
};

constexpr TokenName kAdjacencyKindTokens[] = {
    {"shared-boundary", static_cast<std::uint8_t>(AdjacencyKind::SharedBoundary)},
    {"service-aisle", static_cast<std::uint8_t>(AdjacencyKind::ServiceAisle)},
    {"structural-neighbor", static_cast<std::uint8_t>(AdjacencyKind::StructuralNeighbor)},
};

constexpr TokenName kDomainKindTokens[] = {
    {"power", static_cast<std::uint8_t>(DomainKind::Power)},
    {"cooling", static_cast<std::uint8_t>(DomainKind::Cooling)},
};

constexpr NodeKindMask kNoKinds = 0;

constexpr NodeKindMask without(NodeKindMask mask, NodeKind kind) noexcept {
  return static_cast<NodeKindMask>(mask & static_cast<NodeKindMask>(~node_kind_bit(kind)));
}

constexpr NodeKindMask kBuildingOrDeeper =
    node_kind_bit(NodeKind::Building) | node_kind_bit(NodeKind::Hall) | node_kind_bit(NodeKind::Room) |
    node_kind_bit(NodeKind::Row) | node_kind_bit(NodeKind::Rack) | node_kind_bit(NodeKind::Zone);
constexpr NodeKindMask kHallOrDeeper = node_kind_bit(NodeKind::Hall) | node_kind_bit(NodeKind::Room) |
                                       node_kind_bit(NodeKind::Row) | node_kind_bit(NodeKind::Rack) |
                                       node_kind_bit(NodeKind::Zone);
constexpr NodeKindMask kHallRoomRowRack =
    node_kind_bit(NodeKind::Hall) | node_kind_bit(NodeKind::Room) | node_kind_bit(NodeKind::Row) |
    node_kind_bit(NodeKind::Rack);

// The single authoritative containment schema. Index == NodeKind value.
constexpr std::array<NodeKindSchema, kNodeKindCount> kSchemas = {{
    // Facility: the only root-eligible kind; may hold any non-facility kind.
    {NodeKind::Facility, 0, true, kNoKinds, kBuildingOrDeeper, false},
    {NodeKind::Building,
     1,
     false,
     node_kind_bit(NodeKind::Facility),
     without(kHallOrDeeper, NodeKind::Zone),
     false},
    {NodeKind::Hall,
     2,
     false,
     node_kind_bit(NodeKind::Facility) | node_kind_bit(NodeKind::Building),
     node_kind_bit(NodeKind::Room) | node_kind_bit(NodeKind::Row) | node_kind_bit(NodeKind::Rack),
     true},
    {NodeKind::Room,
     3,
     false,
     node_kind_bit(NodeKind::Facility) | node_kind_bit(NodeKind::Building) | node_kind_bit(NodeKind::Hall),
     node_kind_bit(NodeKind::Row) | node_kind_bit(NodeKind::Rack),
     true},
    {NodeKind::Row,
     4,
     false,
     node_kind_bit(NodeKind::Facility) | node_kind_bit(NodeKind::Building) | node_kind_bit(NodeKind::Hall) |
         node_kind_bit(NodeKind::Room),
     node_kind_bit(NodeKind::Rack),
     true},
    {NodeKind::Rack,
     5,
     false,
     node_kind_bit(NodeKind::Facility) | node_kind_bit(NodeKind::Building) | node_kind_bit(NodeKind::Hall) |
         node_kind_bit(NodeKind::Room) | node_kind_bit(NodeKind::Row),
     kNoKinds,
     true},
    // Zone: a logical boundary declared directly under one facility. Members
    // are attached with Logical containment edges, never Physical ones.
    {NodeKind::Zone, 1, false, node_kind_bit(NodeKind::Facility), kNoKinds, false},
}};

constexpr NodeKindMask kAdjacencyKinds = node_kind_bit(NodeKind::Building) | node_kind_bit(NodeKind::Hall) |
                                         node_kind_bit(NodeKind::Room) | node_kind_bit(NodeKind::Row) |
                                         node_kind_bit(NodeKind::Rack);

static_assert(kHallRoomRowRack != 0, "schema tables must be non-empty");

std::string describe_mismatch(std::string_view field, std::string_view value, std::string_view rule) {
  return std::string(field) + " \"" + std::string(value) + "\" " + std::string(rule);
}

}  // namespace

std::string_view node_kind_token(NodeKind kind) noexcept {
  for (const TokenName& entry : kNodeKindTokens) {
    if (entry.value == static_cast<std::uint8_t>(kind)) {
      return entry.token;
    }
  }
  return "unknown";
}

Result<NodeKind> node_kind_parse(std::string_view token) {
  FT_TRY(value, parse_token(kNodeKindTokens, token, "node kind"));
  return static_cast<NodeKind>(value);
}

std::string_view boundary_kind_token(BoundaryKind kind) noexcept {
  for (const TokenName& entry : kBoundaryKindTokens) {
    if (entry.value == static_cast<std::uint8_t>(kind)) {
      return entry.token;
    }
  }
  return "unknown";
}

Result<BoundaryKind> boundary_kind_parse(std::string_view token) {
  FT_TRY(value, parse_token(kBoundaryKindTokens, token, "boundary kind"));
  return static_cast<BoundaryKind>(value);
}

std::string_view adjacency_kind_token(AdjacencyKind kind) noexcept {
  for (const TokenName& entry : kAdjacencyKindTokens) {
    if (entry.value == static_cast<std::uint8_t>(kind)) {
      return entry.token;
    }
  }
  return "unknown";
}

Result<AdjacencyKind> adjacency_kind_parse(std::string_view token) {
  FT_TRY(value, parse_token(kAdjacencyKindTokens, token, "adjacency kind"));
  return static_cast<AdjacencyKind>(value);
}

std::string_view domain_kind_token(DomainKind kind) noexcept {
  for (const TokenName& entry : kDomainKindTokens) {
    if (entry.value == static_cast<std::uint8_t>(kind)) {
      return entry.token;
    }
  }
  return "unknown";
}

Result<DomainKind> domain_kind_parse(std::string_view token) {
  FT_TRY(value, parse_token(kDomainKindTokens, token, "domain kind"));
  return static_cast<DomainKind>(value);
}

const NodeKindSchema& node_kind_schema(NodeKind kind) noexcept {
  const std::size_t index = node_kind_index(kind);
  return kSchemas[index < kSchemas.size() ? index : 0];
}

bool schema_allows_physical_child(NodeKind parent, NodeKind child) noexcept {
  return (node_kind_schema(parent).allowed_child_kinds & node_kind_bit(child)) != 0;
}

bool schema_allows_adjacency(NodeKind lhs, NodeKind rhs) noexcept {
  return ((kAdjacencyKinds & node_kind_bit(lhs)) != 0) && ((kAdjacencyKinds & node_kind_bit(rhs)) != 0);
}

bool is_canonical_utc_timestamp(std::string_view value) noexcept {
  // The strict parser is the single authority on what a canonical timestamp
  // is: exact shape, real calendar fields (including leap years) and a range
  // the canonical encoding can represent.
  return parse_utc(value).has_value();
}

bool validate_limits(const TopologyLimits& limits, std::string& explanation) {
  const auto fail = [&explanation](std::string message) {
    explanation = std::move(message);
    return false;
  };
  if (limits.max_nodes == 0 || limits.max_containment_edges == 0 || limits.max_adjacency_edges == 0 ||
      limits.max_domain_declarations == 0 || limits.max_domain_associations == 0) {
    return fail("every topology limit must be greater than zero");
  }
  if (limits.max_mutations_per_batch == 0) {
    return fail("max_mutations_per_batch must be greater than zero");
  }
  if (limits.max_label_bytes == 0 || limits.max_label_bytes > 4096) {
    return fail("max_label_bytes must be in 1..4096");
  }
  if (limits.max_source_bytes == 0 || limits.max_source_bytes > 256) {
    return fail("max_source_bytes must be in 1..256");
  }
  if (limits.max_reason_bytes == 0 || limits.max_reason_bytes > 8192) {
    return fail("max_reason_bytes must be in 1..8192");
  }
  if (limits.max_traversal_depth == 0 || limits.max_traversal_depth > 4096) {
    return fail("max_traversal_depth must be in 1..4096");
  }
  if (limits.max_traversal_nodes == 0) {
    return fail("max_traversal_nodes must be greater than zero");
  }
  if (limits.max_traversal_nodes < limits.max_nodes) {
    return fail("max_traversal_nodes must be at least max_nodes so no complete walk can be truncated");
  }
  if (limits.max_document_bytes < 4096) {
    return fail("max_document_bytes must be at least 4096");
  }
  if (limits.max_line_bytes < 128 || limits.max_line_bytes > limits.max_document_bytes) {
    return fail("max_line_bytes must be at least 128 and at most max_document_bytes");
  }
  if (limits.retained_generations == 0 || limits.retained_generations > 4096) {
    return fail("retained_generations must be in 1..4096");
  }
  if (limits.idempotency_window == 0 || limits.idempotency_window > 4096) {
    return fail("idempotency_window must be in 1..4096");
  }
  if (limits.max_containment_edges < limits.max_nodes) {
    return fail("max_containment_edges must be at least max_nodes (every node but a facility root has a parent)");
  }
  return true;
}

Result<void> validate_label(std::string_view label, std::size_t max_bytes, std::string_view field) {
  if (label.size() > max_bytes) {
    return Error(ErrorCode::TextTooLong,
                 describe_mismatch(field, "<oversized>", "exceeds the configured maximum length"))
        .with_subject(std::to_string(label.size()));
  }
  if (text::contains_control_characters(label)) {
    return Error(ErrorCode::MalformedRecord, describe_mismatch(field, "<control characters>", "must not contain control characters"));
  }
  if (!text::is_valid_utf8(label)) {
    return Error(ErrorCode::InvalidUtf8, describe_mismatch(field, "<invalid utf-8>", "must be valid UTF-8"));
  }
  if (!label.empty()) {
    const char front = label.front();
    const char back = label.back();
    if (front == ' ' || front == '\t' || back == ' ' || back == '\t') {
      return Error(ErrorCode::MalformedRecord,
                   describe_mismatch(field, label, "must not begin or end with whitespace"));
    }
  }
  return ok();
}

Result<void> validate_source(std::string_view source, const TopologyLimits& limits) {
  FT_TRYV(validate_label(source, limits.max_source_bytes, "source"));
  if (source.empty()) {
    return Error(ErrorCode::MissingField, "source must not be empty");
  }
  return ok();
}

Result<void> validate_reason(std::string_view reason, const TopologyLimits& limits) {
  return validate_label(reason, limits.max_reason_bytes, "reason");
}

Result<void> validate_provenance(const ProvenanceRecord& provenance, const TopologyLimits& limits) {
  if (provenance.actor.empty()) {
    return Error(ErrorCode::MissingField, "provenance actor must not be empty");
  }
  FT_TRYV(validate_source(provenance.source, limits));
  FT_TRYV(validate_reason(provenance.reason, limits));
  if (!provenance.recorded_at.empty() && !is_canonical_utc_timestamp(provenance.recorded_at)) {
    return Error(ErrorCode::MalformedRecord,
                 "provenance timestamp must be canonical UTC \"YYYY-MM-DDTHH:MM:SSZ\"")
        .with_subject(provenance.recorded_at);
  }
  return ok();
}

}  // namespace dccp::facility_topology
