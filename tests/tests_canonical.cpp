// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/canonical.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

GenerationManifest manifest_for(const TopologySnapshot& snapshot, bool first = true) {
  GenerationManifest manifest;
  manifest.generation = snapshot.generation();
  if (!first) {
    manifest.parent_generation = TopologyGeneration(snapshot.generation().value() - 1);
    manifest.parent_digest = digest_of("parent");
  }
  manifest.authority_epoch = WriterEpoch(3);
  manifest.actor = actor_id("tester");
  manifest.source = "test";
  manifest.reason = "canonical fixture";
  manifest.recorded_at = std::string(kFixtureTimestamp);
  manifest.mutation_id = mutation_id("m-canonical");
  manifest.retention_floor = 1;
  return manifest;
}

/// Replaces the first occurrence of `needle`. Fails the test when the needle is
/// absent, so a fixture edit cannot silently turn an assertion into a no-op.
std::string replace_first(std::string text, std::string_view needle, std::string_view replacement) {
  const std::size_t position = text.find(needle);
  if (position == std::string::npos) {
    FT_FAIL("fixture does not contain the expected text: " + std::string(needle));
    return text;
  }
  text.replace(position, needle.size(), replacement);
  return text;
}

/// Recomputes the trailing digest line so that a deliberately damaged document
/// is internally consistent. This is how a digest that merely detects
/// corruption is distinguished from structural validation.
void resign(std::string& text) {
  const std::size_t position = text.rfind("digest sha256:");
  FT_REQUIRE(position != std::string::npos);
  text.replace(position + 14, 64, digest_hex(document_digest(text)));
}

/// A body edit plus a matching digest must still be rejected.
void expect_rejected(std::string text, ErrorCode expected) {
  resign(text);
  const auto parsed = parse_generation(text, TopologyLimits{});
  if (parsed.has_value()) {
    FT_FAIL("document was accepted but should have been rejected with " +
            std::string(error_code_name(expected)));
    return;
  }
  if (parsed.error().code() != expected) {
    FT_FAIL("expected " + std::string(error_code_name(expected)) + " but got " + parsed.error().to_string());
  }
}

GenerationDocument round_trip(const TopologySnapshot& snapshot, const GenerationManifest& manifest) {
  auto text = serialize_generation(snapshot, manifest);
  FT_REQUIRE(text.has_value());
  auto parsed = parse_generation(*text, TopologyLimits{});
  FT_REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

std::string fixture_text(const TopologySnapshot& snapshot, const GenerationManifest& manifest) {
  auto text = serialize_generation(snapshot, manifest);
  FT_REQUIRE(text.has_value());
  return std::move(*text);
}

}  // namespace

FT_TEST(canonical, round_trip_preserves_structure_and_provenance) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  GenerationDocument document = round_trip(snapshot, manifest);

  FT_CHECK(document.snapshot.same_structure_as(snapshot));
  FT_CHECK(document.manifest == manifest);
  FT_CHECK_EQ(document.snapshot.generation().value(), std::uint64_t{1});
  FT_CHECK_EQ(document.snapshot.stats().nodes, snapshot.stats().nodes);
  FT_CHECK_EQ(document.snapshot.stats().adjacency, snapshot.stats().adjacency);
  FT_CHECK(document.snapshot.content_digest() == snapshot.content_digest());

  // Re-serializing the parsed document must be byte-identical.
  auto again = serialize_generation(document.snapshot, document.manifest);
  FT_REQUIRE(again.has_value());
  FT_CHECK_EQ(*again, fixture_text(snapshot, manifest));
}

FT_TEST(canonical, serialization_is_byte_deterministic) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string first = fixture_text(snapshot, manifest);

  // Rebuilding the same topology from its records in the opposite insertion
  // order produces the identical document.
  std::vector<NodeRecord> nodes = snapshot.nodes();
  std::reverse(nodes.begin(), nodes.end());
  std::vector<ContainmentEdge> containment = snapshot.containment_edges();
  std::reverse(containment.begin(), containment.end());
  std::vector<AdjacencyEdge> adjacency = snapshot.adjacency_edges();
  std::reverse(adjacency.begin(), adjacency.end());
  std::vector<DomainDeclaration> domains = snapshot.domains();
  std::reverse(domains.begin(), domains.end());
  std::vector<DomainAssociation> associations = snapshot.associations();
  std::reverse(associations.begin(), associations.end());

  auto rebuilt = assemble_topology(nodes, containment, adjacency, domains, associations, TopologyGeneration(1));
  FT_REQUIRE(rebuilt.has_value());
  FT_CHECK_EQ(fixture_text(*rebuilt, manifest), first);
}

