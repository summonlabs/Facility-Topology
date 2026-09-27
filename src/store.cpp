// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "canonical_internal.hpp"
#include "file_ops.hpp"
#include "graph_data.hpp"

namespace dccp::facility_topology {
namespace {

constexpr std::string_view kLockName = "LOCK";
constexpr std::string_view kEpochName = "EPOCH";
constexpr std::string_view kHeadName = "HEAD";
constexpr std::string_view kGenerationsDir = "generations";
constexpr std::string_view kPendingDir = "pending";
constexpr std::string_view kQuarantineDir = "quarantine";
constexpr std::string_view kGenerationPrefix = "gen-";
constexpr std::string_view kGenerationSuffix = ".ftop";
constexpr std::string_view kHeadBanner = "ftop-head/1";
constexpr std::string_view kEpochBanner = "ftop-epoch/1";

/// Digits in the zero-padded generation number of a generation file name.
constexpr std::size_t kGenerationDigits = 20;

/// Exit status used by the deterministic fault-injection seam. A process that
/// stops here terminates exactly as a crash would.
constexpr int kFaultExitStatus = 9;

/// Marker file content describing the published head.
struct HeadMarker {
  bool present = false;
  TopologyGeneration generation;
  Digest document_digest{};
};

/// One generation file discovered on disk, with its verification outcome.
struct ScannedGeneration {
  TopologyGeneration generation;
  bool verified = false;
  Digest document_digest{};
  std::string failure;
};

/// A generation file together with its canonical text, verified digest and
/// parsed, structurally validated document. Produced by a single read and a
/// single parse so no caller re-reads or re-parses authoritative state.
struct VerifiedDocument {
  std::string text;
  Digest digest{};
  GenerationDocument document;
};

std::string generation_file_name(std::uint64_t generation) {
  const std::string digits = text::format_u64(generation);
  std::string out(kGenerationPrefix);
  if (digits.size() < kGenerationDigits) {
    out.append(kGenerationDigits - digits.size(), '0');
  }
  out.append(digits);
  out.append(kGenerationSuffix);
  return out;
}

Result<std::uint64_t> parse_generation_file_name(std::string_view name) {
  const std::size_t expected = kGenerationPrefix.size() + kGenerationDigits + kGenerationSuffix.size();
  if (name.size() != expected || name.substr(0, kGenerationPrefix.size()) != kGenerationPrefix ||
      name.substr(name.size() - kGenerationSuffix.size()) != kGenerationSuffix) {
    return Error(ErrorCode::MalformedRecord, "not a generation file name").with_subject(std::string(name));
  }
  // Generation file names are zero padded to a fixed width, so the canonical
  // decimal rule ("no leading zeros") does not apply here; the digits are
  // accumulated with an explicit overflow check instead.
  const std::string_view digits = name.substr(kGenerationPrefix.size(), kGenerationDigits);
  std::uint64_t value = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9') {
      return Error(ErrorCode::MalformedRecord, "generation file name does not contain a decimal number")
          .with_subject(std::string(name));
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10U) {
      return Error(ErrorCode::MalformedRecord, "generation number in the file name overflows")
          .with_subject(std::string(name));
    }
    value = value * 10U + digit;
  }
  if (value == 0) {
    return Error(ErrorCode::MalformedRecord, "published generation numbers start at 1")
        .with_subject(std::string(name));
  }
  return value;
}

void inject_fault(FaultPoint configured, FaultPoint at) {
  if (configured == at && configured != FaultPoint::None) {
    std::_Exit(kFaultExitStatus);
  }
}

}  // namespace

std::string_view open_mode_token(OpenMode mode) noexcept {
  return (mode == OpenMode::ReadWrite) ? "read-write" : "read-only";
}

std::string_view fault_point_token(FaultPoint point) noexcept {
  switch (point) {
    case FaultPoint::None:
      return "none";
    case FaultPoint::AfterPendingWrite:
      return "after-pending-write";
    case FaultPoint::AfterGenerationCommit:
      return "after-generation-commit";
    case FaultPoint::AfterHeadReplace:
      return "after-head-replace";
  }
  return "none";
}

// ---------------------------------------------------------------------------
// Store implementation
// ---------------------------------------------------------------------------

struct TopologyStore::Impl {
  /// One parsed generation kept for reuse.
  ///
  /// The cache is keyed by the digest of the bytes actually read, so a stale
  /// entry can only ever be used when the file on disk hashes to exactly the
  /// content that was parsed: integrity verification still happens on every
  /// read, and only the repeated parse is avoided.
  struct CachedGeneration {
    TopologyGeneration generation;
    Digest digest{};
    GenerationDocument document;
  };

  std::filesystem::path directory;
  OpenMode mode = OpenMode::ReadOnly;
  StoreConfig config;
  WriterEpoch epoch;
  bool open = false;
  SystemClock system_clock;
  mutable std::optional<CachedGeneration> cache_;

  const Clock& clock() const noexcept { return (config.clock != nullptr) ? *config.clock : system_clock; }

