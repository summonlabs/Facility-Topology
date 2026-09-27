// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/canonical.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "canonical_internal.hpp"
#include "graph_data.hpp"

namespace dccp::facility_topology {
namespace {

constexpr std::string_view kContentBanner = "ftop-content/1";

/// One parsed canonical field.
///
/// Whether a field was written as a quoted string is part of the syntax, not
/// decoration: identities, enum tokens, numbers and timestamps are bare while
/// human text is quoted. Keeping the distinction is what makes a document's
/// encoding unique, so parsing and re-serializing is byte-exact.
struct Field {
  std::string value;
  bool quoted = false;
};

void append_optional_token(std::string& out, const std::string& value) {
  if (value.empty()) {
    out.push_back('-');
  } else {
    out.append(value);
  }
}

void append_optional_scope(std::string& out, const std::optional<NodeKind>& scope) {
  if (scope.has_value()) {
    out.append(node_kind_token(*scope));
  } else {
    out.push_back('-');
  }
}

/// Splits one canonical line into fields. Fields are separated by exactly one
/// space; a field is `-`, a bare token without spaces or quotes, or a quoted
/// string with `\"` and `\\` escapes.
Result<std::vector<Field>> tokenize(std::string_view line, std::size_t max_field_bytes) {
  std::vector<Field> fields;
  std::size_t index = 0;
  while (index < line.size()) {
    Field field;
    if (line[index] == '"') {
      field.quoted = true;
      std::size_t cursor = index + 1;
      std::string raw;
      bool closed = false;
      while (cursor < line.size()) {
        const char current = line[cursor];
        if (current == '\\') {
          if (cursor + 1 >= line.size()) {
            return Error(ErrorCode::MalformedRecord, "quoted field ends with a truncated escape sequence");
          }
          raw.push_back(current);
          raw.push_back(line[cursor + 1]);
          cursor += 2;
          continue;
        }
        if (current == '"') {
          closed = true;
          break;
        }
        raw.push_back(current);
        ++cursor;
      }
      if (!closed) {
        return Error(ErrorCode::MalformedRecord, "quoted field is not terminated");
      }
      FT_TRY(value, text::unescape_quoted(raw, max_field_bytes));
      field.value = std::move(value);
      index = cursor + 1;
    } else {
      std::size_t cursor = line.find(' ', index);
      if (cursor == std::string_view::npos) {
        cursor = line.size();
      }
      const std::string_view token = line.substr(index, cursor - index);
      if (token.empty()) {
        return Error(ErrorCode::MalformedRecord, "empty field: fields must be separated by exactly one space");
      }
      if (token.find('"') != std::string_view::npos) {
        return Error(ErrorCode::MalformedRecord, "unquoted field contains a quote character");
      }
      field.value.assign(token);
      index = cursor;
    }
    fields.push_back(std::move(field));
    if (index < line.size()) {
      if (line[index] != ' ') {
        return Error(ErrorCode::MalformedRecord, "fields must be separated by exactly one space");
      }
      ++index;
      if (index == line.size()) {
        return Error(ErrorCode::MalformedRecord, "line ends with a field separator");
      }
    }
  }
  return fields;
}

/// A bare field: identities, enum tokens, numbers and canonical timestamps.
Result<std::string> decode_bare(const Field& field, std::string_view what) {
  if (field.quoted) {
    return Error(ErrorCode::MalformedRecord, std::string(what) + " must not be a quoted string");
  }
  return field.value;
}

/// A quoted field that is always present and never empty.
Result<std::string> decode_required_quoted(const Field& field, std::string_view what, std::size_t max_bytes) {
  if (!field.quoted) {
    return Error(ErrorCode::MalformedRecord, std::string(what) + " must be written as a quoted string");
  }
  if (field.value.empty()) {
    return Error(ErrorCode::MissingField, std::string(what) + " must not be empty");
  }
  if (field.value.size() > max_bytes) {
    return Error(ErrorCode::TextTooLong, std::string(what) + " exceeds the configured maximum length");
  }
  return field.value;
}

/// A field that is either the absent marker `-` or a non-empty quoted string.
/// The empty quoted string is rejected, so a document has exactly one encoding
/// of "absent".
Result<std::string> decode_optional_quoted(const Field& field, std::string_view what, std::size_t max_bytes) {
  if (!field.quoted) {
    if (field.value == "-") {
      return std::string();
    }
    return Error(ErrorCode::MalformedRecord,
                 std::string(what) + " must be written as \"-\" when absent or as a quoted string");
  }
  if (field.value.empty()) {
    return Error(ErrorCode::MalformedRecord, std::string(what) + " must be written as \"-\" when absent");
  }
  if (field.value.size() > max_bytes) {
    return Error(ErrorCode::TextTooLong, std::string(what) + " exceeds the configured maximum length");
  }
  return field.value;
}

/// A bare field that may be absent.
Result<std::string> decode_optional_bare(const Field& field, std::string_view what) {
  FT_TRY(value, decode_bare(field, what));
  return value == "-" ? std::string() : value;
}

Result<std::uint64_t> decode_u64(const Field& field, std::string_view what) {
  FT_TRY(text_value, decode_bare(field, what));
  const std::optional<std::uint64_t> value = text::parse_u64(text_value);
  if (!value.has_value()) {
    return Error(ErrorCode::MalformedRecord,
                 std::string(what) + " must be a canonical unsigned decimal without leading zeros")
        .with_subject(text_value.substr(0, 64));
  }
  return *value;
}

class LineCursor {
 public:
  explicit LineCursor(std::string_view text) : text_(text) {}

