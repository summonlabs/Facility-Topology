// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_STORE_HPP
#define DCCP_FACILITY_TOPOLOGY_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "dccp/facility_topology/canonical.hpp"
#include "dccp/facility_topology/clock.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "dccp/facility_topology/result.hpp"
#include "dccp/facility_topology/topology.hpp"

namespace dccp::facility_topology {

/// How a store directory is opened.
enum class OpenMode : std::uint8_t {
  /// Read-only: no mutation authority is requested, the writer epoch is not
  /// advanced, and the store lock is only ever taken in shared mode.
  ReadOnly = 0,
  /// Read-write: a writer epoch is reserved and mutation is permitted.
  ReadWrite = 1,
};

std::string_view open_mode_token(OpenMode mode) noexcept;

/// Deterministic fault-injection points.
///
/// This is an explicit, opt-in test seam. With the default (`None`) the store
/// behaves exactly as it does in production: the seam exists so that crash and
/// partial-write behaviour can be exercised by a real process that exits at a
/// precise point in the commit protocol. A fault point terminates the calling
/// process immediately (`std::_Exit`) and never returns.
enum class FaultPoint : std::uint8_t {
  None = 0,
  /// After the candidate generation file has been written and flushed into
  /// the pending directory, before it is published.
  AfterPendingWrite = 1,
  /// After the candidate generation file has been atomically published into
  /// the generation directory, before the head marker is replaced.
  AfterGenerationCommit = 2,
  /// After the head marker has been replaced, before superseded generations
  /// are pruned.
  AfterHeadReplace = 3,
};

std::string_view fault_point_token(FaultPoint point) noexcept;

/// Store behaviour configuration.
struct StoreConfig {
  TopologyLimits limits{};

  /// Injection point for crash testing. Default: no injection.
  FaultPoint fault_point = FaultPoint::None;

  /// Time source used to stamp provenance. Borrowed, must outlive the store.
  /// When null, an internal system clock is used.
  const Clock* clock = nullptr;

  /// Create the store directory when it does not exist (read-write only).
  bool create_directory = true;
};

/// Externally visible state of a store directory.
struct StoreStatus {
  bool open = false;
  bool writable = false;
  /// True when a published head exists.
  bool initialized = false;
  /// Published head generation (0 when uninitialized).
  TopologyGeneration head;
  /// Highest generation file present, whether or not it is the head.
  TopologyGeneration max_committed;
  std::size_t generation_count = 0;
  std::size_t pending_count = 0;
  std::size_t quarantine_count = 0;
  /// Writer epoch held by this handle (0 for read-only handles).
  WriterEpoch epoch;
  /// True when the head marker is missing, stale or unreadable, or when
  /// unpublished or temporary files are present.
  bool needs_recovery = false;
  std::string explanation;
};

/// What recovery did.
struct RecoveryReport {
  /// A head marker was reconstructed or corrected.
  bool recovered = false;
  /// Anything at all changed on disk.
  bool repaired = false;
  TopologyGeneration head_before;
  TopologyGeneration head_after;
  std::size_t pending_files_discarded = 0;
  std::size_t corrupt_files_quarantined = 0;
  std::size_t generations_pruned = 0;
  /// Generations that remain readable, ascending.
  std::vector<TopologyGeneration> retained;
  std::string explanation;
};

/// Options for creating the first generation of a store.
struct CreateOptions {
  ActorId actor;
  std::string source;
  std::string reason;
  /// Canonical UTC timestamp; empty means "stamp from the store clock".
  std::string recorded_at;
  /// Batch identity recorded for replay detection; may be empty.
  MutationId mutation_id;
};

/// Options for one durable commit.
struct CommitOptions {
  /// Cooperative cancellation. The token is observed only *before* the commit
  /// point: a cancelled commit never publishes, and a commit that has already
  /// passed the commit point always completes and reports success.
  std::optional<std::stop_token> stop;
};

/// A durable, generation-bound topology store.
///
/// On-disk layout of a store directory:
///
/// ```
/// LOCK                          exclusive/shared advisory lock
/// EPOCH                         highest issued writer epoch
/// HEAD                          published head marker (generation + digest)
/// generations/gen-<20 digits>.ftop   committed generations, immutable
/// pending/                      temporary files, never authoritative
/// quarantine/                   files rejected by integrity checks
/// ```
///
/// Commit protocol:
/// plan -> validate -> reserve transaction identity -> write temporary
/// generation -> verify integrity -> atomically publish -> replace head ->
/// retire superseded temporary state. The atomic rename of the generation file
/// into `generations/` is the commit point; `HEAD` is a derived marker that
/// recovery can always rebuild from the committed generations.
///
/// The type is movable and not copyable. It is not thread-safe: use one handle
/// per thread. Cross-process safety is provided by the store lock plus writer
/// epochs, not by in-process synchronization.
class TopologyStore {
 public:
  TopologyStore() noexcept;
  ~TopologyStore();