  std::filesystem::path lock_path() const { return directory / std::string(kLockName); }
  std::filesystem::path epoch_path() const { return directory / std::string(kEpochName); }
  std::filesystem::path head_path() const { return directory / std::string(kHeadName); }
  std::filesystem::path generations_path() const { return directory / std::string(kGenerationsDir); }
  std::filesystem::path pending_path() const { return directory / std::string(kPendingDir); }
  std::filesystem::path quarantine_path() const { return directory / std::string(kQuarantineDir); }
  std::filesystem::path generation_path(TopologyGeneration generation) const {
    return generations_path() / generation_file_name(generation.value());
  }

  Result<std::string> timestamp_now() const { return format_utc(clock().unix_seconds()); }

  // -- marker files ---------------------------------------------------------

  Result<HeadMarker> read_head() const {
    HeadMarker marker;
    if (!file_ops::is_regular_file(head_path())) {
      return marker;
    }
    FT_TRY(content, file_ops::read_file(head_path(), 4096));
    std::vector<std::string_view> lines;
    std::size_t position = 0;
    while (position < content.size()) {
      const std::size_t newline = content.find('\n', position);
      if (newline == std::string::npos) {
        return Error(ErrorCode::HeadCorrupt, "head marker has an unterminated line").with_subject(head_path().string());
      }
      lines.push_back(std::string_view(content).substr(position, newline - position));
      position = newline + 1;
    }
    if (lines.size() != 3 || lines[0] != kHeadBanner) {
      return Error(ErrorCode::HeadCorrupt, "head marker does not match the expected schema")
          .with_subject(head_path().string());
    }
    if (lines[1].size() <= 11 || lines[1].substr(0, 11) != "generation " || lines[2].size() <= 7 ||
        lines[2].substr(0, 7) != "digest ") {
      return Error(ErrorCode::HeadCorrupt, "head marker fields are missing or out of order")
          .with_subject(head_path().string());
    }
    const std::optional<std::uint64_t> generation = text::parse_u64(lines[1].substr(11));
    if (!generation.has_value() || *generation == 0) {
      return Error(ErrorCode::HeadCorrupt, "head marker records an invalid generation")
          .with_subject(head_path().string());
    }
    FT_TRY(digest, digest_parse_tagged(lines[2].substr(7)));
    marker.present = true;
    marker.generation = TopologyGeneration(*generation);
    marker.document_digest = digest;
    return marker;
  }

  Result<void> write_head(TopologyGeneration generation, const Digest& digest) const {
    std::string content(kHeadBanner);
    content.push_back('\n');
    content.append("generation ");
    content.append(text::format_u64(generation.value()));
    content.push_back('\n');
    content.append("digest ");
    content.append(digest_tagged_hex(digest));
    content.push_back('\n');
    return file_ops::atomic_write_file(head_path(), content);
  }

  Result<WriterEpoch> read_epoch() const {
    if (!file_ops::is_regular_file(epoch_path())) {
      return WriterEpoch(0);
    }
    FT_TRY(content, file_ops::read_file(epoch_path(), 4096));
    const std::size_t first_newline = content.find('\n');
    if (first_newline == std::string::npos || content.substr(0, first_newline) != kEpochBanner) {
      return Error(ErrorCode::IntegrityFailure, "writer epoch file does not match the expected schema")
          .with_subject(epoch_path().string());
    }
    const std::size_t second_newline = content.find('\n', first_newline + 1);
    if (second_newline == std::string::npos || second_newline < first_newline + 7 ||
        content.substr(first_newline + 1, 6) != "epoch ") {
      return Error(ErrorCode::IntegrityFailure, "writer epoch file is truncated").with_subject(epoch_path().string());
    }
    const std::optional<std::uint64_t> value = text::parse_u64(
        std::string_view(content).substr(first_newline + 7, second_newline - first_newline - 7));
    if (!value.has_value()) {
      return Error(ErrorCode::IntegrityFailure, "writer epoch file does not contain a canonical number")
          .with_subject(epoch_path().string());
    }
    return WriterEpoch(*value);
  }

  Result<void> write_epoch(WriterEpoch value) const {
    std::string content(kEpochBanner);
    content.push_back('\n');
    content.append("epoch ");
    content.append(text::format_u64(value.value()));
    content.push_back('\n');
    return file_ops::atomic_write_file(epoch_path(), content);
  }

  // -- generation files -----------------------------------------------------

  /// Generation numbers present in the generation directory, ascending. Names
  /// that do not match the generation pattern are ignored.
  Result<std::vector<TopologyGeneration>> list_generation_numbers() const {
    std::vector<TopologyGeneration> numbers;
    FT_TRY(names, file_ops::list_directory(generations_path()));
    for (const std::string& name : names) {
      const Result<std::uint64_t> number = parse_generation_file_name(name);
      if (number.has_value()) {
        numbers.push_back(TopologyGeneration(*number));
      }
    }
    std::sort(numbers.begin(), numbers.end());
    return numbers;
  }

