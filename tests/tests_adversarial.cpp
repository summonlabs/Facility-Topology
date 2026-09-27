// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial input tests.
//
// Every input here is untrusted. The obligations are: never crash, never hang,
// never allocate unboundedly, and always reject with a stable code or accept a
// document that is genuinely valid.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "dccp/facility_topology/canonical.hpp"
#include "test_rng.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

GenerationManifest fixture_manifest(const TopologySnapshot& snapshot) {
  GenerationManifest manifest;
  manifest.generation = snapshot.generation();
  manifest.authority_epoch = WriterEpoch(1);
  manifest.actor = actor_id("tester");
  manifest.source = "test";
  manifest.recorded_at = std::string(kFixtureTimestamp);
  manifest.retention_floor = 1;
  return manifest;
}

std::string fixture_document() {
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  auto text = serialize_generation(snapshot, fixture_manifest(snapshot));
  FT_REQUIRE(text.has_value());
  return std::move(*text);
}

void resign(std::string& text) {
  const std::size_t position = text.rfind("digest sha256:");
  if (position == std::string::npos) {
    return;
  }
  text.replace(position + 14, 64, digest_hex(document_digest(text)));
}

}  // namespace

FT_TEST(adversarial, byte_mutations_never_crash_and_are_deterministic) {
  const std::string original = fixture_document();
  for (int iteration = 0; iteration < 300; ++iteration) {
    Rng rng(current_seed() + 7919ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("adversarial.byte_mutations_never_crash_and_are_deterministic#" +
                             std::to_string(iteration));
    std::string mutated = original;
    const std::size_t edits = 1 + rng.index(4);
    for (std::size_t edit = 0; edit < edits; ++edit) {
      switch (rng.below(4)) {
        case 0: {
          const std::size_t position = rng.index(mutated.size());
          mutated[position] = static_cast<char>(rng.below(256));
          break;
        }
        case 1: {
          const std::size_t position = rng.index(mutated.size());
          mutated.insert(position, 1, static_cast<char>(rng.below(256)));
          break;
        }
        case 2: {
          const std::size_t position = rng.index(mutated.size());
          mutated.erase(position, 1);
          break;
        }
        default: {
          const std::size_t position = rng.index(mutated.size());
          const std::string token = rng.token(1 + rng.index(8));
          mutated.insert(position, token);
          break;
        }
      }
      if (mutated.empty()) {
        mutated = "x";
      }
    }
    // Half of the cases are re-signed so that the digest check does not mask
    // the parser; the other half exercise the digest path.
    if (rng.chance(1, 2)) {
      resign(mutated);
    }

    const auto first = parse_generation(mutated, TopologyLimits{});
    const auto second = parse_generation(mutated, TopologyLimits{});
    FT_CHECK_EQ(first.has_value(), second.has_value());
    if (!first.has_value()) {
      FT_CHECK(first.error().code() == second.error().code());
      FT_CHECK(!first.error().message().empty());
    } else {
      // Anything accepted must be a genuinely valid generation.
      FT_CHECK(first->snapshot.stats().facilities >= 1);
      FT_CHECK_EQ(first->snapshot.generation().value(), first->manifest.generation.value());
      auto round_trip = serialize_generation(first->snapshot, first->manifest);
      FT_REQUIRE(round_trip.has_value());
      auto reparsed = parse_generation(*round_trip, TopologyLimits{});
      FT_CHECK(reparsed.has_value());
    }
  }
}

FT_TEST(adversarial, random_bytes_are_always_rejected) {
  for (int iteration = 0; iteration < 200; ++iteration) {
    Rng rng(current_seed() + 104729ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("adversarial.random_bytes_are_always_rejected#" + std::to_string(iteration));
    const std::string noise = rng.bytes(1 + rng.index(4096));
    const auto parsed = parse_generation(noise, TopologyLimits{});
    FT_CHECK(!parsed.has_value());
    FT_CHECK(!parsed.error().message().empty());
  }
  // Structured noise that starts like a real document.
  for (int iteration = 0; iteration < 100; ++iteration) {
    Rng rng(current_seed() + 15485863ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("adversarial.random_bytes_are_always_rejected#framed" + std::to_string(iteration));
    std::string noise = "ftop/1\n";
    noise.append(rng.bytes(1 + rng.index(512)));
    noise.push_back('\n');
    const auto parsed = parse_generation(noise, TopologyLimits{});
    FT_CHECK(!parsed.has_value());
  }
}

FT_TEST(adversarial, oversized_declared_counts_are_rejected_without_allocation) {
  const std::string original = fixture_document();
  // A document that declares four million nodes but contains none must be
  // rejected from the declared count alone.
  std::string bomb = original;
  const std::size_t position = bomb.find("nodes 11");
  FT_REQUIRE(position != std::string::npos);
  bomb.replace(position, 8, "nodes 4000000");
  resign(bomb);
  const auto parsed = parse_generation(bomb, TopologyLimits{});
  FT_CHECK(!parsed.has_value());

  // The same with an absurd count that overflows nothing but exceeds any bound.
  std::string absurd = original;
  absurd.replace(position, 8, "nodes 18446744073709551615");
  resign(absurd);
  const auto absurd_result = parse_generation(absurd, TopologyLimits{});
  FT_CHECK(!absurd_result.has_value());
  FT_CHECK_EQ(absurd_result.error().code(), ErrorCode::LimitExceeded);

  // A huge applied-batch window is bounded by the idempotency window.
  std::string window = original;
  const std::size_t applied = window.find("applied 0");
  FT_REQUIRE(applied != std::string::npos);
  window.replace(applied, 9, "applied 1000000");
  resign(window);
  const auto window_result = parse_generation(window, TopologyLimits{});
  FT_CHECK(!window_result.has_value());
  FT_CHECK_EQ(window_result.error().code(), ErrorCode::LimitExceeded);
}

FT_TEST(adversarial, invalid_utf8_and_control_bytes_in_labels_are_rejected) {
  const std::string original = fixture_document();
  const std::size_t label = original.find("\"Rack 01\"");
  FT_REQUIRE(label != std::string::npos);

  std::vector<std::string> payloads = {
      std::string("\xFF"),
      std::string("\xC3\x28"),
      std::string("\xED\xA0\x80"),
      std::string("\xE2\x82"),
      std::string("\xF4\x90\x80\x80"),
  };
  for (const std::string& payload : payloads) {
    std::string mutated = original;
    mutated.replace(label, 9, "\"" + payload + "\"");
    resign(mutated);
    const auto parsed = parse_generation(mutated, TopologyLimits{});
    FT_CHECK(!parsed.has_value());
    if (!parsed.has_value()) {
      FT_CHECK(parsed.error().code() == ErrorCode::InvalidUtf8 ||
               parsed.error().code() == ErrorCode::MalformedRecord);
    }
  }

  // Control characters cannot be smuggled through the escape machinery either.
  for (const std::string& payload : {std::string("a\nb"), std::string("a\rb"), std::string("a\tb")}) {
    std::string mutated = original;
    mutated.replace(label, 9, "\"" + payload + "\"");
    resign(mutated);
    const auto parsed = parse_generation(mutated, TopologyLimits{});
    FT_CHECK(!parsed.has_value());
  }
}

FT_TEST(adversarial, identity_overflow_and_path_like_identities_are_rejected) {
  const std::string original = fixture_document();
  const std::string needle = "node rack-01 rack";
  const std::size_t node = original.find(needle);
  FT_REQUIRE(node != std::string::npos);

  const std::vector<std::string> identities = {
      "../../etc/passwd", "C:\\Windows\\System32", "rack/01", "rack 01", "rack\n01", "-", ".",
      std::string(129, 'a'), "\"rack\"", "rack\xFF", "rack..01", "..", "rack:", ":rack",
  };
  for (const std::string& identity : identities) {
    std::string mutated = original;
    mutated.replace(node, needle.size(), "node " + identity + " rack");
    resign(mutated);
    const auto parsed = parse_generation(mutated, TopologyLimits{});
    FT_CHECK(!parsed.has_value());
    if (parsed.has_value()) {
      FT_FAIL("path-like or oversized identity was accepted: " + identity);
    }
  }
}

FT_TEST(adversarial, deep_and_wide_documents_respect_bounds) {
  // A wide generation bounded by a narrow limit is rejected from the count.
  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  auto text = serialize_generation(snapshot, fixture_manifest(snapshot));
  FT_REQUIRE(text.has_value());

  TopologyLimits narrow = TopologyLimits{};
  narrow.max_nodes = 3;
  narrow.max_containment_edges = 3;
  narrow.max_adjacency_edges = 1;
  narrow.max_domain_associations = 1;
  const auto parsed = parse_generation(*text, narrow);
  FT_CHECK_ERROR(parsed, ErrorCode::LimitExceeded);

  // The deepest legal hierarchy is exactly the schema's structural depth, and
  // traversal stops at the configured bound instead of recursing without limit.
  std::vector<NodeRecord> nodes = {
      make_node("fac-1", NodeKind::Facility), make_node("bld-1", NodeKind::Building),
      make_node("hall-1", NodeKind::Hall),    make_node("room-1", NodeKind::Room),
      make_node("row-1", NodeKind::Row),      make_node("rack-1", NodeKind::Rack),
  };
  std::vector<ContainmentEdge> chain = {physical("fac-1", "bld-1"), physical("bld-1", "hall-1"),
                                        physical("hall-1", "room-1"), physical("room-1", "row-1"),
                                        physical("row-1", "rack-1")};
  auto built = assemble_topology(nodes, chain, {}, {}, {}, TopologyGeneration(1));
  FT_REQUIRE(built.has_value());
  FT_CHECK_EQ(static_cast<int>(built->stats().max_depth), static_cast<int>(kMaxStructuralDepth));

  TraversalLimits bounds;
  bounds.max_depth = 2;
  bounds.max_nodes = 1024;
  const auto bounded = built->traverse(node_id("fac-1"), TraversalOrder::DepthFirstPreOrder, bounds);
  FT_CHECK_ERROR(bounded, ErrorCode::TraversalDepthExceeded);

  // A hierarchy deeper than the schema allows cannot be assembled at all.
  std::vector<NodeRecord> deeper = nodes;
  deeper.push_back(make_node("hall-2", NodeKind::Hall));
  std::vector<ContainmentEdge> deeper_chain = chain;
  deeper_chain.push_back(physical("rack-1", "hall-2"));
  auto rejected = assemble_topology(deeper, deeper_chain, {}, {}, {}, TopologyGeneration(1));
  FT_CHECK_ERROR(rejected, ErrorCode::InvalidParentKind);
}

FT_TEST(adversarial, truncated_documents_are_never_partially_accepted) {
  const std::string original = fixture_document();
  for (std::size_t length = 0; length < original.size(); ++length) {
    const auto parsed = parse_generation(original.substr(0, length), TopologyLimits{});
    FT_CHECK(!parsed.has_value());
  }
  // Line-level truncation with a repaired digest is rejected too.
  const std::size_t half = original.size() / 2;
  std::string cut = original.substr(0, half);
  resign(cut);
  const auto parsed = parse_generation(cut, TopologyLimits{});
  FT_CHECK(!parsed.has_value());
}

FT_TEST(adversarial, duplicate_and_reordered_records_are_rejected) {
  const std::string original = fixture_document();

  // Duplicate a containment record and adjust the declared count.
  const std::size_t first_contain = original.find("\ncontain ");
  FT_REQUIRE(first_contain != std::string::npos);
  const std::size_t second_contain = original.find("\ncontain ", first_contain + 1);
  FT_REQUIRE(second_contain != std::string::npos);
  const std::string block = original.substr(first_contain + 1, second_contain - first_contain - 1);

  std::string duplicated = original;
  duplicated.insert(second_contain + 1, block);
  const std::size_t count_position = duplicated.find("containment 12");
  FT_REQUIRE(count_position != std::string::npos);
  duplicated.replace(count_position, 14, "containment 13");
  resign(duplicated);
  const auto duplicate_result = parse_generation(duplicated, TopologyLimits{});
  FT_CHECK(!duplicate_result.has_value());
  if (!duplicate_result.has_value()) {
    FT_CHECK_EQ(duplicate_result.error().code(), ErrorCode::MalformedRecord);
  }
}

FT_TEST(adversarial, unescape_bounds_are_enforced) {
  // A quoted field that unescapes past the configured field bound is rejected.
  TopologyLimits limits = TopologyLimits{};
  limits.max_line_bytes = 200000;
  limits.max_document_bytes = 1U << 20;
  limits.max_label_bytes = 16;

  const TopologySnapshot snapshot = small_facility(TopologyGeneration(1));
  GenerationManifest manifest = fixture_manifest(snapshot);
  auto text = serialize_generation(snapshot, manifest);
  FT_REQUIRE(text.has_value());

  const std::size_t label = text->find("\"Rack 01\"");
  FT_REQUIRE(label != std::string::npos);
  std::string long_label = *text;
  long_label.replace(label, 9, "\"" + std::string(64, 'a') + "\"");
  resign(long_label);
  const auto parsed = parse_generation(long_label, limits);
  FT_CHECK_ERROR(parsed, ErrorCode::TextTooLong);

  // Escapes that unescape to the original length are accepted.
  std::string escaped_label = *text;
  escaped_label.replace(label, 9, "\"Rack \\\"01\\\"\"");
  resign(escaped_label);
  const auto escaped_result = parse_generation(escaped_label, TopologyLimits{});
  FT_REQUIRE(escaped_result.has_value());
  const NodeRecord* record = escaped_result->snapshot.find_node(node_id("rack-01"));
  FT_REQUIRE(record != nullptr);
  FT_CHECK_EQ(record->label, std::string("Rack \"01\""));
}

FT_TEST(adversarial, malformed_batches_never_mutate_the_base) {
  const TopologySnapshot base = small_facility();
  const Digest before = base.content_digest();
  for (int iteration = 0; iteration < 100; ++iteration) {
    Rng rng(current_seed() + 32452843ULL * static_cast<std::uint64_t>(iteration));
    set_current_case_context("adversarial.malformed_batches_never_mutate_the_base#" + std::to_string(iteration));
    MutationBatch batch = make_batch("adv-" + std::to_string(iteration), base.generation(), {});
    batch.actor = rng.chance(1, 4) ? ActorId() : actor_id("tester");
    batch.source = rng.chance(1, 4) ? rng.bytes(1 + rng.index(32)) : std::string("test");
    batch.reason = rng.chance(1, 4) ? rng.bytes(1 + rng.index(64)) : std::string("reason");
    batch.recorded_at = rng.chance(1, 4) ? rng.bytes(1 + rng.index(32)) : std::string(kFixtureTimestamp);
    const std::size_t count = rng.below(4);
    for (std::size_t index = 0; index < count; ++index) {
      const std::string id = rng.token(1 + rng.index(6));
      switch (rng.below(6)) {
        case 0:
          batch.mutations.push_back(add(id, NodeKind::Rack, rng.bytes(rng.index(8))));
          break;
        case 1:
          batch.mutations.push_back(contain("fac-1", id));
          break;
        case 2:
          batch.mutations.push_back(RemoveNode{node_id("rack-01")});
          break;
        case 3:
          batch.mutations.push_back(AddAdjacency{adjacent("rack-01", "rack-02")});
          break;
        case 4:
          batch.mutations.push_back(RetireDomain{domain_id("pwr-a")});
          break;
        default:
          batch.mutations.push_back(MoveNode{node_id("rack-03"), node_id("row-1"), provenance()});
          break;
      }
    }
    const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
    if (application.accepted()) {
      FT_CHECK(application.candidate.valid());
      FT_CHECK_EQ(application.candidate.generation().value(), base.generation().value() + 1);
    } else {
      FT_CHECK(application.outcome.code != ErrorCode::Ok);
      FT_CHECK(!application.outcome.explanation.empty());
    }
  }
  // The base is untouched regardless of what the batches attempted.
  FT_CHECK(base.content_digest() == before);
  FT_CHECK_EQ(base.stats().nodes, std::size_t{11});
  FT_CHECK_EQ(base.stats().adjacency, std::size_t{3});
}
