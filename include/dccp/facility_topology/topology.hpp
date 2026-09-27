// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_TOPOLOGY_HPP
#define DCCP_FACILITY_TOPOLOGY_TOPOLOGY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/digest.hpp"
#include "dccp/facility_topology/model.hpp"
#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology {

/// Immutable structural storage. Consumers never see this type; they only see
/// validated snapshots and builders.
struct GraphData;

/// Internal access bridge: gives library-private encoders (canonical
/// serialization, diff, durable store) read access to a snapshot's storage
/// without widening the consumer-facing surface.
struct SnapshotAccess;

/// Order in which a structural traversal visits nodes.
///
/// Both orders are fully deterministic because siblings are always visited in
/// canonical NodeId byte order.
enum class TraversalOrder : std::uint8_t {
  BreadthFirst = 0,
  DepthFirstPreOrder = 1,
};

std::string_view traversal_order_token(TraversalOrder order) noexcept;
Result<TraversalOrder> traversal_order_parse(std::string_view token);

/// Explicit bounds for one traversal. Both are clamped to the topology
/// limits, so a caller can never request an unbounded walk.
struct TraversalLimits {
  std::size_t max_depth = 64;
  std::size_t max_nodes = 1'000'000;
};

/// Aggregate counts of one topology generation.
struct TopologyStats {
  std::size_t nodes = 0;
  std::size_t facilities = 0;
  std::size_t physical_containment = 0;
  std::size_t logical_containment = 0;
  std::size_t adjacency = 0;
  std::size_t domains = 0;
  std::size_t associations = 0;
  std::uint32_t max_depth = 0;

  friend bool operator==(const TopologyStats&, const TopologyStats&) noexcept = default;
  std::string to_string() const;
};

/// Severity-free validation finding: a stable code plus an explanation.
struct ValidationIssue {
  ErrorCode code = ErrorCode::Ok;
  std::string subject;
  std::string explanation;

  friend bool operator==(const ValidationIssue&, const ValidationIssue&) noexcept = default;
};

/// Result of validating a candidate topology.
///
/// Issues are reported deterministically: checks always run in the same fixed
/// order and each check visits records in canonical order, so the same
/// candidate always produces the same issue list in the same sequence.
struct ValidationReport {
  bool valid = true;
  std::vector<ValidationIssue> issues;

  std::string to_string() const;
  std::size_t count(ErrorCode code) const;
};

// ---------------------------------------------------------------------------
// Immutable snapshot
// ---------------------------------------------------------------------------

/// An immutable, generation-bound view of one facility topology.
///
/// A snapshot is a value type with shared immutable storage: copying it is
/// cheap and every copy observes exactly the same structure. Concurrent reads
/// from any number of threads are safe; no snapshot is ever mutated after it
/// is produced, so a published generation cannot change underneath a reader.
class TopologySnapshot {
 public:
  /// Empty snapshot: no generation, no nodes.
  TopologySnapshot() noexcept;
  TopologySnapshot(const TopologySnapshot&) noexcept;
  TopologySnapshot(TopologySnapshot&&) noexcept;
  TopologySnapshot& operator=(const TopologySnapshot&) noexcept;
  TopologySnapshot& operator=(TopologySnapshot&&) noexcept;
  ~TopologySnapshot();

  /// The generation this snapshot was produced for. Generation 0 means the
  /// snapshot is a candidate that has never been published.
  const TopologyGeneration& generation() const noexcept;

  /// True when storage is attached (even for an empty topology).
  bool valid() const noexcept;

  /// Limits this generation was validated under.
  const TopologyLimits& limits() const noexcept;

  TopologyStats stats() const;

  /// Integrity digest of the structural content only: nodes (id, kind, label,
  /// zone scope), containment, adjacency, domain declarations and domain
  /// associations. Independent of generation, lineage, provenance and
  /// timestamps, so two generations that describe the same facility have the
  /// same content digest.
  Digest content_digest() const;

