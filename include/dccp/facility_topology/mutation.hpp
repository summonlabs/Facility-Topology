// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_MUTATION_HPP
#define DCCP_FACILITY_TOPOLOGY_MUTATION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dccp/facility_topology/digest.hpp"
#include "dccp/facility_topology/model.hpp"
#include "dccp/facility_topology/result.hpp"
#include "dccp/facility_topology/topology.hpp"

namespace dccp::facility_topology {

// ---------------------------------------------------------------------------
// Mutation commands
// ---------------------------------------------------------------------------

struct AddNode {
  NodeRecord node;
};

struct RemoveNode {
  NodeId id;
};

/// Reparents a node's Physical containment edge in a single atomic step. The
/// node keeps its identity, label, provenance, descendants and domain
/// references; only its container changes.
struct MoveNode {
  NodeId id;
  NodeId new_parent;
  ProvenanceRecord provenance;
};

struct AddContainment {
  ContainmentEdge edge;
};

struct RemoveContainment {
  NodeId parent;
  NodeId child;
};

struct SetNodeLabel {
  NodeId id;
  std::string label;
  ProvenanceRecord provenance;
};

struct AddAdjacency {
  AdjacencyEdge edge;
};

struct RemoveAdjacency {
  NodeId first;
  NodeId second;
  AdjacencyKind kind = AdjacencyKind::SharedBoundary;
};

struct DeclareDomain {
  DomainDeclaration declaration;
};

struct RetireDomain {
  DomainId id;
};

struct AddAssociation {
  DomainAssociation association;
};

struct RemoveAssociation {
  NodeId node;
  DomainId domain;
};

/// A single mutation command. The alternative determines its semantics; there
/// is no ambiguity between "absent" and "zero" because no field is optional
/// except where the model itself is optional.
using Mutation =
    std::variant<AddNode, RemoveNode, MoveNode, AddContainment, RemoveContainment, SetNodeLabel, AddAdjacency,
                 RemoveAdjacency, DeclareDomain, RetireDomain, AddAssociation, RemoveAssociation>;

/// Stable token naming the alternative of a mutation ("add-node", ...).
std::string_view mutation_kind_token(const Mutation& mutation) noexcept;

// ---------------------------------------------------------------------------
// Batch
// ---------------------------------------------------------------------------

/// A batch is the unit of generation transition and the unit of atomicity:
/// either every mutation in it is applied and a new generation is published, or
/// none is and the previous generation remains authoritative.
struct MutationBatch {
  /// Generation the caller believes is current. It must match the store's head
  /// (or the in-memory base) exactly, otherwise the batch is rejected as stale.
  TopologyGeneration base_generation;

  /// Durable authority the caller was granted when it opened the store for
  /// mutation. Zero means "no durable authority claimed" and is accepted only
  /// for in-memory application.
  WriterEpoch authority_epoch;

  /// Who is making the change. Must be a valid ActorId.
  ActorId actor;

  /// Short bounded producer token ("cli", "api", "import", ...).
  std::string source;

  /// Optional bounded human explanation, recorded in the provenance of every
  /// record the batch introduces.
  std::string reason;

  /// Caller-supplied idempotency key. Retrying a batch with the same identity
  /// and identical content is a no-op that reports the original outcome;
  /// reusing an identity for different content is rejected.
  MutationId mutation_id;

  /// Canonical UTC timestamp recorded in provenance. When empty, a durable
  /// store stamps the batch with its injected clock.
  std::string recorded_at;

  /// Ordered mutations. Order is significant and is applied left to right.
  std::vector<Mutation> mutations;
};

/// Validates the untrusted shape of a batch (bounds, identity syntax, enum
/// domains, timestamps) before any structural work happens.
Result<void> validate_batch(const MutationBatch& batch, const TopologyLimits& limits);

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------

/// What happened to exactly one mutation of a batch.
struct MutationDisposition {
  std::size_t index = 0;
  ErrorCode code = ErrorCode::Ok;
  std::string explanation;
  std::string subject;

  bool accepted() const noexcept { return code == ErrorCode::Ok; }
};

/// Machine-readable explanation of a batch outcome.
struct MutationOutcome {
  /// True when the batch produced a candidate generation.
  bool published = false;
  /// True when that candidate was durably published by a store. Always false
  /// for in-memory application.
  bool committed = false;
  /// True when the batch identity had already been applied with identical
  /// content, so nothing was applied again; `new_generation` reports the
  /// generation the original application produced.
  bool replayed = false;
  TopologyGeneration base_generation;
  TopologyGeneration new_generation;
  /// Overall disposition of the batch: Ok when it took effect, BatchRejected
  /// when any mutation or the whole-graph validation refused it.
  ErrorCode code = ErrorCode::Ok;
  /// Specific code of the first failure: the rejecting mutation's own code, or
  /// the whole-graph validation issue when every mutation applied. This is the
  /// code a caller sees when a rejection is reported as an error.
  ErrorCode first_failure = ErrorCode::Ok;
  std::string explanation;
  /// One entry per mutation, in batch order, for both accepted and rejected
  /// batches. A rejected batch keeps dispositions for every mutation it
  /// reached.
  std::vector<MutationDisposition> dispositions;
  /// Content digest of the resulting generation (zero when rejected).
  Digest content_digest{};

  /// True when the batch took effect, either now or in an earlier commit whose
  /// identity this one reused.
  bool accepted() const noexcept { return published || replayed; }

  /// Stable one-line summary: "code=<CODE> published=... base=<n> head=<n> applied=<k>/<m>".
  std::string to_string() const;

  /// Multi-line explanation, one line per mutation.
  std::string explain() const;
};

/// A batch application: the outcome plus the candidate snapshot when accepted.
struct BatchApplication {
  MutationOutcome outcome;
  TopologySnapshot candidate;

  bool accepted() const noexcept { return outcome.published; }
};

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

/// Applies a batch to `base` and returns the candidate generation.
///
/// The returned snapshot is stamped with `base.generation() + 1` and is *not*
/// published anywhere: publication is an explicit store operation. On
/// rejection the error is BatchRejected and its message names the first
/// rejected mutation, its index and its stable code.
Result<TopologySnapshot> apply_batch(const TopologySnapshot& base, const MutationBatch& batch,
                                     const TopologyLimits& limits = {});

/// Applies a batch and always returns a structured explanation of every
/// mutation's disposition. Never fails.
BatchApplication apply_batch_explained(const TopologySnapshot& base, const MutationBatch& batch,
                                       const TopologyLimits& limits = {});

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_MUTATION_HPP