  /// Reads, parses and structurally validates one generation file.
  Result<GenerationDocument> load_document_verified(TopologyGeneration generation) const {
    const std::filesystem::path path = generation_path(generation);
    if (!file_ops::is_regular_file(path)) {
      return Error(ErrorCode::GenerationNotRetained, "generation is not present in this store")
          .with_subject(text::format_u64(generation.value()));
    }
    FT_TRY(content, file_ops::read_file(path, config.limits.max_document_bytes));
    FT_TRY(document, parse_generation(content, config.limits));
    if (document.manifest.generation != generation) {
      return Error(ErrorCode::GenerationNotRetained, "generation file name and document generation disagree")
          .with_subject(text::format_u64(generation.value()));
    }
    return document;
  }

  Result<std::string> load_document_text_verified(TopologyGeneration generation, Digest& digest_out) const {
    FT_TRY(verified, load_verified(generation));
    digest_out = verified.digest;
    return verified.text;
  }

  void remember(const GenerationDocument& document, const Digest& digest) const {
    cache_ = CachedGeneration{document.manifest.generation, digest, document};
  }

  void forget() const noexcept { cache_.reset(); }

  Result<VerifiedDocument> load_verified(TopologyGeneration generation) const {
    const std::filesystem::path path = generation_path(generation);
    if (!file_ops::is_regular_file(path)) {
      return Error(ErrorCode::GenerationNotRetained, "generation is not present in this store")
          .with_subject(text::format_u64(generation.value()));
    }
    FT_TRY(content, file_ops::read_file(path, config.limits.max_document_bytes));
    const Digest digest = document_digest(content);
    if (cache_.has_value() && cache_->generation == generation && digest_equal(cache_->digest, digest)) {
      VerifiedDocument verified;
      verified.digest = digest;
      verified.text = std::move(content);
      verified.document = cache_->document;
      return verified;
    }
    FT_TRY(document, parse_generation(content, config.limits));
    if (document.manifest.generation != generation) {
      return Error(ErrorCode::GenerationNotRetained, "generation file name and document generation disagree")
          .with_subject(text::format_u64(generation.value()));
    }
    VerifiedDocument verified;
    verified.digest = digest;
    verified.text = std::move(content);
    verified.document = std::move(document);
    remember(verified.document, verified.digest);
    return verified;
  }

  /// Verifies every generation file and reports the ones that failed.
  Result<std::vector<ScannedGeneration>> scan_generations(std::vector<std::string>& strays) const {
    std::vector<ScannedGeneration> scanned;
    FT_TRY(names, file_ops::list_directory(generations_path()));
    for (const std::string& name : names) {
      const std::filesystem::path path = generations_path() / name;
      if (!file_ops::is_regular_file(path)) {
        strays.push_back(name);
        continue;
      }
      const Result<std::uint64_t> number = parse_generation_file_name(name);
      if (!number.has_value()) {
        strays.push_back(name);
        continue;
      }
      ScannedGeneration entry;
      entry.generation = TopologyGeneration(*number);
      const Result<std::string> content = file_ops::read_file(path, config.limits.max_document_bytes);
      if (!content.has_value()) {
        entry.failure = content.error().to_string();
        scanned.push_back(std::move(entry));
        continue;
      }
      const Result<GenerationDocument> document = parse_generation(*content, config.limits);
      if (!document.has_value()) {
        entry.failure = document.error().to_string();
        scanned.push_back(std::move(entry));
        continue;
      }
      if (document->manifest.generation != entry.generation) {
        entry.failure = "generation file name and document generation disagree";
        scanned.push_back(std::move(entry));
        continue;
      }
      entry.verified = true;
      entry.document_digest = document_digest(*content);
      scanned.push_back(std::move(entry));
    }
    std::sort(scanned.begin(), scanned.end(), [](const ScannedGeneration& lhs, const ScannedGeneration& rhs) {
      return lhs.generation < rhs.generation;
    });
    return scanned;
  }

  Result<std::filesystem::path> quarantine_file(const std::filesystem::path& path) const {
    const std::string base = path.filename().string();
    for (std::uint64_t attempt = 0; attempt < 100000; ++attempt) {
      std::string candidate = base + ".quarantined";
      if (attempt > 0) {
        candidate.push_back('-');
        candidate.append(text::format_u64(attempt));
      }
      const std::filesystem::path target = quarantine_path() / candidate;
      if (file_ops::exists(target)) {
        continue;
      }
      FT_TRYV(file_ops::rename_replace(path, target));
      return target;
    }
    return Error(ErrorCode::IoError, "cannot find a free quarantine name").with_subject(path.string());
  }