  TopologyStore(TopologyStore&&) noexcept;
  TopologyStore& operator=(TopologyStore&&) noexcept;
  TopologyStore(const TopologyStore&) = delete;
  TopologyStore& operator=(const TopologyStore&) = delete;

  /// Opens a store directory.
  ///
  /// Read-write opens reserve a fresh writer epoch under the exclusive lock and
  /// then release the lock: the epoch, not a held lock, is the mutation
  /// authority. A read-only open takes no lock and never writes.
  static Result<TopologyStore> open(const std::filesystem::path& directory, OpenMode mode, StoreConfig config = {});

  /// Releases authority and closes the handle. Idempotent.
  Result<void> close();

  bool is_open() const noexcept;
  OpenMode mode() const noexcept;
  bool writable() const noexcept;
  WriterEpoch epoch() const noexcept;
  const std::filesystem::path& directory() const noexcept;
  const TopologyLimits& limits() const noexcept;

  /// Read-only status snapshot. Also reports whether recovery is required.
  Result<StoreStatus> status() const;

  /// Creates generation 1 from an already validated topology.
  ///
  /// The supplied snapshot must be structurally valid; it is re-validated
  /// before publication. Fails with STORE_NOT_EMPTY when the store already has
  /// a published head.
  Result<MutationOutcome> create(const TopologySnapshot& initial, const CreateOptions& options);

  /// Applies a batch and atomically publishes the resulting generation.
  ///
  /// Rejections that happen before the commit point leave the store byte-for-
  /// byte unchanged.
  Result<MutationOutcome> commit(const MutationBatch& batch, const CommitOptions& options = {});

  Result<TopologyGeneration> head_generation() const;
  Result<GenerationManifest> head_manifest() const;
  Result<TopologySnapshot> head() const;
  Result<GenerationDocument> head_document() const;

  Result<GenerationDocument> load_document(TopologyGeneration generation) const;
  Result<TopologySnapshot> load_generation(TopologyGeneration generation) const;
  Result<std::vector<TopologyGeneration>> generation_numbers() const;

  /// Repairs the store directory and reconstructs the head marker.
  ///
  /// Recovery is conservative: it discards temporary files that were never
  /// published, quarantines generation files that fail schema or integrity
  /// checks, and re-points the head at the highest *verified* committed
  /// generation at its original generation number. Recovery never invents a
  /// generation, never renumbers evidence and never promotes unpublished
  /// temporary state to authoritative state.
  Result<RecoveryReport> recover();

  /// Writes the head generation to `file` using an atomic replace.
  Result<void> export_head_to(const std::filesystem::path& file) const;

  /// Reads and fully validates a canonical document from disk.
  static Result<GenerationDocument> import_from(const std::filesystem::path& file, const TopologyLimits& limits = {});

 private:
  struct Impl;
  explicit TopologyStore(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_STORE_HPP