FT_TEST(canonical, document_shape_and_digest_line) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string text = fixture_text(snapshot, manifest);

  FT_CHECK_EQ(text.substr(0, 7), std::string("ftop/1\n"));
  FT_CHECK_EQ(text.back(), '\n');
  FT_CHECK(text.find("generation 1\n") != std::string::npos);
  FT_CHECK(text.find("nodes 11\n") != std::string::npos);
  FT_CHECK(text.find("containment 12\n") != std::string::npos);
  FT_CHECK(text.find("adjacency 3\n") != std::string::npos);
  FT_CHECK(text.find("domains 2\n") != std::string::npos);
  FT_CHECK(text.find("associations 3\n") != std::string::npos);
  FT_CHECK(text.find('\r') == std::string::npos);
  FT_CHECK_EQ(std::count(text.begin(), text.end(), '\n') > 0, true);

  const std::size_t digest_position = text.rfind("digest sha256:");
  FT_REQUIRE(digest_position != std::string::npos);
  FT_CHECK_EQ(text.substr(digest_position).size(), std::size_t{7 + 7 + 64 + 1});
  FT_CHECK(!digest_is_zero(document_digest(text)));
  FT_CHECK(digest_is_zero(document_digest("garbage")));
  FT_CHECK(digest_is_zero(document_digest("")));
}

FT_TEST(canonical, corruption_is_detected_by_the_digest) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string text = fixture_text(snapshot, manifest);

  const std::size_t target = text.find("node rack-01");
  FT_REQUIRE(target != std::string::npos);
  std::string tampered = text;
  tampered[target + 5] = 'X';
  FT_CHECK_ERROR(parse_generation(tampered, TopologyLimits{}), ErrorCode::DigestMismatch);

  // A document cut in half is rejected: the digest cannot match, and a cut
  // that lands mid-line is reported as truncation instead.
  const auto cut = parse_generation(text.substr(0, text.size() / 2), TopologyLimits{});
  FT_CHECK(!cut.has_value());
  if (!cut.has_value()) {
    FT_CHECK(cut.error().code() == ErrorCode::DigestMismatch || cut.error().code() == ErrorCode::TruncatedInput ||
             cut.error().code() == ErrorCode::MalformedRecord);
  }
  FT_CHECK_ERROR(parse_generation(text + "\n", TopologyLimits{}), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(parse_generation(text.substr(0, text.size() - 1), TopologyLimits{}), ErrorCode::TruncatedInput);
  FT_CHECK_ERROR(parse_generation("ftop/1\n", TopologyLimits{}), ErrorCode::TruncatedInput);
  FT_CHECK_ERROR(parse_generation("ftop/1\ngeneration 1\n", TopologyLimits{}), ErrorCode::MalformedRecord);
}

FT_TEST(canonical, a_matching_digest_does_not_make_invalid_structure_valid) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string text = fixture_text(snapshot, manifest);

  // The digest detects corruption; structural validation is what decides
  // whether authoritative state may be reconstructed. A document whose digest
  // matches its content is still rejected when the content is impossible.
  expect_rejected(replace_first(text, "contain row-1 rack-01 physical", "contain row-9 rack-01 physical"),
                  ErrorCode::MissingEndpoint);
  expect_rejected(replace_first(text, "node rack-01 rack", "node rack-00 rack"), ErrorCode::MissingEndpoint);
  expect_rejected(replace_first(text, "adjacent rack-01 rack-02", "adjacent rack-01 zone-a"),
                  ErrorCode::InvalidArgument);
  expect_rejected(replace_first(text, "associate rack-01 pwr-a", "associate rack-01 pwr-z"), ErrorCode::NotFound);
  expect_rejected(replace_first(text, "actor tester", "actor tester\r"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "node rack-01 rack", "node \"rack-01\" rack"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "adjacent rack-01 rack-02 structural-neighbor",
                                "adjacent rack-02 rack-01 structural-neighbor"),
                  ErrorCode::MalformedRecord);
}