  /// Deletes the oldest generations beyond `retained_count` and returns the
  /// number of files actually removed.
  ///
  /// Pruning works from the generation file names alone. It runs on every
  /// publication, so re-reading and re-verifying the retained documents here
  /// would make the cost of a commit grow with the retention window times the
  /// size of the topology. Verifying what is on disk is recovery's job;
  /// pruning only decides which superseded files may be retired.
  Result<std::size_t> prune_generations(std::size_t retained_count) const {
    FT_TRY(numbers, list_generation_numbers());
    if (numbers.size() <= retained_count) {
      return std::size_t{0};
    }
    std::size_t removed = 0;
    for (std::size_t index = 0; index + retained_count < numbers.size(); ++index) {
      FT_TRY(deleted, file_ops::remove_file(generation_path(numbers[index])));
      if (deleted) {
        ++removed;
      }
    }
    return removed;
  }

  // -- publication ----------------------------------------------------------

  /// Publishes a prepared document as `manifest.generation` and points the
  /// head at it. Must run while the exclusive store lock is held.
  Result<MutationOutcome> publish_document(std::string document, const GenerationManifest& manifest,
                                           MutationOutcome outcome) const {
    if (document.size() > config.limits.max_document_bytes) {
      return Error(ErrorCode::LimitExceeded, "serialized generation exceeds the configured maximum document size")
          .with_subject(text::format_u64(manifest.generation.value()));
    }

    const std::filesystem::path pending =
        pending_path() / (generation_file_name(manifest.generation.value()) + ".tmp-" + file_ops::process_id_token() +
                          "-" + file_ops::next_sequence_token());
    FT_TRYV(file_ops::atomic_write_file(pending, document));
    inject_fault(config.fault_point, FaultPoint::AfterPendingWrite);

    // The atomic rename into the generation directory is the commit point: a
    // file that appears there is complete, flushed and integrity-checked.
    const std::filesystem::path committed = generation_path(manifest.generation);
    const Result<void> published = file_ops::rename_replace(pending, committed);
    if (!published.has_value()) {
      (void)file_ops::remove_file(pending);
      return published.error();
    }
    inject_fault(config.fault_point, FaultPoint::AfterGenerationCommit);

    FT_TRYV(write_head(manifest.generation, document_digest(document)));
    inject_fault(config.fault_point, FaultPoint::AfterHeadReplace);

    // Retention is best effort after the commit point: failing to remove a
    // superseded generation leaves a file that the next commit or a recovery
    // prunes. It never affects the published generation.
    (void)prune_generations(config.limits.retained_generations);

    outcome.published = true;
    outcome.committed = true;
    outcome.new_generation = manifest.generation;
    return outcome;
  }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TopologyStore::TopologyStore() noexcept = default;
TopologyStore::TopologyStore(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TopologyStore::~TopologyStore() = default;
TopologyStore::TopologyStore(TopologyStore&&) noexcept = default;
TopologyStore& TopologyStore::operator=(TopologyStore&&) noexcept = default;

Result<TopologyStore> TopologyStore::open(const std::filesystem::path& directory, OpenMode mode, StoreConfig config) {
  std::string explanation;
  if (!validate_limits(config.limits, explanation)) {
    return Error(ErrorCode::InvalidArgument, "invalid topology limits: " + explanation);
  }
  if (directory.empty()) {
    return Error(ErrorCode::PathInvalid, "store directory path is empty");
  }

  auto impl = std::make_unique<Impl>();
  impl->directory = directory;
  impl->mode = mode;
  impl->config = config;

  const bool writable = (mode == OpenMode::ReadWrite);
  if (!file_ops::exists(directory)) {
    if (!writable) {
      return Error(ErrorCode::StoreNotFound, "store directory does not exist").with_subject(directory.string());
    }
    if (!config.create_directory) {
      return Error(ErrorCode::StoreNotFound, "store directory does not exist and creation is disabled")
          .with_subject(directory.string());
    }
    FT_TRYV(file_ops::create_directories(directory));
  } else if (!file_ops::is_directory(directory)) {
    return Error(ErrorCode::PathInvalid, "store path exists but is not a directory").with_subject(directory.string());
  }

  if (writable) {
    FT_TRYV(file_ops::create_directories(impl->generations_path()));
    FT_TRYV(file_ops::create_directories(impl->pending_path()));
    FT_TRYV(file_ops::create_directories(impl->quarantine_path()));

    // Reserving mutation authority is the only thing done while the lock is
    // held: the epoch, not a held lock, is what authorizes later commits.
    Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl->lock_path(), file_ops::LockMode::Exclusive);
    if (!lock.has_value()) {
      return lock.error();
    }
    FT_TRY(current, impl->read_epoch());
    FT_TRY(next, current.next());
    FT_TRYV(impl->write_epoch(next));
    impl->epoch = next;
  }

  impl->open = true;
  return TopologyStore(std::move(impl));
}

Result<void> TopologyStore::close() {
  if (impl_ == nullptr) {
    return ok();
  }
  impl_->open = false;
  impl_->epoch = WriterEpoch(0);
  impl_.reset();
  return ok();
}

bool TopologyStore::is_open() const noexcept { return impl_ != nullptr && impl_->open; }

OpenMode TopologyStore::mode() const noexcept { return (impl_ == nullptr) ? OpenMode::ReadOnly : impl_->mode; }

bool TopologyStore::writable() const noexcept { return is_open() && impl_->mode == OpenMode::ReadWrite; }

WriterEpoch TopologyStore::epoch() const noexcept { return (impl_ == nullptr) ? WriterEpoch(0) : impl_->epoch; }

const std::filesystem::path& TopologyStore::directory() const noexcept {
  static const std::filesystem::path kEmpty;
  return (impl_ == nullptr) ? kEmpty : impl_->directory;
}

const TopologyLimits& TopologyStore::limits() const noexcept {
  static const TopologyLimits kDefault{};
  return (impl_ == nullptr) ? kDefault : impl_->config.limits;
}

Result<StoreStatus> TopologyStore::status() const {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  StoreStatus status;
  status.open = true;
  status.writable = writable();
  status.epoch = impl_->epoch;

  FT_TRY(numbers, impl_->list_generation_numbers());
  status.generation_count = numbers.size();
  if (!numbers.empty()) {
    status.max_committed = numbers.back();
  }

  FT_TRY(pending, file_ops::list_directory(impl_->pending_path()));
  status.pending_count = pending.size();
  FT_TRY(quarantine, file_ops::list_directory(impl_->quarantine_path()));
  status.quarantine_count = quarantine.size();

  const Result<HeadMarker> marker = impl_->read_head();
  if (!marker.has_value()) {
    status.needs_recovery = true;
    status.explanation = marker.error().to_string();
    return status;
  }
  status.initialized = marker->present;
  status.head = marker->generation;
  status.needs_recovery = !marker->present || marker->generation != status.max_committed || status.pending_count > 0;
  if (!marker->present) {
    status.explanation = "no head marker is published";
  } else if (marker->generation != status.max_committed) {
    status.explanation = "the head marker is not the newest committed generation";
  } else if (status.pending_count > 0) {
    status.explanation = "a commit was interrupted and left temporary state";
  } else {
    status.explanation = "head marker matches the newest committed generation";
  }
  return status;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

Result<GenerationDocument> TopologyStore::load_document(TopologyGeneration generation) const {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Shared);
  if (!lock.has_value()) {
    return lock.error();
  }
  return impl_->load_document_verified(generation);
}

Result<TopologySnapshot> TopologyStore::load_generation(TopologyGeneration generation) const {
  FT_TRY(document, load_document(generation));
  return document.snapshot;
}

Result<std::vector<TopologyGeneration>> TopologyStore::generation_numbers() const {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Shared);
  if (!lock.has_value()) {
    return lock.error();
  }
  return impl_->list_generation_numbers();
}

