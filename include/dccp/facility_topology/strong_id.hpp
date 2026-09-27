// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_STRONG_ID_HPP
#define DCCP_FACILITY_TOPOLOGY_STRONG_ID_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology {

/// Maximum length, in bytes, of any identifier accepted by the library.
inline constexpr std::size_t kMaxIdentifierBytes = 128;

/// Identifier syntax (canonical form):
///   - 1..128 bytes;
///   - first and last byte are ASCII alphanumeric;
///   - interior bytes are ASCII alphanumeric or one of '.', ':', '-'.
///
/// The grammar is deliberately free of whitespace, quoting, path separators
/// and non-ASCII bytes so that canonical serialization is escape-free and
/// identity strings cannot smuggle paths, control characters or look-alikes.
bool is_valid_identifier_syntax(std::string_view raw) noexcept;

/// Explains why an identifier was rejected (empty result when it is valid).
std::string_view identifier_syntax_help() noexcept;

/// Tag types selecting a distinct StrongId instantiation. Unrelated identities
/// are distinct types and cannot be implicitly converted into one another.
struct NodeIdTag {
  static constexpr std::string_view kind_name = "node";
};
struct DomainIdTag {
  static constexpr std::string_view kind_name = "domain";
};
struct ActorIdTag {
  static constexpr std::string_view kind_name = "actor";
};
struct StoreIdTag {
  static constexpr std::string_view kind_name = "store";
};
struct MutationIdTag {
  static constexpr std::string_view kind_name = "mutation";
};

/// A validated, strongly typed identifier.
///
/// Construction only succeeds through parse(); the default-constructed value is
/// empty and only exists so that identifiers can live in containers. An empty
/// identifier is never written to durable state and is rejected by every public
/// entry point that requires one.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  StrongId() noexcept = default;

  /// Parses and validates untrusted text.
  static Result<StrongId> parse(std::string_view raw) {
    if (!is_valid_identifier_syntax(raw)) {
      return Error(ErrorCode::MalformedIdentifier,
                   "identifier does not match the canonical grammar (1..128 bytes, "
                   "ASCII alphanumeric first/last byte, interior [A-Za-z0-9._:-])")
          .with_subject(std::string(raw.substr(0, 160)));
    }
    return StrongId(std::string(raw));
  }

  bool empty() const noexcept { return value_.empty(); }
  std::string_view value() const noexcept { return value_; }
  const std::string& str() const noexcept { return value_; }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept = default;

  /// Ordering is byte-wise over the canonical identifier, so iteration order is
  /// identical on every platform.
  friend std::strong_ordering operator<=>(const StrongId& lhs, const StrongId& rhs) noexcept {
    const int cmp = lhs.value_.compare(rhs.value_);
    return cmp < 0 ? std::strong_ordering::less
                   : (cmp > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
  }

 private:
  explicit StrongId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

using NodeId = StrongId<NodeIdTag>;
using DomainId = StrongId<DomainIdTag>;
using ActorId = StrongId<ActorIdTag>;
using StoreId = StrongId<StoreIdTag>;

/// Caller-supplied idempotency key of one mutation batch.
///
/// The type is deliberately distinct from every other identity so a mutation
/// key can never be passed where a node or actor identity is expected.
using MutationId = StrongId<MutationIdTag>;

/// Monotonic generation counter of published topology state.
///
/// Generation 0 means "no topology published yet"; published generations start
/// at 1. Distinct from WriterEpoch and from revision-like counters: mixing them
/// requires an explicit conversion, and no arithmetic is implicit.
class TopologyGeneration {
 public:
  static constexpr std::uint64_t kFirstPublished = 1;

  constexpr TopologyGeneration() noexcept = default;
  explicit constexpr TopologyGeneration(std::uint64_t value) noexcept : value_(value) {}

  static Result<TopologyGeneration> parse(std::uint64_t value) {
    return TopologyGeneration(value);
  }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool published() const noexcept { return value_ != 0; }

  /// Strictly increasing successor. Overflow is reported, never wrapped.
  Result<TopologyGeneration> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::GenerationOverflow, "generation counter exhausted").with_subject(std::to_string(value_));
    }
    return TopologyGeneration(value_ + 1);
  }

  friend constexpr bool operator==(const TopologyGeneration&, const TopologyGeneration&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const TopologyGeneration& lhs,
                                                    const TopologyGeneration& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Durable mutation-authority epoch of a writer incarnation.
///
/// Every writer that opens a store for mutation advances the epoch. Operations
/// carry the epoch they were authorized under; an operation whose epoch is
/// older than the store's current epoch is rejected instead of being applied to
/// state the writer no longer owns.
class WriterEpoch {
 public:
  constexpr WriterEpoch() noexcept = default;
  explicit constexpr WriterEpoch(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool valid() const noexcept { return value_ != 0; }

  static Result<WriterEpoch> parse(std::uint64_t value) { return WriterEpoch(value); }

  Result<WriterEpoch> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::GenerationOverflow, "writer epoch counter exhausted");
    }
    return WriterEpoch(value_ + 1);
  }

  friend constexpr bool operator==(const WriterEpoch&, const WriterEpoch&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const WriterEpoch& lhs, const WriterEpoch& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

}  // namespace dccp::facility_topology

namespace std {
template <class Tag>
struct hash<dccp::facility_topology::StrongId<Tag>> {
  std::size_t operator()(const dccp::facility_topology::StrongId<Tag>& id) const noexcept {
    return std::hash<std::string_view>{}(id.value());
  }
};
template <>
struct hash<dccp::facility_topology::WriterEpoch> {
  std::size_t operator()(const dccp::facility_topology::WriterEpoch& epoch) const noexcept {
    return std::hash<std::uint64_t>{}(epoch.value());
  }
};
}  // namespace std

#endif  // DCCP_FACILITY_TOPOLOGY_STRONG_ID_HPP
