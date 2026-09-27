// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_CANONICAL_HPP
#define DCCP_FACILITY_TOPOLOGY_CANONICAL_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/digest.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "dccp/facility_topology/result.hpp"
#include "dccp/facility_topology/topology.hpp"
#include "dccp/facility_topology/version.hpp"

namespace dccp::facility_topology {

/// One remembered batch identity, used for idempotent replay detection.
struct AppliedBatch {
  MutationId mutation_id;
  /// Digest of the canonical form of the batch content.
  Digest batch_digest{};
  /// Generation that applied the batch.
  TopologyGeneration generation;

  friend bool operator==(const AppliedBatch&, const AppliedBatch&) noexcept = default;
};

/// Lineage, authority and provenance of one durable generation.
///
/// The manifest is content that changes with every publication, so it is part
/// of the document digest but *not* of the content digest: two generations
/// with identical structure share a content digest and differ in document
/// digest.
struct GenerationManifest {
  TopologyGeneration generation;
  /// Generation this one was derived from; 0 for the first generation.
  TopologyGeneration parent_generation;
  /// Document digest of the parent generation (zero for the first).
  Digest parent_digest{};
  /// Writer authority that published this generation.
  WriterEpoch authority_epoch;
  /// Actor that published this generation.
  ActorId actor;
  std::string source;
  std::string reason;
  /// Canonical UTC timestamp of publication; empty when not recorded.
  std::string recorded_at;
  /// Batch identity that produced this generation; empty for an initial
  /// import.
  MutationId mutation_id;
  /// Oldest generation still retained on disk (the retention floor).
  std::size_t retention_floor = 0;
  /// Recent batch identities, newest first, bounded by the idempotency window.
  std::vector<AppliedBatch> applied;

  friend bool operator==(const GenerationManifest&, const GenerationManifest&) noexcept = default;
};

/// A parsed canonical document: manifest plus validated structure.
struct GenerationDocument {
  GenerationManifest manifest;
  TopologySnapshot snapshot;
};

/// Serializes a generation to its canonical document form.
///
/// The output is byte-deterministic: the same structure and manifest always
/// produce identical bytes, records appear in canonical order, and the
/// trailing `digest sha256:<hex>` line covers every preceding byte after the
/// banner. The caller's snapshot generation must equal the manifest
/// generation.
Result<std::string> serialize_generation(const TopologySnapshot& snapshot, const GenerationManifest& manifest);

/// Parses and fully validates a canonical document.
///
/// Untrusted input is rejected rather than repaired: an unsupported banner,
/// unknown keyword, wrong field count, bad enum token, malformed identity,
/// non-canonical record order, count mismatch, duplicate record, oversized
/// line, oversized document, digest mismatch, or structurally invalid graph
/// all fail with a stable code and an explanation.
Result<GenerationDocument> parse_generation(std::string_view text, const TopologyLimits& limits = {});

/// Digest of a whole canonical document (as produced by serialize_generation).
Digest document_digest(std::string_view canonical_text);

/// Canonical serialization of a mutation batch. Deterministic; the output is a
/// faithful rendering of every field, including the writer epoch the caller
/// claimed.
std::string serialize_batch(const MutationBatch& batch);

/// Digest of the canonical form of a batch's *content*: base generation, actor,
/// source, reason, timestamp, mutation identity and the mutations themselves.
///
/// The writer epoch is deliberately excluded. It is transport authority rather
/// than request content, and it necessarily changes when a caller reopens a
/// store; including it would make a legitimate retry of an already-applied
/// batch look like a different request and be rejected as a conflict instead of
/// being recognised as a replay.
Digest batch_digest(const MutationBatch& batch);

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_CANONICAL_HPP
