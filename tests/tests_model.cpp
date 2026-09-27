// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstdint>
#include <string>
#include <vector>

#include "dccp/facility_topology/clock.hpp"
#include "dccp/facility_topology/model.hpp"
#include "dccp/facility_topology/version.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

FT_TEST(model, node_kind_tokens_round_trip) {
  const std::vector<NodeKind> kinds = {NodeKind::Facility, NodeKind::Building, NodeKind::Hall, NodeKind::Room,
                                       NodeKind::Row,      NodeKind::Rack,     NodeKind::Zone};
  for (const NodeKind kind : kinds) {
    const std::string_view token = node_kind_token(kind);
    FT_CHECK(!token.empty());
    FT_CHECK(token != std::string_view("unknown"));
    auto parsed = node_kind_parse(token);
    FT_REQUIRE(parsed.has_value());
    FT_CHECK_EQ(static_cast<int>(*parsed), static_cast<int>(kind));
    FT_CHECK(node_kind_index(kind) < kNodeKindCount);
  }
  FT_CHECK_ERROR(node_kind_parse("Facility"), ErrorCode::UnknownEnumToken);
  FT_CHECK_ERROR(node_kind_parse(""), ErrorCode::UnknownEnumToken);
  FT_CHECK_ERROR(node_kind_parse("datacenter"), ErrorCode::UnknownEnumToken);
}

FT_TEST(model, other_enum_tokens_round_trip) {
  FT_CHECK_EQ(boundary_kind_token(BoundaryKind::Physical), std::string_view("physical"));
  FT_CHECK_EQ(boundary_kind_token(BoundaryKind::Logical), std::string_view("logical"));
  FT_CHECK(boundary_kind_parse("physical").value() == BoundaryKind::Physical);
  FT_CHECK(boundary_kind_parse("logical").value() == BoundaryKind::Logical);
  FT_CHECK_ERROR(boundary_kind_parse("structural"), ErrorCode::UnknownEnumToken);

  FT_CHECK(adjacency_kind_parse(adjacency_kind_token(AdjacencyKind::SharedBoundary)).value() ==
           AdjacencyKind::SharedBoundary);
  FT_CHECK(adjacency_kind_parse(adjacency_kind_token(AdjacencyKind::ServiceAisle)).value() ==
           AdjacencyKind::ServiceAisle);
  FT_CHECK(adjacency_kind_parse(adjacency_kind_token(AdjacencyKind::StructuralNeighbor)).value() ==
           AdjacencyKind::StructuralNeighbor);
  FT_CHECK_ERROR(adjacency_kind_parse("adjacent"), ErrorCode::UnknownEnumToken);

  FT_CHECK(domain_kind_parse("power").value() == DomainKind::Power);
  FT_CHECK(domain_kind_parse("cooling").value() == DomainKind::Cooling);
  FT_CHECK_ERROR(domain_kind_parse("network"), ErrorCode::UnknownEnumToken);
}