FT_TEST(canonical, malformed_documents_are_rejected_with_stable_codes) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string text = fixture_text(snapshot, manifest);

  FT_CHECK_ERROR(parse_generation("", TopologyLimits{}), ErrorCode::TruncatedInput);
  FT_CHECK_ERROR(parse_generation("ftop/2\n", TopologyLimits{}), ErrorCode::UnsupportedSchemaVersion);
  FT_CHECK_ERROR(parse_generation("ftop/1", TopologyLimits{}), ErrorCode::TruncatedInput);
  FT_CHECK_ERROR(parse_generation("ftop/1\n", TopologyLimits{}), ErrorCode::TruncatedInput);
  FT_CHECK_ERROR(parse_generation(text + "extra\n", TopologyLimits{}), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(parse_generation(replace_first(text, "digest sha256:", "digest sha512:"), TopologyLimits{}),
                 ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(parse_generation(replace_first(text, "digest sha256:", "summary sha256:"), TopologyLimits{}),
                 ErrorCode::MalformedRecord);

  expect_rejected(replace_first(text, "generation 1\n", "generation 1"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "generation 1", "generation 0"), ErrorCode::InvalidGeneration);
  expect_rejected(replace_first(text, "generation 1", "generation 01"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "generation 1", "generation"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "parent 0", "parent 5"), ErrorCode::InvalidGeneration);
  expect_rejected(replace_first(text, "parent-digest -", "parent-digest sha256:00"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "authority-epoch 3", "authority-epoch 0"), ErrorCode::StaleAuthorityEpoch);
  expect_rejected(replace_first(text, "authority-epoch 3", "authority-epoch 3x"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "actor tester", "actor test er"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "actor tester", "actor test/er"), ErrorCode::MalformedIdentifier);
  expect_rejected(replace_first(text, "source \"test\"", "source test"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "retention-floor 1", "retention-floor 9"), ErrorCode::InvalidGeneration);
  expect_rejected(replace_first(text, "applied 0", "applied 1"), ErrorCode::CountMismatch);
  expect_rejected(replace_first(text, "applied 0", "applied -1"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "nodes 11", "nodes 12"), ErrorCode::CountMismatch);
  expect_rejected(replace_first(text, "nodes 11", "nodes -1"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "containment 12", "containment 11"), ErrorCode::CountMismatch);
  expect_rejected(replace_first(text, "node rack-01 rack", "node rack-01 RACK"), ErrorCode::UnknownEnumToken);
  expect_rejected(replace_first(text, "node rack-01 rack", "node bad/id rack"), ErrorCode::MalformedIdentifier);
  expect_rejected(replace_first(text, "node rack-01 rack \"Rack 01\"", "node rack-01 rack \"Rack 01"),
                  ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "node rack-01 rack", "node rack-01 rack \"\""), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "node rack-01 rack", "node rack-01 rack \"-"), ErrorCode::MalformedRecord);
  expect_rejected(replace_first(text, "contain row-1 rack-01 physical", "contain row-1 rack-01 sideways"),
                  ErrorCode::UnknownEnumToken);
  expect_rejected(replace_first(text, "domain pwr-a power", "domain pwr-a electricity"),
                  ErrorCode::UnknownEnumToken);
  expect_rejected(replace_first(text, "adjacent rack-01 rack-02 structural-neighbor",
                                "adjacent rack-01 rack-02 touching"),
                  ErrorCode::UnknownEnumToken);
  expect_rejected(replace_first(text, "associate rack-01 pwr-a power", "associate rack-01 pwr-a pwr"),
                  ErrorCode::UnknownEnumToken);
  expect_rejected(replace_first(text, "source \"test\"", "source test"), ErrorCode::MalformedRecord);
}

FT_TEST(canonical, ordering_and_duplicate_rules_are_enforced) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string text = fixture_text(snapshot, manifest);

  const std::size_t first_node = text.find("\nnode ");
  const std::size_t second_node = text.find("\nnode ", first_node + 1);
  const std::size_t third_node = text.find("\nnode ", second_node + 1);
  FT_REQUIRE(first_node != std::string::npos);
  FT_REQUIRE(second_node != std::string::npos);
  FT_REQUIRE(third_node != std::string::npos);

  std::string reordered = text;
  const std::string block_a = reordered.substr(first_node + 1, second_node - first_node - 1);
  const std::string block_b = reordered.substr(second_node + 1, third_node - second_node - 1);
  reordered.replace(first_node + 1, third_node - first_node - 1, block_b + block_a);

  // Without a matching digest this is a corruption finding.
  FT_CHECK_ERROR(parse_generation(reordered, TopologyLimits{}), ErrorCode::DigestMismatch);
  // With a matching digest it is an ordering violation, which is rejected
  // rather than silently reordered.
  expect_rejected(reordered, ErrorCode::MalformedRecord);

  // A duplicated record is likewise rejected, never de-duplicated.
  std::string duplicated = text;
  const std::string first_block = duplicated.substr(first_node + 1, second_node - first_node - 1);
  duplicated.insert(second_node + 1, first_block);
  duplicated = replace_first(duplicated, "nodes 11", "nodes 12");
  expect_rejected(duplicated, ErrorCode::MalformedRecord);
}

