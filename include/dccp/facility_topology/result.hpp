// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_RESULT_HPP
#define DCCP_FACILITY_TOPOLOGY_RESULT_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dccp::facility_topology {

/// Stable, machine-readable outcome codes.
///
/// These codes are part of the public contract: values are appended to, never
/// renumbered or repurposed. The textual name returned by error_code_name() is
/// equally stable and is what the CLI prints as "error-code=<NAME>".
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Input shape and encoding (untrusted data).
  InvalidArgument,
  MalformedIdentifier,
  IdentifierTooLong,
  InvalidUtf8,
  TextTooLong,
  UnknownEnumToken,
  MissingField,
  DuplicateField,
  MalformedRecord,
  UnsupportedSchemaVersion,
  CountMismatch,
  TruncatedInput,
  DigestMismatch,
  LimitExceeded,

  // Structural and semantic rejection.
  NotFound,
  AlreadyPresent,
  AlreadyAtParent,
  IdentityConflict,
  InvalidParentKind,
  FacilityMustBeRoot,
  NonFacilityMustBeContained,
  ContainmentCycle,
  NodeHasChildren,
  NodeReferenced,
  SelfEdge,
  DuplicateEdge,
  AdjacencyCrossFacility,
  ZoneMemberKindInvalid,
  ZoneTargetKindInvalid,
  ZoneConflict,
  MissingEndpoint,
  InvalidGeneration,
  GenerationOverflow,

  // Authority, generations and lifecycle.
  BatchRejected,
  StaleBaseGeneration,
  StaleAuthorityEpoch,
  StoreLocked,
  StoreClosed,
  StoreNotFound,
  StoreNotEmpty,
  StoreMismatch,
  GenerationAlreadyExists,
  GenerationNotRetained,
  HeadMissing,
  HeadCorrupt,
  RecoveryRequired,
  RecoveryUnavailable,
  RecoveryNotNeeded,
  IntegrityFailure,
  IoError,
  Cancelled,
  TraversalDepthExceeded,
  InternalError,

  // Appended after the initial vocabulary: existing values are never
  // renumbered, so new codes only ever extend the list.
  StoreReadOnly,
  NotInitialized,
  InvalidState,
  LockUnavailable,
  PathInvalid,
};

/// Coarse classification of an ErrorCode, for callers that branch on the kind
/// of failure rather than the exact code.
enum class ErrorCategory : std::uint8_t {
  Ok = 0,
  Argument,     // caller-supplied or untrusted input was rejected
  Structure,    // the topology structure would be invalid
  Authority,    // generation/epoch precondition failed
  Persistence,  // durable state is missing, corrupt or unwritable
  Lifecycle,    // the store is locked, closed, or in the wrong state
  Limit,        // a configured bound was exceeded
  Cancelled,    // the operation was cancelled before publication
  Internal,     // defect in the library
};

/// Stable textual name of an error code (upper snake case).
std::string_view error_code_name(ErrorCode code) noexcept;

/// Category of an error code.
ErrorCategory error_category(ErrorCode code) noexcept;

/// Human-readable name of a category.
std::string_view error_category_name(ErrorCategory category) noexcept;

/// An error value: stable code, human explanation, and optional subject.
class Error {
 public:
  Error() noexcept = default;

  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  ErrorCode code() const noexcept { return code_; }
  ErrorCategory category() const noexcept { return error_category(code_); }
  const std::string& message() const noexcept { return message_; }

  /// Identifier of the object the error is about, when one exists.
  const std::string& subject() const noexcept { return subject_; }

  Error& with_subject(std::string subject) {
    subject_ = std::move(subject);
    return *this;
  }

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }

  /// "CODE: message" (plus " [subject=...]" when a subject is present).
  std::string to_string() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string subject_;
};

/// Result of an operation that yields a value of type T or an Error.
///
/// The library never uses exceptions for expected failure modes; Result is the
/// only channel for them. value() throws std::logic_error only on programmer
/// error (dereferencing a failed Result).
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(normalize(std::move(error))) {}

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & {
    require_value();
    return *value_;
  }
  const T& value() const& {
    require_value();
    return *value_;
  }
  T&& value() && {
    require_value();
    return std::move(*value_);
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Error& error() const noexcept { return error_; }

 private:
  static Error normalize(Error error) {
    if (error.ok()) {
      return Error(ErrorCode::InternalError, "result constructed without a value or an error");
    }
    return error;
  }

  void require_value() const {
    if (!value_.has_value()) {
      throw std::logic_error("facility_topology: Result has no value: " + error_.to_string());
    }
  }

  std::optional<T> value_;
  Error error_;
};

/// Result specialization for operations that produce no value.
template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Error error) : error_(normalize(std::move(error))) {}

  static Result success() noexcept { return Result(); }

  bool has_value() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return has_value(); }

  const Error& error() const noexcept { return error_; }

 private:
  static Error normalize(Error error) {
    if (error.ok()) {
      return Error(ErrorCode::InternalError, "result constructed without a value or an error");
    }
    return error;
  }

  Error error_;
};

/// Convenience constructors.
inline Error make_error(ErrorCode code, std::string message) { return Error(code, std::move(message)); }

inline Result<void> ok() noexcept { return Result<void>(); }

}  // namespace dccp::facility_topology

/// Propagate a failed Result out of the current function.
///
/// FT_TRY(name, expression) declares "name" bound to the successful value and
/// returns the error if the expression failed.
#define FT_TRY(value_name, expression)          \
  auto value_name##_ft_result = (expression);   \
  if (!value_name##_ft_result.has_value()) {    \
    return value_name##_ft_result.error();      \
  }                                             \
  auto& value_name = *value_name##_ft_result

/// Propagate a failed void Result out of the current function.
#define FT_TRYV(expression)           \
  do {                                \
    auto ft_result_ = (expression);   \
    if (!ft_result_.has_value()) {    \
      return ft_result_.error();      \
    }                                 \
  } while (false)

#endif  // DCCP_FACILITY_TOPOLOGY_RESULT_HPP