  /// True when both snapshots describe the same structure.
  bool same_structure_as(const TopologySnapshot& other) const;

  // -- node queries ---------------------------------------------------------

  const NodeRecord* find_node(const NodeId& id) const;
  bool contains_node(const NodeId& id) const;
  std::optional<NodeKind> node_kind(const NodeId& id) const;

  /// Physical (structural) parent: the container this node physically sits in.
  std::optional<NodeId> physical_parent(const NodeId& id) const;

  /// Zone that logically groups this node, when it is a zone member.
  std::optional<NodeId> zone_of(const NodeId& id) const;

  /// Physical children in canonical NodeId order.
  std::vector<NodeId> children(const NodeId& id) const;

  /// Nodes logically grouped by a zone, in canonical NodeId order.
  std::vector<NodeId> zone_members(const NodeId& zone) const;

  /// Ancestors from the facility root down to (excluding) the node.
  std::vector<NodeId> ancestry(const NodeId& id) const;

  /// Physical descendants in deterministic breadth-first, canonical order
  /// (the node itself is not included).
  std::vector<NodeId> descendants(const NodeId& id) const;

  /// Nodes physically adjacent to `id`, in canonical order.
  std::vector<NodeId> neighbors(const NodeId& id) const;

  /// Adjacency edges incident to `id`, in canonical order.
  std::vector<AdjacencyEdge> adjacency_of(const NodeId& id) const;

  /// Facility roots, in canonical order.
  std::vector<NodeId> facilities() const;

  /// All nodes of a kind, in canonical order.
  std::vector<NodeId> nodes_of_kind(NodeKind kind) const;

  /// Facility root that contains `id` (the node itself when it is a facility).
  std::optional<NodeId> facility_of(const NodeId& id) const;

  /// Depth below the facility root (root = 0); nullopt when uncontained.
  std::optional<std::uint32_t> depth_of(const NodeId& id) const;

  // -- domain queries -------------------------------------------------------

  const DomainDeclaration* find_domain(const DomainId& id) const;
  std::vector<DomainDeclaration> domains() const;
  /// Nodes referencing a domain, in canonical order.
  std::vector<NodeId> nodes_in_domain(const DomainId& domain) const;
  /// Domain references held by a node, in canonical order.
  std::vector<DomainAssociation> associations_of(const NodeId& node) const;

  // -- whole-graph reads ----------------------------------------------------

  /// Node records in canonical NodeId byte order (O(n) copy).
  std::vector<NodeRecord> nodes() const;
  std::vector<NodeId> node_ids() const;
  /// Containment edges in canonical (parent, child, boundary) order.
  std::vector<ContainmentEdge> containment_edges() const;
  /// Adjacency edges in canonical (first, second, kind) order.
  std::vector<AdjacencyEdge> adjacency_edges() const;
  /// Domain associations in canonical (node, domain, kind) order.
  std::vector<DomainAssociation> associations() const;

  // -- deterministic traversals --------------------------------------------

  /// Deterministic structural traversal of the Physical containment tree
  /// rooted at `root`. The root itself is not part of the result. Siblings are
  /// always visited in canonical NodeId byte order.
  ///
  /// The walk is complete or it fails: if the tree below `root` is deeper than
  /// `limits.max_depth`, or would visit more than `limits.max_nodes` nodes, the
  /// call fails with TRAVERSAL_DEPTH_EXCEEDED or LIMIT_EXCEEDED rather than
  /// returning a silently truncated result. Both bounds are clamped to the
  /// generation's own limits.
  Result<std::vector<NodeId>> traverse(const NodeId& root, TraversalOrder order,
                                       TraversalLimits limits) const;

  /// Deterministic reachability test between two nodes of the same facility.
  Result<bool> is_descendant_of(const NodeId& node, const NodeId& ancestor) const;