FT_TEST(canonical, oversized_inputs_are_rejected_before_parsing) {
  TopologyLimits limits = TopologyLimits{};
  limits.max_document_bytes = 4096;
  limits.max_line_bytes = 1024;
  FT_CHECK_ERROR(parse_generation(std::string(5000, 'a'), limits), ErrorCode::LimitExceeded);
  FT_CHECK_ERROR(parse_generation("ftop/1\n", limits), ErrorCode::TruncatedInput);

  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  const GenerationManifest manifest = manifest_for(snapshot);
  const std::string text = fixture_text(snapshot, manifest);

  // A long line is rejected from the line bound alone.
  TopologyLimits line_bounded = TopologyLimits{};
  std::vector<NodeRecord> wide = {make_node("fac-1", NodeKind::Facility)};
  wide.push_back(make_node("rack-1", NodeKind::Rack, std::string(200, 'l')));
  auto wide_snapshot = assemble_topology(wide, {physical("fac-1", "rack-1")}, {}, {}, {}, TopologyGeneration(1),
                                         line_bounded);
  FT_REQUIRE(wide_snapshot.has_value());
  line_bounded.max_line_bytes = 128;
  const std::string wide_text = fixture_text(*wide_snapshot, manifest_for(*wide_snapshot));
  FT_CHECK_ERROR(parse_generation(wide_text, line_bounded), ErrorCode::LimitExceeded);

  TopologyLimits narrow = TopologyLimits{};
  narrow.max_nodes = 5;
  FT_CHECK_ERROR(parse_generation(text, narrow), ErrorCode::LimitExceeded);

  TopologyLimits narrow_domains = TopologyLimits{};
  narrow_domains.max_domain_declarations = 1;
  FT_CHECK_ERROR(parse_generation(text, narrow_domains), ErrorCode::LimitExceeded);

  TopologyLimits narrow_associations = TopologyLimits{};
  narrow_associations.max_domain_associations = 1;
  FT_CHECK_ERROR(parse_generation(text, narrow_associations), ErrorCode::LimitExceeded);
}

FT_TEST(canonical, generation_lineage_is_validated) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(3));
  const GenerationManifest manifest = manifest_for(snapshot, false);
  const std::string text = fixture_text(snapshot, manifest);
  auto parsed = parse_generation(text, TopologyLimits{});
  FT_REQUIRE(parsed.has_value());
  FT_CHECK_EQ(parsed->manifest.parent_generation.value(), std::uint64_t{2});
  FT_CHECK(!digest_is_zero(parsed->manifest.parent_digest));
  FT_CHECK(parsed->snapshot.same_structure_as(snapshot));

  GenerationManifest broken = manifest;
  broken.parent_digest = Digest{};
  FT_CHECK_ERROR(serialize_generation(snapshot, broken), ErrorCode::MissingField);

  GenerationManifest wrong_parent = manifest;
  wrong_parent.parent_generation = TopologyGeneration(0);
  FT_CHECK_ERROR(serialize_generation(snapshot, wrong_parent), ErrorCode::InvalidGeneration);

  GenerationManifest mismatch = manifest;
  mismatch.generation = TopologyGeneration(9);
  FT_CHECK_ERROR(serialize_generation(snapshot, mismatch), ErrorCode::InvalidGeneration);

  GenerationManifest no_actor = manifest;
  no_actor.actor = ActorId();
  FT_CHECK_ERROR(serialize_generation(snapshot, no_actor), ErrorCode::MissingField);

  GenerationManifest no_epoch = manifest;
  no_epoch.authority_epoch = WriterEpoch(0);
  FT_CHECK_ERROR(serialize_generation(snapshot, no_epoch), ErrorCode::StaleAuthorityEpoch);

  GenerationManifest bad_floor = manifest;
  bad_floor.retention_floor = 99;
  FT_CHECK_ERROR(serialize_generation(snapshot, bad_floor), ErrorCode::InvalidGeneration);

  GenerationManifest bad_source = manifest;
  bad_source.source.clear();
  FT_CHECK_ERROR(serialize_generation(snapshot, bad_source), ErrorCode::MissingField);

  GenerationManifest bad_timestamp = manifest;
  bad_timestamp.recorded_at = "2026-13-01T00:00:00Z";
  FT_CHECK_ERROR(serialize_generation(snapshot, bad_timestamp), ErrorCode::MalformedRecord);
}