Result<GenerationDocument> TopologyStore::head_document() const {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Shared);
  if (!lock.has_value()) {
    return lock.error();
  }
  FT_TRY(marker, impl_->read_head());
  if (!marker.present) {
    return Error(ErrorCode::HeadMissing, "the store has no published head; run recover()")
        .with_subject(impl_->head_path().string());
  }
  FT_TRY(verified, impl_->load_verified(marker.generation));
  if (!digest_equal(verified.digest, marker.document_digest)) {
    return Error(ErrorCode::HeadCorrupt, "the head marker digest does not match the generation it names")
        .with_subject(impl_->head_path().string());
  }
  return verified.document;
}

Result<GenerationManifest> TopologyStore::head_manifest() const {
  FT_TRY(document, head_document());
  return document.manifest;
}

Result<TopologySnapshot> TopologyStore::head() const {
  FT_TRY(document, head_document());
  return document.snapshot;
}

Result<TopologyGeneration> TopologyStore::head_generation() const {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Shared);
  if (!lock.has_value()) {
    return lock.error();
  }
  FT_TRY(marker, impl_->read_head());
  if (!marker.present) {
    return Error(ErrorCode::HeadMissing, "the store has no published head; run recover()")
        .with_subject(impl_->head_path().string());
  }
  return marker.generation;
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