  /// Deepest node common to both nodes' ancestry (nullopt when they are in
  /// different facilities).
  Result<std::optional<NodeId>> lowest_common_ancestor(const NodeId& lhs, const NodeId& rhs) const;

 private:
  friend class TopologyBuilder;
  friend struct SnapshotAccess;
  friend Result<TopologySnapshot> snapshot_from_data(std::shared_ptr<const GraphData> data);

  explicit TopologySnapshot(std::shared_ptr<const GraphData> data) noexcept;

  std::shared_ptr<const GraphData> data_;
};

/// Attaches already-validated storage to a snapshot. Internal boundary used by
/// the builder and the store; it performs no validation itself.
Result<TopologySnapshot> snapshot_from_data(std::shared_ptr<const GraphData> data);

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

/// Mutable construction surface for a candidate topology generation.
///
/// A builder is the only way structure is created or changed. It accepts
/// untrusted records one at a time, enforces every structural precondition
/// immediately (returning a stable rejection code and a human explanation),
/// and produces an immutable snapshot only after whole-graph validation.
///
/// A builder is neither copyable nor thread-safe: one builder belongs to one
/// sequence of mutations on one thread.
class TopologyBuilder {
 public:
  TopologyBuilder();
  explicit TopologyBuilder(TopologyLimits limits);
  ~TopologyBuilder();

  TopologyBuilder(TopologyBuilder&&) noexcept;
  TopologyBuilder& operator=(TopologyBuilder&&) noexcept;
  TopologyBuilder(const TopologyBuilder&) = delete;
  TopologyBuilder& operator=(const TopologyBuilder&) = delete;

  /// Seeds a builder with the complete contents of an existing snapshot.
  /// Generation and provenance are carried over unchanged.
  static Result<TopologyBuilder> from_snapshot(const TopologySnapshot& snapshot);

  const TopologyLimits& limits() const noexcept;
  void set_limits(TopologyLimits limits) noexcept;

  /// Current generation of the data being built (0 while constructing a fresh
  /// topology). `build()` stamps the candidate generation.
  const TopologyGeneration& generation() const noexcept;

  // -- mutations ------------------------------------------------------------

  Result<void> add_node(NodeRecord node);
  /// Removes a leaf node with no incident edges or domain references.
  Result<void> remove_node(const NodeId& id);
  Result<void> set_node_label(const NodeId& id, std::string label, ProvenanceRecord provenance);

  Result<void> add_containment(ContainmentEdge edge);
  Result<void> remove_containment(const NodeId& parent, const NodeId& child);
  /// Reparents a node's Physical containment edge in one step.
  Result<void> move_node(const NodeId& child, const NodeId& new_parent, ProvenanceRecord provenance);

  Result<void> add_adjacency(AdjacencyEdge edge);
  Result<void> remove_adjacency(const NodeId& first, const NodeId& second, AdjacencyKind kind);

  Result<void> declare_domain(DomainDeclaration declaration);
  Result<void> retire_domain(const DomainId& id);
  Result<void> add_association(DomainAssociation association);
  Result<void> remove_association(const NodeId& node, const DomainId& domain);

  // -- queries (identical semantics to TopologySnapshot) --------------------