FT_TEST(canonical, applied_window_round_trips_and_is_bounded) {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(2));
  GenerationManifest manifest = manifest_for(snapshot, false);
  AppliedBatch first;
  first.mutation_id = mutation_id("m-0001");
  first.batch_digest = digest_of("batch one");
  first.generation = TopologyGeneration(1);
  AppliedBatch second;
  second.mutation_id = mutation_id("m-0002");
  second.batch_digest = digest_of("batch two");
  second.generation = TopologyGeneration(2);
  manifest.applied = {second, first};

  const std::string text = fixture_text(snapshot, manifest);
  auto parsed = parse_generation(text, TopologyLimits{});
  FT_REQUIRE(parsed.has_value());
  FT_REQUIRE(parsed->manifest.applied.size() == 2);
  FT_CHECK_EQ(parsed->manifest.applied[0].mutation_id.str(), std::string("m-0002"));
  FT_CHECK_EQ(parsed->manifest.applied[1].generation.value(), std::uint64_t{1});
  FT_CHECK(parsed->manifest.applied[0].batch_digest == second.batch_digest);

  TopologyLimits narrow = TopologyLimits{};
  narrow.idempotency_window = 1;
  auto narrow_snapshot = assemble_topology({make_node("fac-1", NodeKind::Facility)}, {}, {}, {}, {},
                                           TopologyGeneration(2), narrow);
  FT_REQUIRE(narrow_snapshot.has_value());
  FT_CHECK_ERROR(serialize_generation(*narrow_snapshot, manifest), ErrorCode::LimitExceeded);
}

FT_TEST(canonical, batch_serialization_is_deterministic) {
  const TopologySnapshot base = small_facility();
  MutationBatch batch = make_batch("m-batch", base.generation(), {});
  batch.authority_epoch = WriterEpoch(2);
  batch.mutations = {
      add("rack-04", NodeKind::Rack, "Rack 04"),
      RemoveNode{node_id("rack-04")},
      MoveNode{node_id("rack-03"), node_id("row-1"), provenance()},
      contain("row-2", "rack-04"),
      RemoveContainment{node_id("row-2"), node_id("rack-04")},
      SetNodeLabel{node_id("rack-03"), "Rack 03", provenance()},
      AddAdjacency{adjacent("rack-01", "rack-03", AdjacencyKind::ServiceAisle)},
      RemoveAdjacency{node_id("rack-01"), node_id("rack-02"), AdjacencyKind::StructuralNeighbor},
      DeclareDomain{declare_domain("pwr-b", DomainKind::Power, "Feed B")},
      RetireDomain{domain_id("pwr-b")},
      AddAssociation{associate("rack-03", "pwr-a", DomainKind::Power)},
      RemoveAssociation{node_id("rack-03"), domain_id("pwr-a")},
  };
  const std::string first = serialize_batch(batch);
  const std::string second = serialize_batch(batch);
  FT_CHECK_EQ(first, second);
  FT_CHECK_EQ(first.substr(0, 13), std::string("ftop-batch/1\n"));
  FT_CHECK_EQ(batch_digest(batch), batch_digest(batch));
  FT_CHECK(!digest_is_zero(batch_digest(batch)));

  MutationBatch different = batch;
  different.mutations.pop_back();
  FT_CHECK(!(batch_digest(different) == batch_digest(batch)));

  MutationBatch other_id = batch;
  other_id.mutation_id = mutation_id("m-other");
  FT_CHECK(!(batch_digest(other_id) == batch_digest(batch)));

  MutationBatch other_base = batch;
  other_base.base_generation = TopologyGeneration(9);
  FT_CHECK(!(batch_digest(other_base) == batch_digest(batch)));

  MutationBatch other_actor = batch;
  other_actor.actor = actor_id("someone-else");
  FT_CHECK(!(batch_digest(other_actor) == batch_digest(batch)));

  // The writer epoch is authority, not content: reopening a store must not
  // turn a legitimate retry into a different request.
  MutationBatch other_epoch = batch;
  other_epoch.authority_epoch = WriterEpoch(9);
  FT_CHECK(batch_digest(other_epoch) == batch_digest(batch));
  FT_CHECK(!(serialize_batch(other_epoch) == serialize_batch(batch)));
}