Result<MutationOutcome> TopologyStore::create(const TopologySnapshot& initial, const CreateOptions& options) {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  if (impl_->mode != OpenMode::ReadWrite) {
    return Error(ErrorCode::StoreReadOnly, "store was opened read-only; mutation authority was not requested")
        .with_subject(impl_->directory.string());
  }
  if (options.actor.empty()) {
    return Error(ErrorCode::MissingField, "create actor must not be empty");
  }
  FT_TRYV(validate_source(options.source, impl_->config.limits));
  FT_TRYV(validate_reason(options.reason, impl_->config.limits));
  if (!options.recorded_at.empty() && !is_canonical_utc_timestamp(options.recorded_at)) {
    return Error(ErrorCode::MalformedRecord, "create timestamp must be canonical UTC");
  }
  if (options.mutation_id.empty()) {
    return Error(ErrorCode::MissingField,
                 "creating a durable generation requires a mutation identity so retries are idempotent");
  }
  if (!initial.valid()) {
    return Error(ErrorCode::NotInitialized, "create requires an assembled initial topology");
  }

  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Exclusive);
  if (!lock.has_value()) {
    return lock.error();
  }
  FT_TRY(current_epoch, impl_->read_epoch());
  if (current_epoch != impl_->epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch,
                 "another writer has taken mutation authority for this store; reopen it")
        .with_subject(impl_->directory.string());
  }
  FT_TRY(existing, impl_->read_head());
  if (existing.present) {
    return Error(ErrorCode::StoreNotEmpty, "the store already has a published head")
        .with_subject(impl_->directory.string());
  }
  FT_TRY(numbers, impl_->list_generation_numbers());
  if (!numbers.empty()) {
    return Error(ErrorCode::RecoveryRequired,
                 "the store holds committed generations but no head marker; run recover() first")
        .with_subject(impl_->directory.string());
  }
  FT_TRY(quarantine, file_ops::list_directory(impl_->quarantine_path()));
  if (!quarantine.empty()) {
    return Error(ErrorCode::StoreNotEmpty,
                 "the store holds quarantined evidence; inspect it before creating a new generation")
        .with_subject(impl_->directory.string());
  }

  // The created generation is always generation 1: an initial topology is the
  // root of this store's history regardless of the generation number the
  // source document carried.
  Result<TopologyBuilder> builder = TopologyBuilder::from_snapshot(initial);
  if (!builder.has_value()) {
    return builder.error();
  }
  builder->set_limits(impl_->config.limits);
  const ValidationReport report = builder->validate();
  if (!report.valid) {
    const ValidationIssue& issue = report.issues.front();
    return Error(issue.code, "initial topology is not structurally valid: " + issue.explanation)
        .with_subject(issue.subject);
  }
  FT_TRY(snapshot, builder->build(TopologyGeneration(TopologyGeneration::kFirstPublished)));

  GenerationManifest manifest;
  manifest.generation = TopologyGeneration(TopologyGeneration::kFirstPublished);
  manifest.parent_generation = TopologyGeneration(0);
  manifest.authority_epoch = impl_->epoch;
  manifest.actor = options.actor;
  manifest.source = options.source;
  manifest.reason = options.reason;
  manifest.mutation_id = options.mutation_id;
  manifest.retention_floor = TopologyGeneration::kFirstPublished;
  if (!options.recorded_at.empty()) {
    manifest.recorded_at = options.recorded_at;
  } else {
    FT_TRY(stamp, impl_->timestamp_now());
    manifest.recorded_at = stamp;
  }

  FT_TRY(document, serialize_generation(snapshot, manifest));
  MutationOutcome outcome;
  outcome.base_generation = TopologyGeneration(0);
  outcome.code = ErrorCode::Ok;
  outcome.content_digest = snapshot.content_digest();
  outcome.explanation = "created generation 1 with " + std::to_string(snapshot.stats().nodes) + " nodes";
  const Digest document_digest_value = document_digest(document);
  FT_TRY(published, impl_->publish_document(std::move(document), manifest, std::move(outcome)));
  GenerationDocument created;
  created.manifest = manifest;
  created.snapshot = snapshot;
  impl_->remember(created, document_digest_value);
  return published;
}

