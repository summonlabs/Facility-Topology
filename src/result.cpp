// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
  ErrorCategory category;
};

// Single authoritative table: every enumerator appears exactly once, in the
// order in which it is declared.
constexpr CodeName kCodeNames[] = {
    {ErrorCode::Ok, "OK", ErrorCategory::Ok},
    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT", ErrorCategory::Argument},
    {ErrorCode::MalformedIdentifier, "MALFORMED_IDENTIFIER", ErrorCategory::Argument},
    {ErrorCode::IdentifierTooLong, "IDENTIFIER_TOO_LONG", ErrorCategory::Argument},
    {ErrorCode::InvalidUtf8, "INVALID_UTF8", ErrorCategory::Argument},
    {ErrorCode::TextTooLong, "TEXT_TOO_LONG", ErrorCategory::Argument},
    {ErrorCode::UnknownEnumToken, "UNKNOWN_ENUM_TOKEN", ErrorCategory::Argument},
    {ErrorCode::MissingField, "MISSING_FIELD", ErrorCategory::Argument},
    {ErrorCode::DuplicateField, "DUPLICATE_FIELD", ErrorCategory::Argument},
    {ErrorCode::MalformedRecord, "MALFORMED_RECORD", ErrorCategory::Argument},
    {ErrorCode::UnsupportedSchemaVersion, "UNSUPPORTED_SCHEMA_VERSION", ErrorCategory::Argument},
    {ErrorCode::CountMismatch, "COUNT_MISMATCH", ErrorCategory::Argument},
    {ErrorCode::TruncatedInput, "TRUNCATED_INPUT", ErrorCategory::Argument},
    {ErrorCode::DigestMismatch, "DIGEST_MISMATCH", ErrorCategory::Persistence},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED", ErrorCategory::Limit},
    {ErrorCode::NotFound, "NOT_FOUND", ErrorCategory::Structure},
    {ErrorCode::AlreadyPresent, "ALREADY_PRESENT", ErrorCategory::Structure},
    {ErrorCode::AlreadyAtParent, "ALREADY_AT_PARENT", ErrorCategory::Structure},
    {ErrorCode::IdentityConflict, "IDENTITY_CONFLICT", ErrorCategory::Structure},
    {ErrorCode::InvalidParentKind, "INVALID_PARENT_KIND", ErrorCategory::Structure},
    {ErrorCode::FacilityMustBeRoot, "FACILITY_MUST_BE_ROOT", ErrorCategory::Structure},
    {ErrorCode::NonFacilityMustBeContained, "NON_FACILITY_MUST_BE_CONTAINED", ErrorCategory::Structure},
    {ErrorCode::ContainmentCycle, "CONTAINMENT_CYCLE", ErrorCategory::Structure},
    {ErrorCode::NodeHasChildren, "NODE_HAS_CHILDREN", ErrorCategory::Structure},
    {ErrorCode::NodeReferenced, "NODE_REFERENCED", ErrorCategory::Structure},
    {ErrorCode::SelfEdge, "SELF_EDGE", ErrorCategory::Structure},
    {ErrorCode::DuplicateEdge, "DUPLICATE_EDGE", ErrorCategory::Structure},
    {ErrorCode::AdjacencyCrossFacility, "ADJACENCY_CROSS_FACILITY", ErrorCategory::Structure},
    {ErrorCode::ZoneMemberKindInvalid, "ZONE_MEMBER_KIND_INVALID", ErrorCategory::Structure},
    {ErrorCode::ZoneTargetKindInvalid, "ZONE_TARGET_KIND_INVALID", ErrorCategory::Structure},
    {ErrorCode::ZoneConflict, "ZONE_CONFLICT", ErrorCategory::Structure},
    {ErrorCode::MissingEndpoint, "MISSING_ENDPOINT", ErrorCategory::Structure},
    {ErrorCode::InvalidGeneration, "INVALID_GENERATION", ErrorCategory::Structure},
    {ErrorCode::GenerationOverflow, "GENERATION_OVERFLOW", ErrorCategory::Structure},
    {ErrorCode::BatchRejected, "BATCH_REJECTED", ErrorCategory::Structure},
    {ErrorCode::StaleBaseGeneration, "STALE_BASE_GENERATION", ErrorCategory::Authority},
    {ErrorCode::StaleAuthorityEpoch, "STALE_AUTHORITY_EPOCH", ErrorCategory::Authority},
    {ErrorCode::StoreLocked, "STORE_LOCKED", ErrorCategory::Lifecycle},
    {ErrorCode::StoreClosed, "STORE_CLOSED", ErrorCategory::Lifecycle},
    {ErrorCode::StoreNotFound, "STORE_NOT_FOUND", ErrorCategory::Persistence},
    {ErrorCode::StoreNotEmpty, "STORE_NOT_EMPTY", ErrorCategory::Lifecycle},
    {ErrorCode::StoreMismatch, "STORE_MISMATCH", ErrorCategory::Persistence},
    {ErrorCode::GenerationAlreadyExists, "GENERATION_ALREADY_EXISTS", ErrorCategory::Persistence},
    {ErrorCode::GenerationNotRetained, "GENERATION_NOT_RETAINED", ErrorCategory::Persistence},
    {ErrorCode::HeadMissing, "HEAD_MISSING", ErrorCategory::Persistence},
    {ErrorCode::HeadCorrupt, "HEAD_CORRUPT", ErrorCategory::Persistence},
    {ErrorCode::RecoveryRequired, "RECOVERY_REQUIRED", ErrorCategory::Persistence},
    {ErrorCode::RecoveryUnavailable, "RECOVERY_UNAVAILABLE", ErrorCategory::Persistence},
    {ErrorCode::RecoveryNotNeeded, "RECOVERY_NOT_NEEDED", ErrorCategory::Lifecycle},
    {ErrorCode::IntegrityFailure, "INTEGRITY_FAILURE", ErrorCategory::Persistence},
    {ErrorCode::IoError, "IO_ERROR", ErrorCategory::Persistence},
    {ErrorCode::Cancelled, "CANCELLED", ErrorCategory::Cancelled},
    {ErrorCode::TraversalDepthExceeded, "TRAVERSAL_DEPTH_EXCEEDED", ErrorCategory::Limit},
    {ErrorCode::InternalError, "INTERNAL_ERROR", ErrorCategory::Internal},
    {ErrorCode::StoreReadOnly, "STORE_READ_ONLY", ErrorCategory::Lifecycle},
    {ErrorCode::NotInitialized, "NOT_INITIALIZED", ErrorCategory::Lifecycle},
    {ErrorCode::InvalidState, "INVALID_STATE", ErrorCategory::Lifecycle},
    {ErrorCode::LockUnavailable, "LOCK_UNAVAILABLE", ErrorCategory::Lifecycle},
    {ErrorCode::PathInvalid, "PATH_INVALID", ErrorCategory::Argument},
};

constexpr std::string_view kUnknownCodeName = "UNKNOWN_ERROR_CODE";

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return kUnknownCodeName;
}

ErrorCategory error_category(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.category;
    }
  }
  return ErrorCategory::Internal;
}

std::string_view error_category_name(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok:
      return "ok";
    case ErrorCategory::Argument:
      return "argument";
    case ErrorCategory::Structure:
      return "structure";
    case ErrorCategory::Authority:
      return "authority";
    case ErrorCategory::Persistence:
      return "persistence";
    case ErrorCategory::Lifecycle:
      return "lifecycle";
    case ErrorCategory::Limit:
      return "limit";
    case ErrorCategory::Cancelled:
      return "cancelled";
    case ErrorCategory::Internal:
      return "internal";
  }
  return "internal";
}

std::string Error::to_string() const {
  std::string out;
  out.reserve(message_.size() + subject_.size() + 24);
  out.append(error_code_name(code_));
  out.append(": ");
  out.append(message_);
  if (!subject_.empty()) {
    out.append(" [subject=");
    out.append(subject_);
    out.append("]");
  }
  return out;
}

}  // namespace dccp::facility_topology