FT_TEST(model, schema_tables_encode_the_containment_rules) {
  FT_CHECK(node_kind_schema(NodeKind::Facility).root_eligible);
  FT_CHECK_EQ(static_cast<int>(node_kind_schema(NodeKind::Facility).depth), 0);
  for (const NodeKind kind : {NodeKind::Building, NodeKind::Hall, NodeKind::Room, NodeKind::Row, NodeKind::Rack,
                              NodeKind::Zone}) {
    FT_CHECK(!node_kind_schema(kind).root_eligible);
  }
  FT_CHECK(!node_kind_schema(NodeKind::Zone).zonable);
  FT_CHECK(node_kind_schema(NodeKind::Rack).zonable);
  FT_CHECK(node_kind_schema(NodeKind::Row).zonable);
  FT_CHECK(node_kind_schema(NodeKind::Room).zonable);
  FT_CHECK(node_kind_schema(NodeKind::Hall).zonable);
  FT_CHECK(!node_kind_schema(NodeKind::Building).zonable);
  FT_CHECK(!node_kind_schema(NodeKind::Facility).zonable);
  FT_CHECK_EQ(static_cast<int>(node_kind_schema(NodeKind::Rack).allowed_child_kinds), 0);

  const std::vector<NodeKind> kinds = {NodeKind::Facility, NodeKind::Building, NodeKind::Hall, NodeKind::Room,
                                       NodeKind::Row,      NodeKind::Rack,     NodeKind::Zone};
  for (const NodeKind parent : kinds) {
    for (const NodeKind child : kinds) {
      const bool allowed = schema_allows_physical_child(parent, child);
      if (allowed) {
        FT_CHECK(!node_kind_schema(child).root_eligible);
        FT_CHECK((node_kind_schema(parent).allowed_child_kinds & node_kind_bit(child)) != 0);
        FT_CHECK((node_kind_schema(child).allowed_parent_kinds & node_kind_bit(parent)) != 0);
      }
    }
  }

  // The documented relationships.
  FT_CHECK(schema_allows_physical_child(NodeKind::Facility, NodeKind::Building));
  FT_CHECK(schema_allows_physical_child(NodeKind::Facility, NodeKind::Rack));
  FT_CHECK(schema_allows_physical_child(NodeKind::Building, NodeKind::Hall));
  FT_CHECK(schema_allows_physical_child(NodeKind::Hall, NodeKind::Room));
  FT_CHECK(schema_allows_physical_child(NodeKind::Room, NodeKind::Row));
  FT_CHECK(schema_allows_physical_child(NodeKind::Row, NodeKind::Rack));
  FT_CHECK(schema_allows_physical_child(NodeKind::Facility, NodeKind::Zone));
  FT_CHECK(!schema_allows_physical_child(NodeKind::Rack, NodeKind::Room));
  FT_CHECK(!schema_allows_physical_child(NodeKind::Row, NodeKind::Hall));
  FT_CHECK(!schema_allows_physical_child(NodeKind::Zone, NodeKind::Rack));
  FT_CHECK(!schema_allows_physical_child(NodeKind::Building, NodeKind::Building));
  FT_CHECK(!schema_allows_physical_child(NodeKind::Facility, NodeKind::Facility));
  FT_CHECK(!schema_allows_physical_child(NodeKind::Room, NodeKind::Zone));
}

FT_TEST(model, adjacency_rules_exclude_facility_and_zone) {
  FT_CHECK(schema_allows_adjacency(NodeKind::Rack, NodeKind::Rack));
  FT_CHECK(schema_allows_adjacency(NodeKind::Row, NodeKind::Row));
  FT_CHECK(schema_allows_adjacency(NodeKind::Hall, NodeKind::Room));
  FT_CHECK(!schema_allows_adjacency(NodeKind::Facility, NodeKind::Hall));
  FT_CHECK(!schema_allows_adjacency(NodeKind::Zone, NodeKind::Rack));
  FT_CHECK(!schema_allows_adjacency(NodeKind::Rack, NodeKind::Zone));
}

FT_TEST(model, canonical_timestamps) {
  FT_CHECK(is_canonical_utc_timestamp("2026-02-01T00:00:00Z"));
  FT_CHECK(is_canonical_utc_timestamp("1970-01-01T00:00:00Z"));
  FT_CHECK(is_canonical_utc_timestamp("2024-02-29T23:59:59Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2023-02-29T00:00:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-13-01T00:00:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-00-01T00:00:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-32T00:00:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-01T24:00:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-01T00:60:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-01T00:00:60Z"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-01T00:00:00"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-01T00:00:00+01:00"));
  FT_CHECK(!is_canonical_utc_timestamp("2026-01-01 00:00:00Z"));
  FT_CHECK(!is_canonical_utc_timestamp(""));
  FT_CHECK(!is_canonical_utc_timestamp("2026-1-01T00:00:00Z"));
}