  const NodeRecord* find_node(const NodeId& id) const;
  bool contains_node(const NodeId& id) const;
  std::optional<NodeKind> node_kind(const NodeId& id) const;
  std::optional<NodeId> physical_parent(const NodeId& id) const;
  std::optional<NodeId> zone_of(const NodeId& id) const;
  std::vector<NodeId> children(const NodeId& id) const;
  std::vector<NodeId> zone_members(const NodeId& zone) const;
  std::vector<NodeId> ancestry(const NodeId& id) const;
  std::vector<NodeId> descendants(const NodeId& id) const;
  std::vector<NodeId> neighbors(const NodeId& id) const;
  std::vector<NodeId> facilities() const;
  std::vector<NodeId> nodes_of_kind(NodeKind kind) const;
  std::optional<NodeId> facility_of(const NodeId& id) const;
  std::optional<std::uint32_t> depth_of(const NodeId& id) const;
  const DomainDeclaration* find_domain(const DomainId& id) const;
  std::vector<NodeId> nodes_in_domain(const DomainId& domain) const;
  std::vector<DomainAssociation> associations_of(const NodeId& node) const;
  std::size_t node_count() const;
  std::size_t containment_count() const;
  std::size_t adjacency_count() const;
  std::size_t domain_count() const;
  std::size_t association_count() const;
  std::vector<NodeId> node_ids() const;
  std::vector<ContainmentEdge> containment_edges() const;
  std::vector<AdjacencyEdge> adjacency_edges() const;
  std::vector<DomainAssociation> associations() const;
  std::vector<DomainDeclaration> domains() const;
  std::vector<NodeRecord> nodes() const;
  Result<std::vector<NodeId>> traverse(const NodeId& root, TraversalOrder order,
                                       TraversalLimits limits) const;
  Result<bool> is_descendant_of(const NodeId& node, const NodeId& ancestor) const;

  // -- validation and publication ------------------------------------------

  /// Full whole-graph validation of the current candidate state.
  ValidationReport validate() const;

  TopologyStats stats() const;

  /// Validates and freezes the candidate into an immutable snapshot stamped
  /// with `generation`. Returns the first validation issue as an error when
  /// the candidate is not publishable.
  Result<TopologySnapshot> build(const TopologyGeneration& generation) const;

 private:
  friend Result<TopologySnapshot> snapshot_from_data(std::shared_ptr<const GraphData> data);

  std::unique_ptr<GraphData> data_;
};

/// Convenience: build a snapshot from a record set in one call.
Result<TopologySnapshot> assemble_topology(std::vector<NodeRecord> nodes, std::vector<ContainmentEdge> containment,
                                           std::vector<AdjacencyEdge> adjacency,
                                           std::vector<DomainDeclaration> domains,
                                           std::vector<DomainAssociation> associations,
                                           const TopologyGeneration& generation, TopologyLimits limits = {});

/// Validates already-assembled storage (used by import and recovery paths).
ValidationReport validate_graph(const GraphData& data, const TopologyLimits& limits);

// ---------------------------------------------------------------------------
// In-process publication
// ---------------------------------------------------------------------------

/// The single place where many threads read one topology generation.
///
/// The concurrency model is deliberately trivial to audit:
///   * readers call current() and receive a shared handle to an immutable
///     snapshot; they never hold a lock and never observe a partially updated
///     value, because a snapshot is never mutated after it is built;
///   * one publisher at a time may publish(); a second concurrent publisher is
///     refused immediately rather than blocked, so there is no lock ordering,
///     no lock held across a callback and no re-entrancy hazard;
///   * no callback is ever invoked while internal state is being changed.
class PublishedTopology {
 public:
  PublishedTopology() noexcept;
  ~PublishedTopology();

  PublishedTopology(const PublishedTopology&) = delete;
  PublishedTopology& operator=(const PublishedTopology&) = delete;

  /// Immutable snapshot currently published; null before the first successful
  /// publish.
  std::shared_ptr<const TopologySnapshot> current() const noexcept;

  /// Publishes `snapshot` as the current generation.
  ///
  /// Returns false when another thread is publishing at that moment. The
  /// caller that lost the race still holds a valid snapshot and may retry or
  /// abandon the change; nothing is half-published either way.
  bool publish(const TopologySnapshot& snapshot) noexcept;

  /// Number of successful publications.
  std::uint64_t publish_count() const noexcept;

 private:
  std::atomic<std::shared_ptr<const TopologySnapshot>> current_;
  std::atomic<bool> publishing_;
  std::atomic<std::uint64_t> publish_count_;
};

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_TOPOLOGY_HPP