Result<MutationOutcome> TopologyStore::commit(const MutationBatch& batch, const CommitOptions& options) {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  if (impl_->mode != OpenMode::ReadWrite) {
    return Error(ErrorCode::StoreReadOnly, "store was opened read-only; mutation authority was not requested")
        .with_subject(impl_->directory.string());
  }
  if (options.stop.has_value() && options.stop->stop_requested()) {
    return Error(ErrorCode::Cancelled, "commit was cancelled before any work started");
  }
  FT_TRYV(validate_batch(batch, impl_->config.limits));
  if (batch.mutation_id.empty()) {
    return Error(ErrorCode::MissingField, "a durable commit requires a mutation identity so retries are idempotent");
  }
  if (!batch.authority_epoch.valid() || batch.authority_epoch != impl_->epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch,
                 "the batch carries writer epoch " + text::format_u64(batch.authority_epoch.value()) +
                     " but this handle holds " + text::format_u64(impl_->epoch.value()))
        .with_subject(impl_->directory.string());
  }

  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Exclusive);
  if (!lock.has_value()) {
    return lock.error();
  }
  FT_TRY(current_epoch, impl_->read_epoch());
  if (current_epoch != impl_->epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch,
                 "another writer has taken mutation authority for this store; reopen it")
        .with_subject(impl_->directory.string());
  }

  FT_TRY(marker, impl_->read_head());
  if (!marker.present) {
    return Error(ErrorCode::RecoveryRequired, "the store has no readable head marker; run recover() before committing")
        .with_subject(impl_->head_path().string());
  }
  Digest head_digest{};
  FT_TRY(head_verified, impl_->load_verified(marker.generation));
  head_digest = head_verified.digest;
  if (!digest_equal(head_digest, marker.document_digest)) {
    return Error(ErrorCode::HeadCorrupt, "the head marker digest does not match the generation it names")
        .with_subject(impl_->head_path().string());
  }
  GenerationDocument& head_document = head_verified.document;

  // Idempotency is decided before the base-generation precondition: a retry of
  // a batch that already succeeded necessarily arrives after the head advanced
  // past the batch's base, and that is a replay rather than a stale request.
  const Digest request_digest = batch_digest(batch);
  for (const AppliedBatch& applied : head_document.manifest.applied) {
    if (applied.mutation_id != batch.mutation_id) {
      continue;
    }
    if (!digest_equal(applied.batch_digest, request_digest)) {
      return Error(ErrorCode::IdentityConflict, "this mutation identity was already applied with different content")
          .with_subject(batch.mutation_id.str());
    }
    MutationOutcome outcome;
    outcome.replayed = true;
    outcome.base_generation = batch.base_generation;
    outcome.new_generation = applied.generation;
    outcome.code = ErrorCode::Ok;
    outcome.explanation = "batch identity " + batch.mutation_id.str() + " was already applied at generation " +
                          text::format_u64(applied.generation.value()) + "; nothing was written";
    return outcome;
  }

  if (marker.generation != batch.base_generation) {
    return Error(ErrorCode::StaleBaseGeneration,
                 "the batch is based on generation " + text::format_u64(batch.base_generation.value()) +
                     " but the current generation is " + text::format_u64(marker.generation.value()))
        .with_subject(impl_->directory.string());
  }

  if (options.stop.has_value() && options.stop->stop_requested()) {
    return Error(ErrorCode::Cancelled, "commit was cancelled before the candidate generation was built");
  }

  BatchApplication application = apply_batch_explained(head_document.snapshot, batch, impl_->config.limits);
  if (!application.accepted()) {
    const ErrorCode code = (application.outcome.first_failure != ErrorCode::Ok) ? application.outcome.first_failure
                                                                               : application.outcome.code;
    return Error(code, application.outcome.explain()).with_subject(batch.mutation_id.str());
  }
  const TopologySnapshot& candidate = application.candidate;

  GenerationManifest manifest;
  manifest.generation = candidate.generation();
  manifest.parent_generation = marker.generation;
  manifest.parent_digest = head_digest;
  manifest.authority_epoch = impl_->epoch;
  manifest.actor = batch.actor;
  manifest.source = batch.source;
  manifest.reason = batch.reason;
  manifest.mutation_id = batch.mutation_id;
  manifest.retention_floor = head_document.manifest.retention_floor;
  if (!batch.recorded_at.empty()) {
    manifest.recorded_at = batch.recorded_at;
  } else {
    FT_TRY(stamp, impl_->timestamp_now());
    manifest.recorded_at = stamp;
  }

  AppliedBatch applied;
  applied.mutation_id = batch.mutation_id;
  applied.batch_digest = request_digest;
  applied.generation = candidate.generation();
  manifest.applied.push_back(std::move(applied));
  for (const AppliedBatch& previous : head_document.manifest.applied) {
    if (manifest.applied.size() >= impl_->config.limits.idempotency_window) {
      break;
    }
    manifest.applied.push_back(previous);
  }

  FT_TRY(serialized, serialize_generation(candidate, manifest));
  MutationOutcome outcome;
  outcome.base_generation = batch.base_generation;
  outcome.code = ErrorCode::Ok;
  outcome.content_digest = candidate.content_digest();
  outcome.dispositions = application.outcome.dispositions;
  outcome.explanation = "published generation " + text::format_u64(candidate.generation().value()) + " from base " +
                        text::format_u64(marker.generation.value()) + " (" +
                        std::to_string(batch.mutations.size()) + " mutations applied)";
  const Digest published_digest = document_digest(serialized);
  FT_TRY(published, impl_->publish_document(std::move(serialized), manifest, std::move(outcome)));
  GenerationDocument published_document;
  published_document.manifest = manifest;
  published_document.snapshot = candidate;
  impl_->remember(published_document, published_digest);
  return published;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Result<RecoveryReport> TopologyStore::recover() {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  if (impl_->mode != OpenMode::ReadWrite) {
    return Error(ErrorCode::StoreReadOnly, "recovery rewrites the head marker and needs a read-write handle")
        .with_subject(impl_->directory.string());
  }

  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Exclusive);
  if (!lock.has_value()) {
    return lock.error();
  }
  impl_->forget();

  RecoveryReport report;
  const Result<HeadMarker> marker = impl_->read_head();
  const bool head_readable = marker.has_value();
  const bool head_present = head_readable && marker->present;
  if (head_present) {
    report.head_before = marker->generation;
    report.head_after = marker->generation;
  }

  // 1. Retire temporary state that never reached its commit point. A pending
  //    file carries no authority: it is the residue of an interrupted commit.
  FT_TRY(pending, file_ops::list_directory(impl_->pending_path()));
  for (const std::string& name : pending) {
    const std::filesystem::path path = impl_->pending_path() / name;
    if (!file_ops::is_regular_file(path)) {
      continue;
    }
    FT_TRY(removed, file_ops::remove_file(path));
    if (removed) {
      ++report.pending_files_discarded;
    }
  }
  FT_TRY(root_names, file_ops::list_directory(impl_->directory));
  for (const std::string& name : root_names) {
    if (name.find(".tmp-") == std::string::npos) {
      continue;
    }
    const std::filesystem::path path = impl_->directory / name;
    if (file_ops::is_regular_file(path)) {
      FT_TRY(removed, file_ops::remove_file(path));
      if (removed) {
        ++report.pending_files_discarded;
      }
    }
  }

  // 2. Verify every generation file; quarantine, never rewrite, the ones that
  //    fail. Quarantined evidence stays readable for an operator.
  std::vector<std::string> strays;
  FT_TRY(scanned, impl_->scan_generations(strays));
  for (const std::string& name : strays) {
    FT_TRYV(impl_->quarantine_file(impl_->generations_path() / name));
    ++report.corrupt_files_quarantined;
  }
  std::vector<ScannedGeneration> verified;
  for (const ScannedGeneration& entry : scanned) {
    if (entry.verified) {
      verified.push_back(entry);
      continue;
    }
    FT_TRYV(impl_->quarantine_file(impl_->generation_path(entry.generation)));
    ++report.corrupt_files_quarantined;
  }

  // 3. Restore the retention bound so persisted growth stays bounded even when
  //    a crash happened before pruning.
  const std::size_t keep = impl_->config.limits.retained_generations;
  if (verified.size() > keep) {
    const std::size_t drop = verified.size() - keep;
    FT_TRY(pruned, impl_->prune_generations(keep));
    report.generations_pruned = pruned;
    verified.erase(verified.begin(), verified.begin() + static_cast<std::ptrdiff_t>(drop));
  }
  for (const ScannedGeneration& entry : verified) {
    report.retained.push_back(entry.generation);
  }

  // 4. Re-point the head at the highest generation that verifies, keeping that
  //    generation's own number. Recovery never invents a generation number and
  //    never promotes temporary state to authoritative state.
  if (verified.empty()) {
    if (head_present) {
      FT_TRYV(file_ops::remove_file(impl_->head_path()));
    }
    report.head_after = TopologyGeneration(0);
    report.repaired =
        report.pending_files_discarded > 0 || report.corrupt_files_quarantined > 0 || head_present;
    report.explanation = report.repaired
                             ? "no verified generation remains; quarantined evidence was preserved and the store is "
                               "uninitialized"
                             : "nothing to recover";
    return report;
  }

  const ScannedGeneration& newest = verified.back();
  const bool head_ok = head_present && marker->generation == newest.generation &&
                       digest_equal(marker->document_digest, newest.document_digest);
  if (!head_ok) {
    // A head marker that exists but is not a regular file (a symbolic link, a
    // directory, a device) is evidence of tampering rather than a marker:
    // preserve it in quarantine and publish a verified marker instead.
    if (file_ops::exists(impl_->head_path()) && !file_ops::is_regular_file(impl_->head_path())) {
      FT_TRYV(impl_->quarantine_file(impl_->head_path()));
      ++report.corrupt_files_quarantined;
    }
    FT_TRYV(impl_->write_head(newest.generation, newest.document_digest));
    report.recovered = true;
    report.head_after = newest.generation;
  }
  report.repaired = report.recovered || report.pending_files_discarded > 0 ||
                    report.corrupt_files_quarantined > 0 || report.generations_pruned > 0;
  report.explanation = report.repaired
                           ? "recovered head generation " + text::format_u64(newest.generation.value()) +
                                 " from verified committed generations"
                           : "head marker already matches the newest verified generation";
  return report;
}