  bool done() const noexcept { return position_ >= text_.size(); }
  std::size_t line_number() const noexcept { return line_number_; }

  Result<std::string_view> next(std::size_t max_line_bytes) {
    if (done()) {
      return Error(ErrorCode::TruncatedInput, "document ended before all declared records were read");
    }
    const std::size_t newline = text_.find('\n', position_);
    if (newline == std::string_view::npos) {
      return Error(ErrorCode::TruncatedInput, "document line is not terminated by a newline");
    }
    const std::string_view line = text_.substr(position_, newline - position_);
    position_ = newline + 1;
    ++line_number_;
    if (line.size() > max_line_bytes) {
      return Error(ErrorCode::LimitExceeded, "document line exceeds the configured maximum length")
          .with_subject(std::to_string(line_number_));
    }
    if (line.find('\r') != std::string_view::npos) {
      return Error(ErrorCode::MalformedRecord, "canonical documents use LF line endings only")
          .with_subject(std::to_string(line_number_));
    }
    return line;
  }

 private:
  std::string_view text_;
  std::size_t position_ = 0;
  std::size_t line_number_ = 0;
};

Result<ProvenanceRecord> decode_provenance(const std::vector<Field>& fields, std::size_t offset,
                                           const TopologyLimits& limits) {
  if (fields.size() != offset + 4) {
    return Error(ErrorCode::MalformedRecord, "record does not carry the required provenance fields");
  }
  ProvenanceRecord provenance;
  FT_TRY(actor_text, decode_bare(fields[offset], "provenance actor"));
  FT_TRY(actor, ActorId::parse(actor_text));
  provenance.actor = actor;
  FT_TRY(source, decode_required_quoted(fields[offset + 1], "provenance source", limits.max_source_bytes));
  provenance.source = source;
  FT_TRY(reason, decode_optional_quoted(fields[offset + 2], "provenance reason", limits.max_reason_bytes));
  provenance.reason = reason;
  FT_TRY(recorded_at, decode_optional_bare(fields[offset + 3], "provenance timestamp"));
  if (!recorded_at.empty()) {
    if (!is_canonical_utc_timestamp(recorded_at)) {
      return Error(ErrorCode::MalformedRecord, "provenance timestamp is not canonical UTC").with_subject(recorded_at);
    }
    provenance.recorded_at = recorded_at;
  }
  return provenance;
}

bool is_record_keyword(std::string_view token) {
  return token == "node" || token == "contain" || token == "adjacent" || token == "domain" || token == "associate" ||
         token == "replay";
}

}  // namespace

// ---------------------------------------------------------------------------
// Structural encoding
// ---------------------------------------------------------------------------

Digest structure_digest(const GraphData& data) {
  Sha256 hasher;
  hasher.update(kContentBanner);
  hasher.update("\n");
  std::string line;
  for (const auto& entry : data.nodes) {
    const NodeRecord& node = entry.second;
    line.clear();
    line.append("node ");
    line.append(node.id.str());
    line.push_back(' ');
    line.append(node_kind_token(node.kind));
    line.push_back(' ');
    append_optional_scope(line, node.zone_member_kind);
    line.push_back(' ');
    append_quoted(line, node.label);
    line.push_back('\n');
    hasher.update(line);
  }
  for (const auto& entry : data.containment) {
    const ContainmentEdge& edge = entry.second;
    line.clear();
    line.append("contain ");
    line.append(edge.parent.str());
    line.push_back(' ');
    line.append(edge.child.str());
    line.push_back(' ');
    line.append(boundary_kind_token(edge.boundary));
    line.push_back('\n');
    hasher.update(line);
  }
  for (const auto& entry : data.adjacency) {
    const AdjacencyEdge& edge = entry.second;
    line.clear();
    line.append("adjacent ");
    line.append(edge.first.str());
    line.push_back(' ');
    line.append(edge.second.str());
    line.push_back(' ');
    line.append(adjacency_kind_token(edge.kind));
    line.push_back('\n');
    hasher.update(line);
  }
  for (const auto& entry : data.domains) {
    const DomainDeclaration& declaration = entry.second;
    line.clear();
    line.append("domain ");
    line.append(declaration.id.str());
    line.push_back(' ');
    line.append(domain_kind_token(declaration.kind));
    line.push_back(' ');
    append_quoted(line, declaration.label);
    line.push_back('\n');
    hasher.update(line);
  }
  for (const auto& entry : data.associations) {
    const DomainAssociation& association = entry.second;
    line.clear();
    line.append("associate ");
    line.append(association.node.str());
    line.push_back(' ');
    line.append(association.domain.str());
    line.push_back(' ');
    line.append(domain_kind_token(association.kind));
    line.push_back('\n');
    hasher.update(line);
  }
  return hasher.finish();
}

void append_optional_quoted(std::string& out, const std::string& value) {
  if (value.empty()) {
    out.push_back('-');
  } else {
    append_quoted(out, value);
  }
}

void append_quoted(std::string& out, const std::string& value) {
  out.push_back('"');
  out.append(text::escape_quoted(value));
  out.push_back('"');
}

void append_provenance_suffix(std::string& out, const ProvenanceRecord& provenance) {
  out.push_back(' ');
  out.append(provenance.actor.str());
  out.push_back(' ');
  append_quoted(out, provenance.source);
  out.push_back(' ');
  append_optional_quoted(out, provenance.reason);
  out.push_back(' ');
  append_optional_token(out, provenance.recorded_at);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

Result<std::string> serialize_generation(const TopologySnapshot& snapshot, const GenerationManifest& manifest) {
  const GraphData* data = SnapshotAccess::data(snapshot);
  if (data == nullptr) {
    return Error(ErrorCode::NotInitialized, "cannot serialize a snapshot that holds no topology");
  }
  if (data->generation != manifest.generation) {
    return Error(ErrorCode::InvalidGeneration, "snapshot generation and manifest generation disagree");
  }
  if (!manifest.generation.published()) {
    return Error(ErrorCode::InvalidGeneration, "a published generation number starts at 1");
  }
  if (manifest.actor.empty()) {
    return Error(ErrorCode::MissingField, "manifest actor must not be empty");
  }
  FT_TRYV(validate_source(manifest.source, data->limits));
  FT_TRYV(validate_reason(manifest.reason, data->limits));
  if (!manifest.recorded_at.empty() && !is_canonical_utc_timestamp(manifest.recorded_at)) {
    return Error(ErrorCode::MalformedRecord, "manifest timestamp is not canonical UTC")
        .with_subject(manifest.recorded_at);
  }
  if (!manifest.authority_epoch.valid()) {
    return Error(ErrorCode::StaleAuthorityEpoch, "a published generation records a non-zero writer epoch");
  }
  const std::uint64_t generation = manifest.generation.value();
  if (generation == TopologyGeneration::kFirstPublished) {
    if (manifest.parent_generation.value() != 0 || !digest_is_zero(manifest.parent_digest)) {
      return Error(ErrorCode::InvalidGeneration, "the first generation has no parent");
    }
  } else {
    if (manifest.parent_generation.value() + 1 != generation) {
      return Error(ErrorCode::InvalidGeneration, "a generation's parent is exactly the previous generation");
    }
    if (digest_is_zero(manifest.parent_digest)) {
      return Error(ErrorCode::MissingField, "a derived generation records its parent's document digest");
    }
  }
  if (manifest.retention_floor > generation) {
    return Error(ErrorCode::InvalidGeneration, "the retention floor cannot be newer than the generation");
  }
  if (manifest.applied.size() > data->limits.idempotency_window) {
    return Error(ErrorCode::LimitExceeded, "the applied-batch window exceeds the configured maximum");
  }

  std::string body;
  body.reserve(256 + data->nodes.size() * 96);
  const auto field = [&body](std::string_view keyword, std::uint64_t value) {
    body.append(keyword);
    body.push_back(' ');
    body.append(text::format_u64(value));
    body.push_back('\n');
  };
  field("generation", generation);
  field("parent", manifest.parent_generation.value());
  body.append("parent-digest ");
  if (digest_is_zero(manifest.parent_digest)) {
    body.push_back('-');
  } else {
    body.append(digest_tagged_hex(manifest.parent_digest));
  }
  body.push_back('\n');
  field("authority-epoch", manifest.authority_epoch.value());
  body.append("actor ");
  body.append(manifest.actor.str());
  body.push_back('\n');
  body.append("source ");
  append_quoted(body, manifest.source);
  body.push_back('\n');
  body.append("reason ");
  append_optional_quoted(body, manifest.reason);
  body.push_back('\n');
  body.append("recorded-at ");
  append_optional_token(body, manifest.recorded_at);
  body.push_back('\n');
  body.append("mutation-id ");
  append_optional_token(body, manifest.mutation_id.str());
  body.push_back('\n');
  field("retention-floor", manifest.retention_floor);
  field("applied", manifest.applied.size());
  for (const AppliedBatch& applied : manifest.applied) {
    body.append("replay ");
    body.append(applied.mutation_id.str());
    body.push_back(' ');
    body.append(digest_tagged_hex(applied.batch_digest));
    body.push_back(' ');
    body.append(text::format_u64(applied.generation.value()));
    body.push_back('\n');
  }

  field("nodes", data->nodes.size());
  for (const auto& entry : data->nodes) {
    const NodeRecord& node = entry.second;
    body.append("node ");
    body.append(node.id.str());
    body.push_back(' ');
    body.append(node_kind_token(node.kind));
    body.push_back(' ');
    append_optional_quoted(body, node.label);
    body.push_back(' ');
    append_optional_scope(body, node.zone_member_kind);
    append_provenance_suffix(body, node.provenance);
    body.push_back('\n');
  }

  field("containment", data->containment.size());
  for (const auto& entry : data->containment) {
    const ContainmentEdge& edge = entry.second;
    body.append("contain ");
    body.append(edge.parent.str());
    body.push_back(' ');
    body.append(edge.child.str());
    body.push_back(' ');
    body.append(boundary_kind_token(edge.boundary));
    append_provenance_suffix(body, edge.provenance);
    body.push_back('\n');
  }

  field("adjacency", data->adjacency.size());
  for (const auto& entry : data->adjacency) {
    const AdjacencyEdge& edge = entry.second;
    body.append("adjacent ");
    body.append(edge.first.str());
    body.push_back(' ');
    body.append(edge.second.str());
    body.push_back(' ');
    body.append(adjacency_kind_token(edge.kind));
    append_provenance_suffix(body, edge.provenance);
    body.push_back('\n');
  }

  field("domains", data->domains.size());
  for (const auto& entry : data->domains) {
    const DomainDeclaration& declaration = entry.second;
    body.append("domain ");
    body.append(declaration.id.str());
    body.push_back(' ');
    body.append(domain_kind_token(declaration.kind));
    body.push_back(' ');
    append_optional_quoted(body, declaration.label);
    append_provenance_suffix(body, declaration.provenance);
    body.push_back('\n');
  }

  field("associations", data->associations.size());
  for (const auto& entry : data->associations) {
    const DomainAssociation& association = entry.second;
    body.append("associate ");
    body.append(association.node.str());
    body.push_back(' ');
    body.append(association.domain.str());
    body.push_back(' ');
    body.append(domain_kind_token(association.kind));
    append_provenance_suffix(body, association.provenance);
    body.push_back('\n');
  }

  std::string out;
  out.reserve(body.size() + 96);
  out.append(kCanonicalBanner);
  out.push_back('\n');
  out.append(body);
  out.append("digest ");
  out.append(digest_tagged_hex(digest_of(body)));
  out.push_back('\n');
  return out;
}

Digest document_digest(std::string_view canonical_text) {
  if (canonical_text.empty() || canonical_text.back() != '\n') {
    return Digest{};
  }
  const std::size_t first_newline = canonical_text.find('\n');
  if (first_newline == std::string_view::npos) {
    return Digest{};
  }
  const std::size_t previous_newline = canonical_text.rfind('\n', canonical_text.size() - 2);
  const std::size_t digest_line_start = (previous_newline == std::string_view::npos) ? 0 : previous_newline + 1;
  if (digest_line_start <= first_newline) {
    return Digest{};
  }
  const std::string_view body = canonical_text.substr(first_newline + 1, digest_line_start - first_newline - 1);
  return digest_of(body);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

Result<GenerationDocument> parse_generation(std::string_view text, const TopologyLimits& limits) {
  std::string limits_explanation;
  if (!validate_limits(limits, limits_explanation)) {
    return Error(ErrorCode::InvalidArgument, "invalid topology limits: " + limits_explanation);
  }
  if (text.size() > limits.max_document_bytes) {
    return Error(ErrorCode::LimitExceeded, "document exceeds the configured maximum size")
        .with_subject(std::to_string(text.size()));
  }
  if (text.empty() || text.back() != '\n') {
    return Error(ErrorCode::TruncatedInput, "document is empty or does not end with a newline");
  }

  const std::size_t first_newline = text.find('\n');
  if (first_newline == std::string_view::npos) {
    return Error(ErrorCode::TruncatedInput, "document has no line terminator");
  }
  if (text.substr(0, first_newline) != kCanonicalBanner) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "unsupported canonical document banner")
        .with_subject(std::string(text.substr(0, std::min<std::size_t>(first_newline, 32))));
  }

  const std::size_t last_line_start = text.rfind('\n', text.size() - 2) + 1;
  if (last_line_start <= first_newline) {
    return Error(ErrorCode::TruncatedInput, "document has no digest line");
  }
  const std::string_view digest_line = text.substr(last_line_start, text.size() - 1 - last_line_start);
  const std::string_view body = text.substr(first_newline + 1, last_line_start - first_newline - 1);
  if (digest_line.size() < 7 || digest_line.substr(0, 7) != "digest ") {
    return Error(ErrorCode::MalformedRecord, "document must end with \"digest sha256:<hex>\"");
  }
  FT_TRY(declared_digest, digest_parse_tagged(digest_line.substr(7)));
  if (!digest_equal(declared_digest, digest_of(body))) {
    return Error(ErrorCode::DigestMismatch, "document digest does not match its content");
  }

  LineCursor cursor(body);
  GenerationManifest manifest;
  TopologyBuilder builder(limits);

  const auto fail = [](ErrorCode code, std::string message) { return Error(code, std::move(message)); };
  const auto read_fields = [&cursor, &limits]() -> Result<std::vector<Field>> {
    FT_TRY(line, cursor.next(limits.max_line_bytes));
    return tokenize(line, limits.max_line_bytes);
  };
  const auto expect_keyword = [&fail](const std::vector<Field>& fields, std::string_view keyword) -> Result<void> {
    if (fields.size() != 2 || fields[0].quoted || fields[0].value != keyword) {
      return fail(ErrorCode::MalformedRecord,
                  "expected a line of the form \"" + std::string(keyword) + " <value>\"");
    }
    return ok();
  };

  // -- header ---------------------------------------------------------------
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "generation"));
    FT_TRY(value, decode_u64(fields[1], "generation"));
    if (value == 0 || value == UINT64_MAX) {
      return fail(ErrorCode::InvalidGeneration, "a published generation number starts at 1");
    }
    manifest.generation = TopologyGeneration(value);
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "parent"));
    FT_TRY(value, decode_u64(fields[1], "parent generation"));
    manifest.parent_generation = TopologyGeneration(value);
    if (manifest.generation.value() == TopologyGeneration::kFirstPublished) {
      if (value != 0) {
        return fail(ErrorCode::InvalidGeneration, "the first generation has no parent");
      }
    } else if (value + 1 != manifest.generation.value()) {
      return fail(ErrorCode::InvalidGeneration, "a generation's parent is exactly the previous generation");
    }
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "parent-digest"));
    FT_TRY(token, decode_bare(fields[1], "parent digest"));
    if (token != "-") {
      FT_TRY(digest, digest_parse_tagged(token));
      manifest.parent_digest = digest;
    }
    if (manifest.generation.value() == TopologyGeneration::kFirstPublished) {
      if (!digest_is_zero(manifest.parent_digest)) {
        return fail(ErrorCode::InvalidGeneration, "the first generation has no parent digest");
      }
    } else if (digest_is_zero(manifest.parent_digest)) {
      return fail(ErrorCode::MissingField, "a derived generation records its parent's document digest");
    }
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "authority-epoch"));
    FT_TRY(value, decode_u64(fields[1], "authority epoch"));
    if (value == 0) {
      return fail(ErrorCode::StaleAuthorityEpoch, "a published generation records a non-zero writer epoch");
    }
    manifest.authority_epoch = WriterEpoch(value);
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "actor"));
    FT_TRY(actor_text, decode_bare(fields[1], "manifest actor"));
    FT_TRY(actor, ActorId::parse(actor_text));
    manifest.actor = actor;
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "source"));
    FT_TRY(source, decode_required_quoted(fields[1], "manifest source", limits.max_source_bytes));
    manifest.source = source;
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "reason"));
    FT_TRY(reason, decode_optional_quoted(fields[1], "manifest reason", limits.max_reason_bytes));
    manifest.reason = reason;
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "recorded-at"));
    FT_TRY(recorded_at, decode_optional_bare(fields[1], "manifest timestamp"));
    if (!recorded_at.empty()) {
      if (!is_canonical_utc_timestamp(recorded_at)) {
        return Error(ErrorCode::MalformedRecord, "manifest timestamp is not canonical UTC").with_subject(recorded_at);
      }
      manifest.recorded_at = recorded_at;
    }
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "mutation-id"));
    FT_TRY(id_text, decode_optional_bare(fields[1], "mutation identity"));
    if (!id_text.empty()) {
      FT_TRY(id, MutationId::parse(id_text));
      manifest.mutation_id = id;
    }
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "retention-floor"));
    FT_TRY(value, decode_u64(fields[1], "retention floor"));
    if (value > manifest.generation.value()) {
      return fail(ErrorCode::InvalidGeneration, "the retention floor cannot be newer than the generation");
    }
    if (value > std::numeric_limits<std::size_t>::max()) {
      return fail(ErrorCode::LimitExceeded, "the retention floor is not representable on this platform");
    }
    manifest.retention_floor = static_cast<std::size_t>(value);
  }
  {
    FT_TRY(fields, read_fields());
    FT_TRYV(expect_keyword(fields, "applied"));
    FT_TRY(count, decode_u64(fields[1], "applied count"));
    if (count > limits.idempotency_window) {
      return fail(ErrorCode::LimitExceeded, "the applied-batch window exceeds the configured maximum");
    }
    manifest.applied.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
      FT_TRY(entry_fields, read_fields());
      if (entry_fields.size() != 4 || entry_fields[0].quoted || entry_fields[0].value != "replay") {
        return fail(ErrorCode::CountMismatch, "the applied-batch window is shorter than declared");
      }
      AppliedBatch applied;
      FT_TRY(id_text, decode_bare(entry_fields[1], "applied mutation identity"));
      FT_TRY(id, MutationId::parse(id_text));
      applied.mutation_id = id;
      FT_TRY(digest_token, decode_bare(entry_fields[2], "applied batch digest"));
      FT_TRY(digest, digest_parse_tagged(digest_token));
      applied.batch_digest = digest;
      FT_TRY(generation, decode_u64(entry_fields[3], "applied generation"));
      if (generation == 0) {
        return fail(ErrorCode::InvalidGeneration, "an applied batch records a published generation");
      }
      applied.generation = TopologyGeneration(generation);
      manifest.applied.push_back(std::move(applied));
    }
  }

  // -- sections -------------------------------------------------------------
  struct SectionSpec {
    std::string_view count_keyword;
    std::string_view record_keyword;
    std::size_t limit;
  };
  const std::vector<SectionSpec> sections = {
      {"nodes", "node", limits.max_nodes},
      {"containment", "contain", limits.max_containment_edges},
      {"adjacency", "adjacent", limits.max_adjacency_edges},
      {"domains", "domain", limits.max_domain_declarations},
      {"associations", "associate", limits.max_domain_associations},
  };

  for (const SectionSpec& section : sections) {
    FT_TRY(count_fields, read_fields());
    if (count_fields.size() != 2 || count_fields[0].quoted || count_fields[0].value != section.count_keyword) {
      if (!count_fields.empty() && !count_fields[0].quoted && is_record_keyword(count_fields[0].value)) {
        return Error(ErrorCode::CountMismatch,
                     "section \"" + std::string(section.count_keyword) + "\" has more records than it declares");
      }
      return Error(ErrorCode::MalformedRecord,
                   "expected the section header \"" + std::string(section.count_keyword) + " <count>\"");
    }
    FT_TRY(count, decode_u64(count_fields[1], "section count"));
    if (count > section.limit) {
      return Error(ErrorCode::LimitExceeded,
                   "declared " + std::string(section.count_keyword) + " count exceeds the configured maximum")
          .with_subject(count_fields[1].value);
    }

    const std::size_t declared = static_cast<std::size_t>(count);
    // Canonical order is enforced, never repaired: records must appear in
    // strictly increasing canonical order, so a document has exactly one
    // encoding and parse/serialize round trips are byte-exact.
    std::optional<NodeId> previous_node;
    std::optional<ContainmentKey> previous_containment;
    std::optional<AdjacencyKey> previous_adjacency;
    std::optional<DomainId> previous_domain;
    std::optional<AssociationKey> previous_association;

    for (std::size_t index = 0; index < declared; ++index) {
      FT_TRY(fields, read_fields());
      if (fields.empty() || fields[0].quoted || fields[0].value != section.record_keyword) {
        return Error(ErrorCode::CountMismatch,
                     "section \"" + std::string(section.count_keyword) + "\" is shorter than declared")
            .with_subject(std::to_string(index));
      }

      if (section.record_keyword == "node") {
        if (fields.size() != 9) {
          return fail(ErrorCode::MalformedRecord, "a node record has exactly nine fields");
        }
        NodeRecord node;
        FT_TRY(id_text, decode_bare(fields[1], "node identity"));
        FT_TRY(id, NodeId::parse(id_text));
        node.id = id;
        FT_TRY(kind_text, decode_bare(fields[2], "node kind"));
        FT_TRY(kind, node_kind_parse(kind_text));
        node.kind = kind;
        FT_TRY(label, decode_optional_quoted(fields[3], "node label", limits.max_label_bytes));
        node.label = label;
        FT_TRY(scope_text, decode_optional_bare(fields[4], "zone member kind"));
        if (!scope_text.empty()) {
          FT_TRY(scope, node_kind_parse(scope_text));
          node.zone_member_kind = scope;
        }
        FT_TRY(provenance, decode_provenance(fields, 5, limits));
        node.provenance = std::move(provenance);
        if (previous_node.has_value() && !(*previous_node < node.id)) {
          return Error(ErrorCode::MalformedRecord, "node records are not in canonical order")
              .with_subject(node.id.str());
        }
        previous_node = node.id;
        FT_TRYV(builder.add_node(std::move(node)));
        continue;
      }
      if (section.record_keyword == "contain") {
        if (fields.size() != 8) {
          return fail(ErrorCode::MalformedRecord, "a containment record has exactly eight fields");
        }
        ContainmentEdge edge;
        FT_TRY(parent_text, decode_bare(fields[1], "containment parent"));
        FT_TRY(parent, NodeId::parse(parent_text));
        edge.parent = parent;
        FT_TRY(child_text, decode_bare(fields[2], "containment child"));
        FT_TRY(child, NodeId::parse(child_text));
        edge.child = child;
        FT_TRY(boundary_text, decode_bare(fields[3], "containment boundary"));
        FT_TRY(boundary, boundary_kind_parse(boundary_text));
        edge.boundary = boundary;
        FT_TRY(provenance, decode_provenance(fields, 4, limits));
        edge.provenance = std::move(provenance);
        const ContainmentKey key{edge.parent, edge.child};
        if (previous_containment.has_value() && !(*previous_containment < key)) {
          return Error(ErrorCode::MalformedRecord, "containment records are not in canonical order")
              .with_subject(edge.child.str());
        }
        previous_containment = key;
        if (builder.find_node(edge.parent) == nullptr || builder.find_node(edge.child) == nullptr) {
          return Error(ErrorCode::MissingEndpoint, "containment endpoint is not declared in this generation");
        }
        FT_TRYV(builder.add_containment(std::move(edge)));
        continue;
      }
      if (section.record_keyword == "adjacent") {
        if (fields.size() != 8) {
          return fail(ErrorCode::MalformedRecord, "an adjacency record has exactly eight fields");
        }
        AdjacencyEdge edge;
        FT_TRY(first_text, decode_bare(fields[1], "adjacency endpoint"));
        FT_TRY(first, NodeId::parse(first_text));
        edge.first = first;
        FT_TRY(second_text, decode_bare(fields[2], "adjacency endpoint"));
        FT_TRY(second, NodeId::parse(second_text));
        edge.second = second;
        FT_TRY(kind_text, decode_bare(fields[3], "adjacency kind"));
        FT_TRY(kind, adjacency_kind_parse(kind_text));
        edge.kind = kind;
        FT_TRY(provenance, decode_provenance(fields, 4, limits));
        edge.provenance = std::move(provenance);
        // The symmetric relation is stored with the lesser endpoint first; a
        // record written the other way round is a second encoding of the same
        // edge and is rejected rather than normalized.
        if (!(edge.first < edge.second)) {
          return Error(ErrorCode::MalformedRecord,
                       "adjacency endpoints must be written in canonical order (lesser identity first)")
              .with_subject(edge.first.str());
        }
        const AdjacencyKey key = make_adjacency_key(edge.first, edge.second);
        if (previous_adjacency.has_value() && !(*previous_adjacency < key)) {
          return Error(ErrorCode::MalformedRecord, "adjacency records are not in canonical order")
              .with_subject(edge.first.str());
        }
        previous_adjacency = key;
        FT_TRYV(builder.add_adjacency(std::move(edge)));
        continue;
      }
      if (section.record_keyword == "domain") {
        if (fields.size() != 8) {
          return fail(ErrorCode::MalformedRecord, "a domain record has exactly eight fields");
        }
        DomainDeclaration declaration;
        FT_TRY(id_text, decode_bare(fields[1], "domain identity"));
        FT_TRY(id, DomainId::parse(id_text));
        declaration.id = id;
        FT_TRY(kind_text, decode_bare(fields[2], "domain kind"));
        FT_TRY(kind, domain_kind_parse(kind_text));
        declaration.kind = kind;
        FT_TRY(label, decode_optional_quoted(fields[3], "domain label", limits.max_label_bytes));
        declaration.label = label;
        FT_TRY(provenance, decode_provenance(fields, 4, limits));
        declaration.provenance = std::move(provenance);
        if (previous_domain.has_value() && !(*previous_domain < declaration.id)) {
          return Error(ErrorCode::MalformedRecord, "domain records are not in canonical order")
              .with_subject(declaration.id.str());
        }
        previous_domain = declaration.id;
        FT_TRYV(builder.declare_domain(std::move(declaration)));
        continue;
      }
      // "associate"
      if (fields.size() != 8) {
        return fail(ErrorCode::MalformedRecord, "a domain association record has exactly eight fields");
      }
      DomainAssociation association;
      FT_TRY(node_text, decode_bare(fields[1], "association node"));
      FT_TRY(node, NodeId::parse(node_text));
      association.node = node;
      FT_TRY(domain_text, decode_bare(fields[2], "association domain"));
      FT_TRY(domain, DomainId::parse(domain_text));
      association.domain = domain;
      FT_TRY(kind_text, decode_bare(fields[3], "association kind"));
      FT_TRY(kind, domain_kind_parse(kind_text));
      association.kind = kind;
      FT_TRY(provenance, decode_provenance(fields, 4, limits));
      association.provenance = std::move(provenance);
      const AssociationKey key{association.node, association.domain};
      if (previous_association.has_value() && !(*previous_association < key)) {
        return Error(ErrorCode::MalformedRecord, "domain association records are not in canonical order")
            .with_subject(association.node.str());
      }
      previous_association = key;
      FT_TRYV(builder.add_association(std::move(association)));
    }
  }

  if (!cursor.done()) {
    return Error(ErrorCode::MalformedRecord, "document contains trailing content after the last section")
        .with_subject(std::to_string(cursor.line_number()));
  }

  FT_TRY(snapshot, builder.build(manifest.generation));
  GenerationDocument document;
  document.manifest = std::move(manifest);
  document.snapshot = std::move(snapshot);
  return document;
}

