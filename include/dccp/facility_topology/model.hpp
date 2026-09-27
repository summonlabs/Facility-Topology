// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_MODEL_HPP
#define DCCP_FACILITY_TOPOLOGY_MODEL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dccp/facility_topology/strong_id.hpp"
#include "dccp/facility_topology/text.hpp"

namespace dccp::facility_topology {

// ---------------------------------------------------------------------------
// Structural vocabulary
// ---------------------------------------------------------------------------

/// Kind of a structural node in the physical facility graph.
///
/// Values are part of the durable schema: they are appended to, never
/// renumbered. The canonical token is what canonical documents contain.
enum class NodeKind : std::uint8_t {
  Facility = 0,  ///< physical root: the site itself
  Building = 1,  ///< a building on the site
  Hall = 2,      ///< a data hall inside a building
  Room = 3,      ///< a room inside a building or hall
  Row = 4,       ///< a row of racks inside a room
  Rack = 5,      ///< a rack; always a structural leaf
  Zone = 6,      ///< a declared boundary region grouping existing nodes
};

/// Canonical lower-case token of a node kind (e.g. "rack").
std::string_view node_kind_token(NodeKind kind) noexcept;

/// Strictly parses a node kind token. Case-sensitive.
Result<NodeKind> node_kind_parse(std::string_view token);

/// Number of distinct node kinds.
inline constexpr std::size_t kNodeKindCount = 7;

/// Index of a node kind, usable as an array subscript.
constexpr std::size_t node_kind_index(NodeKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

/// Kind of boundary a containment relationship crosses.
///
/// A Physical boundary is a subdivision of physical space and forms the tree
/// that every structural node except a facility root hangs from. A Logical
/// boundary is a declared grouping: it may only originate at a Zone node and
/// expresses membership rather than physical subdivision.
enum class BoundaryKind : std::uint8_t {
  Physical = 0,
  Logical = 1,
};

std::string_view boundary_kind_token(BoundaryKind kind) noexcept;
Result<BoundaryKind> boundary_kind_parse(std::string_view token);

/// Reason two nodes are recorded as physically adjacent.
enum class AdjacencyKind : std::uint8_t {
  SharedBoundary = 0,      ///< they share a physical boundary surface
  ServiceAisle = 1,        ///< they face the same service aisle
  StructuralNeighbor = 2,  ///< they are consecutive siblings in one container
};

std::string_view adjacency_kind_token(AdjacencyKind kind) noexcept;
Result<AdjacencyKind> adjacency_kind_parse(std::string_view token);

/// Kind of facility domain a node is associated with.
///
/// Facility Topology only *references* domains. It never models capacity,
/// actuation, feed topology, cooling control or failure propagation: those
/// belong to the adjacent power and cooling control planes.
enum class DomainKind : std::uint8_t {
  Power = 0,
  Cooling = 1,
};

std::string_view domain_kind_token(DomainKind kind) noexcept;
Result<DomainKind> domain_kind_parse(std::string_view token);

// ---------------------------------------------------------------------------
// Schema tables (the single authoritative statement of containment rules)
// ---------------------------------------------------------------------------

/// Bit mask of node kinds; used by the structural schema tables.
using NodeKindMask = std::uint16_t;

constexpr NodeKindMask node_kind_bit(NodeKind kind) noexcept {
  return static_cast<NodeKindMask>(1U << static_cast<unsigned>(kind));
}

/// Declarative structural schema of one node kind.
struct NodeKindSchema {
  NodeKind kind;
  /// Canonical depth below a facility root (Facility = 0, Rack = 5).
  std::uint8_t depth;
  /// True when a node of this kind may have no Physical parent.
  bool root_eligible;
  /// Node kinds that may be this kind's Physical parent.
  NodeKindMask allowed_parent_kinds;
  /// Node kinds that may have this kind as their Physical parent.
  NodeKindMask allowed_child_kinds;
  /// True when a node of this kind may be a member of a Zone.
  bool zonable;
};

/// Schema of a node kind.
const NodeKindSchema& node_kind_schema(NodeKind kind) noexcept;

/// True when "parent" may be the Physical parent of "child".
bool schema_allows_physical_child(NodeKind parent, NodeKind child) noexcept;

/// Maximum canonical structural depth (Rack).
inline constexpr std::size_t kMaxStructuralDepth = 5;

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

/// Who caused authoritative state to change, and why.
///
/// A record is attached to every node, containment edge, adjacency edge,
/// domain declaration and domain association. It records the actor of the
/// mutation that introduced the current value; it is carried forward unchanged
/// when unrelated parts of the topology change.
struct ProvenanceRecord {
  ActorId actor;
  /// Short bounded token naming the producer ("cli", "import", "api", ...).
  std::string source;
  /// Optional short bounded human reason supplied by the caller.
  std::string reason;
  /// Optional canonical UTC timestamp (RFC 3339, "YYYY-MM-DDTHH:MM:SSZ").
  std::string recorded_at;

  friend bool operator==(const ProvenanceRecord&, const ProvenanceRecord&) noexcept = default;
};

/// True when a string is a canonical UTC RFC 3339 timestamp with second
/// precision ("YYYY-MM-DDTHH:MM:SSZ") within the representable range.
bool is_canonical_utc_timestamp(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

/// A structural node of the facility graph.
struct NodeRecord {
  NodeId id;
  NodeKind kind = NodeKind::Facility;
  /// Human-readable label. Empty is allowed (the identity carries meaning).
  std::string label;
  /// Required for Zone nodes and forbidden otherwise: the single node kind
  /// that this zone may group.
  std::optional<NodeKind> zone_member_kind;
  ProvenanceRecord provenance;

  friend bool operator==(const NodeRecord&, const NodeRecord&) noexcept = default;
};

/// A containment relationship: `parent` contains `child`.
///
/// The Physical boundary of every node except a facility root is unique. A
/// Logical boundary may only originate at a Zone node and is unique per child.
struct ContainmentEdge {
  NodeId parent;
  NodeId child;
  BoundaryKind boundary = BoundaryKind::Physical;
  ProvenanceRecord provenance;

  friend bool operator==(const ContainmentEdge&, const ContainmentEdge&) noexcept = default;
};

/// A symmetric physical adjacency between two distinct nodes.
///
/// Stored canonically with `first < second` so that a single edge represents
/// both directions and duplicates are impossible.
struct AdjacencyEdge {
  NodeId first;
  NodeId second;
  AdjacencyKind kind = AdjacencyKind::SharedBoundary;
  ProvenanceRecord provenance;

  friend bool operator==(const AdjacencyEdge&, const AdjacencyEdge&) noexcept = default;
};

/// Declaration that this generation references an external facility domain.
///
/// The declaration exists purely so that associations have referential
/// integrity inside one authoritative generation. It carries no capacity,
/// feed, actuation or control semantics.
struct DomainDeclaration {
  DomainId id;
  DomainKind kind = DomainKind::Power;
  std::string label;
  ProvenanceRecord provenance;

  friend bool operator==(const DomainDeclaration&, const DomainDeclaration&) noexcept = default;
};

/// Reference from a structural node to a declared domain.
struct DomainAssociation {
  NodeId node;
  DomainId domain;
  DomainKind kind = DomainKind::Power;
  ProvenanceRecord provenance;

  friend bool operator==(const DomainAssociation&, const DomainAssociation&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

/// Externally influenced bounds applied before allocation and mutation.
///
/// Every bound is checked against untrusted input before any container grows.
struct TopologyLimits {
  std::size_t max_nodes = 4'000'000;
  std::size_t max_containment_edges = 8'000'000;
  std::size_t max_adjacency_edges = 16'000'000;
  std::size_t max_domain_declarations = 1'000'000;
  std::size_t max_domain_associations = 16'000'000;
  std::size_t max_mutations_per_batch = 1'000'000;
  std::size_t max_label_bytes = 256;
  std::size_t max_source_bytes = 64;
  std::size_t max_reason_bytes = 512;
  /// Hard ceiling on any traversal depth, applied in addition to the caller's
  /// requested bound.
  std::size_t max_traversal_depth = 64;
  std::size_t max_traversal_nodes = 8'000'000;
  /// Largest canonical document accepted from disk, in bytes.
  std::size_t max_document_bytes = 1U << 30;  // 1 GiB
  /// Largest single canonical line accepted from disk, in bytes.
  std::size_t max_line_bytes = 1U << 16;  // 64 KiB
  /// Number of published generations kept on disk (including the head).
  std::size_t retained_generations = 8;
  /// Number of recent mutation identities remembered for idempotent replay.
  std::size_t idempotency_window = 64;

  friend bool operator==(const TopologyLimits&, const TopologyLimits&) noexcept = default;
};

/// True when a limits value is internally consistent and usable.
bool validate_limits(const TopologyLimits& limits, std::string& explanation);

// ---------------------------------------------------------------------------
// Shared value validation
// ---------------------------------------------------------------------------

/// Validates a caller-supplied label against the configured bound.
Result<void> validate_label(std::string_view label, std::size_t max_bytes, std::string_view field);

/// Validates a caller-supplied source token.
Result<void> validate_source(std::string_view source, const TopologyLimits& limits);

/// Validates a caller-supplied reason string.
Result<void> validate_reason(std::string_view reason, const TopologyLimits& limits);

/// Validates a provenance record against the configured limits.
Result<void> validate_provenance(const ProvenanceRecord& provenance, const TopologyLimits& limits);

/// True when two nodes may be physically adjacent (both attached to the same
/// facility and not the facility itself).
bool schema_allows_adjacency(NodeKind lhs, NodeKind rhs) noexcept;

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_MODEL_HPP