FT_TEST(model, clock_format_and_parse_round_trip) {
  const std::vector<std::int64_t> values = {0,          1,          86399,      86400,      951782400,
                                            1700000000, 1770000000, 2147483647, 4102444800, 253402300799LL};
  for (const std::int64_t value : values) {
    auto formatted = format_utc(value);
    FT_REQUIRE(formatted.has_value());
    FT_CHECK(is_canonical_utc_timestamp(*formatted));
    auto parsed = parse_utc(*formatted);
    FT_REQUIRE(parsed.has_value());
    FT_CHECK_EQ(*parsed, value);
  }
  FT_CHECK_EQ(format_utc(0).value(), std::string("1970-01-01T00:00:00Z"));
  FT_CHECK_EQ(format_utc(951782400).value(), std::string("2000-02-29T00:00:00Z"));
  FT_CHECK_EQ(parse_utc("2000-02-29T00:00:00Z").value(), 951782400);
  FT_CHECK_ERROR(format_utc(-1), ErrorCode::InvalidArgument);
  FT_CHECK_ERROR(format_utc(253402300800LL), ErrorCode::InvalidArgument);
  FT_CHECK_ERROR(parse_utc("1970-01-01T00:00:01"), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(parse_utc("2026-02-30T00:00:00Z"), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(parse_utc("aaaa-bb-ccTdd:ee:ffZ"), ErrorCode::MalformedRecord);

  FixedClock clock(12345);
  FT_CHECK_EQ(clock.unix_seconds(), std::int64_t{12345});
  clock.set_unix_seconds(999);
  FT_CHECK_EQ(clock.unix_seconds(), std::int64_t{999});
  FT_CHECK(SystemClock().unix_seconds() > 1700000000);
}

FT_TEST(model, limits_validation) {
  std::string explanation;
  FT_CHECK(validate_limits(TopologyLimits{}, explanation));

  TopologyLimits broken = TopologyLimits{};
  broken.max_nodes = 0;
  FT_CHECK(!validate_limits(broken, explanation));
  FT_CHECK(!explanation.empty());

  broken = TopologyLimits{};
  broken.retained_generations = 0;
  FT_CHECK(!validate_limits(broken, explanation));

  broken = TopologyLimits{};
  broken.idempotency_window = 0;
  FT_CHECK(!validate_limits(broken, explanation));

  broken = TopologyLimits{};
  broken.max_nodes = 100;
  broken.max_containment_edges = 10;
  FT_CHECK(!validate_limits(broken, explanation));

  broken = TopologyLimits{};
  broken.max_line_bytes = 10;
  FT_CHECK(!validate_limits(broken, explanation));

  broken = TopologyLimits{};
  broken.max_traversal_depth = 0;
  FT_CHECK(!validate_limits(broken, explanation));

  broken = TopologyLimits{};
  broken.max_label_bytes = 0;
  FT_CHECK(!validate_limits(broken, explanation));
}

FT_TEST(model, field_validation_helpers) {
  const TopologyLimits limits;
  FT_CHECK(validate_label("ok", 16, "label").has_value());
  FT_CHECK_ERROR(validate_label(std::string(17, 'a'), 16, "label"), ErrorCode::TextTooLong);
  FT_CHECK_ERROR(validate_label("bad\n", 16, "label"), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(validate_label("\xFF", 16, "label"), ErrorCode::InvalidUtf8);

  FT_CHECK(validate_source("cli", limits).has_value());
  FT_CHECK_ERROR(validate_source("", limits), ErrorCode::MissingField);
  FT_CHECK_ERROR(validate_source(std::string(limits.max_source_bytes + 1, 'a'), limits), ErrorCode::TextTooLong);

  FT_CHECK(validate_reason("", limits).has_value());
  FT_CHECK_ERROR(validate_reason(std::string(limits.max_reason_bytes + 1, 'a'), limits), ErrorCode::TextTooLong);

  ProvenanceRecord record;
  FT_CHECK_ERROR(validate_provenance(record, limits), ErrorCode::MissingField);
  record.actor = actor_id("tester");
  FT_CHECK_ERROR(validate_provenance(record, limits), ErrorCode::MissingField);
  record.source = "test";
  FT_CHECK(validate_provenance(record, limits).has_value());
  record.recorded_at = "not-a-timestamp";
  FT_CHECK_ERROR(validate_provenance(record, limits), ErrorCode::MalformedRecord);
  record.recorded_at = std::string(kFixtureTimestamp);
  FT_CHECK(validate_provenance(record, limits).has_value());
}

FT_TEST(model, error_codes_have_stable_names_and_categories) {
  FT_CHECK_EQ(error_code_name(ErrorCode::Ok), std::string_view("OK"));
  FT_CHECK_EQ(error_code_name(ErrorCode::ContainmentCycle), std::string_view("CONTAINMENT_CYCLE"));
  FT_CHECK_EQ(error_code_name(ErrorCode::StaleBaseGeneration), std::string_view("STALE_BASE_GENERATION"));
  FT_CHECK_EQ(error_code_name(ErrorCode::StoreReadOnly), std::string_view("STORE_READ_ONLY"));
  FT_CHECK_EQ(error_code_name(static_cast<ErrorCode>(60000)), std::string_view("UNKNOWN_ERROR_CODE"));
  FT_CHECK(error_category(ErrorCode::StaleAuthorityEpoch) == ErrorCategory::Authority);
  FT_CHECK(error_category(ErrorCode::LimitExceeded) == ErrorCategory::Limit);
  FT_CHECK(error_category(ErrorCode::Cancelled) == ErrorCategory::Cancelled);
  FT_CHECK(error_category(static_cast<ErrorCode>(60000)) == ErrorCategory::Internal);
  FT_CHECK_EQ(error_category_name(ErrorCategory::Persistence), std::string_view("persistence"));

  const Error error = Error(ErrorCode::NotFound, "missing").with_subject("rack-01");
  FT_CHECK_EQ(error.to_string(), std::string("NOT_FOUND: missing [subject=rack-01]"));
  FT_CHECK(!error.ok());
  FT_CHECK(Error().ok());
}

FT_TEST(model, compiled_version_matches_the_build_configuration) {
  FT_CHECK_EQ(std::string(kLibraryVersion), std::string(FACILITY_TOPOLOGY_CMAKE_VERSION));
  FT_CHECK_EQ(FACILITY_TOPOLOGY_VERSION_MAJOR, 1);
  FT_CHECK_EQ(FACILITY_TOPOLOGY_VERSION_MINOR, 0);
  FT_CHECK_EQ(FACILITY_TOPOLOGY_VERSION_PATCH, 0);
  FT_CHECK_EQ(kCanonicalSchemaVersion, 1U);
  FT_CHECK_EQ(std::string(kCanonicalBanner), std::string("ftop/1"));
}

FT_TEST(model, generations_and_epochs_do_not_wrap) {
  FT_CHECK(!TopologyGeneration().published());
  FT_CHECK_EQ(TopologyGeneration(1).value(), std::uint64_t{1});
  FT_CHECK_EQ(TopologyGeneration(7).next().value().value(), std::uint64_t{8});
  FT_CHECK_ERROR(TopologyGeneration(UINT64_MAX).next(), ErrorCode::GenerationOverflow);
  FT_CHECK(!WriterEpoch().valid());
  FT_CHECK_EQ(WriterEpoch(3).next().value().value(), std::uint64_t{4});
  FT_CHECK_ERROR(WriterEpoch(UINT64_MAX).next(), ErrorCode::GenerationOverflow);
  FT_CHECK(TopologyGeneration(2) > TopologyGeneration(1));
  FT_CHECK(WriterEpoch(2) > WriterEpoch(1));
}