// ---------------------------------------------------------------------------
// Batch encoding
// ---------------------------------------------------------------------------

namespace {

/// Renders a batch. The writer epoch is authority rather than content, so
/// batch_digest() hashes the rendering without it while serialize_batch()
/// renders every field.
std::string render_batch(const MutationBatch& batch, bool include_epoch) {
  std::string out;
  out.append("ftop-batch/1\n");
  const auto field = [&out](std::string_view keyword, std::uint64_t value) {
    out.append(keyword);
    out.push_back(' ');
    out.append(text::format_u64(value));
    out.push_back('\n');
  };
  field("base", batch.base_generation.value());
  if (include_epoch) {
    field("epoch", batch.authority_epoch.value());
  }
  out.append("actor ");
  out.append(batch.actor.str());
  out.push_back('\n');
  out.append("source ");
  append_quoted(out, batch.source);
  out.push_back('\n');
  out.append("reason ");
  append_optional_quoted(out, batch.reason);
  out.push_back('\n');
  out.append("recorded-at ");
  append_optional_token(out, batch.recorded_at);
  out.push_back('\n');
  out.append("mutation-id ");
  append_optional_token(out, batch.mutation_id.str());
  out.push_back('\n');
  field("mutations", batch.mutations.size());

  for (const Mutation& mutation : batch.mutations) {
    std::visit(
        [&out](const auto& command) {
          using Command = std::decay_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, AddNode>) {
            out.append("add-node ");
            out.append(command.node.id.str());
            out.push_back(' ');
            out.append(node_kind_token(command.node.kind));
            out.push_back(' ');
            append_optional_quoted(out, command.node.label);
            out.push_back(' ');
            append_optional_scope(out, command.node.zone_member_kind);
          } else if constexpr (std::is_same_v<Command, RemoveNode>) {
            out.append("remove-node ");
            out.append(command.id.str());
          } else if constexpr (std::is_same_v<Command, MoveNode>) {
            out.append("move-node ");
            out.append(command.id.str());
            out.push_back(' ');
            out.append(command.new_parent.str());
            append_provenance_suffix(out, command.provenance);
          } else if constexpr (std::is_same_v<Command, AddContainment>) {
            out.append("add-containment ");
            out.append(command.edge.parent.str());
            out.push_back(' ');
            out.append(command.edge.child.str());
            out.push_back(' ');
            out.append(boundary_kind_token(command.edge.boundary));
            append_provenance_suffix(out, command.edge.provenance);
          } else if constexpr (std::is_same_v<Command, RemoveContainment>) {
            out.append("remove-containment ");
            out.append(command.parent.str());
            out.push_back(' ');
            out.append(command.child.str());
          } else if constexpr (std::is_same_v<Command, SetNodeLabel>) {
            out.append("set-label ");
            out.append(command.id.str());
            out.push_back(' ');
            append_quoted(out, command.label);
            append_provenance_suffix(out, command.provenance);
          } else if constexpr (std::is_same_v<Command, AddAdjacency>) {
            out.append("add-adjacency ");
            out.append(command.edge.first.str());
            out.push_back(' ');
            out.append(command.edge.second.str());
            out.push_back(' ');
            out.append(adjacency_kind_token(command.edge.kind));
            append_provenance_suffix(out, command.edge.provenance);
          } else if constexpr (std::is_same_v<Command, RemoveAdjacency>) {
            out.append("remove-adjacency ");
            out.append(command.first.str());
            out.push_back(' ');
            out.append(command.second.str());
            out.push_back(' ');
            out.append(adjacency_kind_token(command.kind));
          } else if constexpr (std::is_same_v<Command, DeclareDomain>) {
            out.append("declare-domain ");
            out.append(command.declaration.id.str());
            out.push_back(' ');
            out.append(domain_kind_token(command.declaration.kind));
            out.push_back(' ');
            append_optional_quoted(out, command.declaration.label);
            append_provenance_suffix(out, command.declaration.provenance);
          } else if constexpr (std::is_same_v<Command, RetireDomain>) {
            out.append("retire-domain ");
            out.append(command.id.str());
          } else if constexpr (std::is_same_v<Command, AddAssociation>) {
            out.append("add-association ");
            out.append(command.association.node.str());
            out.push_back(' ');
            out.append(command.association.domain.str());
            out.push_back(' ');
            out.append(domain_kind_token(command.association.kind));
            append_provenance_suffix(out, command.association.provenance);
          } else {
            out.append("remove-association ");
            out.append(command.node.str());
            out.push_back(' ');
            out.append(command.domain.str());
          }
          out.push_back('\n');
        },
        mutation);
  }
  return out;
}

}  // namespace

std::string serialize_batch(const MutationBatch& batch) { return render_batch(batch, true); }

Digest batch_digest(const MutationBatch& batch) { return digest_of(render_batch(batch, false)); }

}  // namespace dccp::facility_topology