// ---------------------------------------------------------------------------
// Export and import
// ---------------------------------------------------------------------------

Result<void> TopologyStore::export_head_to(const std::filesystem::path& file) const {
  if (impl_ == nullptr || !impl_->open) {
    return Error(ErrorCode::StoreClosed, "store handle is closed");
  }
  if (file.empty()) {
    return Error(ErrorCode::PathInvalid, "export path is empty");
  }
  Result<file_ops::FileLock> lock = file_ops::FileLock::acquire(impl_->lock_path(), file_ops::LockMode::Shared);
  if (!lock.has_value()) {
    return lock.error();
  }
  FT_TRY(marker, impl_->read_head());
  if (!marker.present) {
    return Error(ErrorCode::HeadMissing, "the store has no published head").with_subject(impl_->head_path().string());
  }
  Digest digest{};
  FT_TRY(content, impl_->load_document_text_verified(marker.generation, digest));
  return file_ops::atomic_write_file(file, content);
}

Result<GenerationDocument> TopologyStore::import_from(const std::filesystem::path& file, const TopologyLimits& limits) {
  std::string explanation;
  if (!validate_limits(limits, explanation)) {
    return Error(ErrorCode::InvalidArgument, "invalid topology limits: " + explanation);
  }
  if (file.empty()) {
    return Error(ErrorCode::PathInvalid, "import path is empty");
  }
  FT_TRY(content, file_ops::read_file(file, limits.max_document_bytes));
  return parse_generation(content, limits);
}

}  // namespace dccp::facility_topology
