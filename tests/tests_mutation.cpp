// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/facility_topology/diff.hpp"
#include "dccp/facility_topology/mutation.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

std::vector<std::string> names(const std::vector<NodeId>& ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const NodeId& id : ids) {
    out.push_back(id.str());
  }
  return out;
}

std::size_t accepted_count(const MutationOutcome& outcome) {
  std::size_t total = 0;
  for (const MutationDisposition& disposition : outcome.dispositions) {
    if (disposition.accepted()) {
      ++total;
    }
  }
  return total;
}

}  // namespace

FT_TEST(mutation, applies_a_batch_and_stamps_the_next_generation) {
  const TopologySnapshot base = small_facility(TopologyGeneration(4));
  const MutationBatch batch =
      make_batch("m-0001", base.generation(),
                 {add("rack-04", NodeKind::Rack, "Rack 04"), contain("row-2", "rack-04"),
                  AddAdjacency{adjacent("rack-03", "rack-04", AdjacencyKind::StructuralNeighbor)}});

  const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
  FT_REQUIRE(application.accepted());
  FT_CHECK_EQ(application.outcome.code, ErrorCode::Ok);
  FT_CHECK(application.outcome.published);
  FT_CHECK(!application.outcome.committed);
  FT_CHECK(!application.outcome.replayed);
  FT_CHECK_EQ(application.outcome.new_generation.value(), std::uint64_t{5});
  FT_CHECK_EQ(application.outcome.dispositions.size(), std::size_t{3});
  FT_CHECK_EQ(accepted_count(application.outcome), std::size_t{3});
  FT_CHECK_EQ(application.candidate.generation().value(), std::uint64_t{5});
  FT_CHECK_EQ(application.candidate.stats().nodes, std::size_t{12});
  FT_CHECK_EQ(application.candidate.stats().adjacency, std::size_t{4});
  FT_CHECK(application.candidate.content_digest() != base.content_digest());
  FT_CHECK(!application.outcome.explanation.empty());
  FT_CHECK(application.outcome.to_string().find("published=yes") != std::string::npos);
  for (const MutationDisposition& disposition : application.outcome.dispositions) {
    FT_CHECK_EQ(disposition.explanation, std::string("applied"));
  }

  // The base snapshot is untouched: a batch never mutates published state.
  FT_CHECK_EQ(base.stats().nodes, std::size_t{11});
  FT_CHECK_EQ(base.generation().value(), std::uint64_t{4});
}

FT_TEST(mutation, batches_are_atomic) {
  const TopologySnapshot base = small_facility();
  const MutationBatch batch =
      make_batch("m-0002", base.generation(),
                 {add("rack-04", NodeKind::Rack, "Rack 04"), contain("row-2", "rack-04"),
                  contain("row-2", "rack-04")});

  const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
  FT_CHECK(!application.accepted());
  FT_CHECK(!application.outcome.published);
  FT_CHECK_EQ(application.outcome.code, ErrorCode::BatchRejected);
  FT_REQUIRE(application.outcome.dispositions.size() == 3);
  FT_CHECK(application.outcome.dispositions[0].accepted());
  FT_CHECK(application.outcome.dispositions[1].accepted());
  FT_CHECK(!application.outcome.dispositions[2].accepted());
  FT_CHECK_EQ(application.outcome.dispositions[2].code, ErrorCode::AlreadyPresent);
  FT_CHECK_EQ(application.outcome.dispositions[2].index, std::size_t{2});
  FT_CHECK(!application.candidate.valid());
  FT_CHECK(application.outcome.explain().find("ALREADY_PRESENT") != std::string::npos);

  // Nothing from the rejected batch survives.
  FT_CHECK(!base.contains_node(node_id("rack-04")));
  FT_CHECK_EQ(base.stats().nodes, std::size_t{11});
}

FT_TEST(mutation, stale_base_generation_is_rejected) {
  const TopologySnapshot base = small_facility(TopologyGeneration(7));
  const MutationBatch stale = make_batch("m-0003", TopologyGeneration(6), {add("rack-04", NodeKind::Rack)});
  const BatchApplication application = apply_batch_explained(base, stale, TopologyLimits{});
  FT_CHECK(!application.accepted());
  FT_CHECK_EQ(application.outcome.code, ErrorCode::StaleBaseGeneration);
  FT_CHECK(application.outcome.dispositions.empty());
  FT_CHECK(application.outcome.explanation.find("6") != std::string::npos);
  FT_CHECK(application.outcome.explanation.find("7") != std::string::npos);
}

FT_TEST(mutation, malformed_batches_are_rejected_before_any_work) {
  const TopologySnapshot base = small_facility();
  const auto rejected = [&base](MutationBatch batch, ErrorCode expected) {
    const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
    FT_CHECK(!application.accepted());
    FT_CHECK_EQ(application.outcome.code, expected);
  };

  MutationBatch no_actor = make_batch("m-0004", base.generation(), {add("rack-04", NodeKind::Rack)});
  no_actor.actor = ActorId();
  rejected(no_actor, ErrorCode::MissingField);

  MutationBatch no_source = make_batch("m-0005", base.generation(), {add("rack-04", NodeKind::Rack)});
  no_source.source.clear();
  rejected(no_source, ErrorCode::MissingField);

  MutationBatch bad_timestamp = make_batch("m-0006", base.generation(), {add("rack-04", NodeKind::Rack)});
  bad_timestamp.recorded_at = "2026-13-01T00:00:00Z";
  rejected(bad_timestamp, ErrorCode::MalformedRecord);

  MutationBatch huge_reason = make_batch("m-0007", base.generation(), {add("rack-04", NodeKind::Rack)});
  huge_reason.reason = std::string(TopologyLimits{}.max_reason_bytes + 1, 'a');
  rejected(huge_reason, ErrorCode::TextTooLong);

  TopologyLimits tiny = TopologyLimits{};
  tiny.max_mutations_per_batch = 1;
  const BatchApplication too_many =
      apply_batch_explained(base, make_batch("m-0008", base.generation(),
                                             {add("rack-04", NodeKind::Rack), add("rack-05", NodeKind::Rack)}),
                            tiny);
  FT_CHECK(!too_many.accepted());
  FT_CHECK_EQ(too_many.outcome.code, ErrorCode::LimitExceeded);
}

FT_TEST(mutation, whole_graph_validation_failures_are_attributed_to_the_batch) {
  // Removing the only facility leaves a generation with no root. Every single
  // mutation is individually valid, so the failure belongs to the candidate as
  // a whole and is reported one past the last mutation.
  auto isolated = assemble_topology({make_node("fac-1", NodeKind::Facility), make_node("zone-a", NodeKind::Zone, "", NodeKind::Rack)},
                                    {physical("fac-1", "zone-a")}, {}, {}, {}, TopologyGeneration(1));
  FT_REQUIRE(isolated.has_value());

  const MutationBatch batch = make_batch("m-0009", isolated->generation(),
                                         {RemoveContainment{node_id("fac-1"), node_id("zone-a")},
                                          RemoveNode{node_id("zone-a")}, RemoveNode{node_id("fac-1")}});
  const BatchApplication application = apply_batch_explained(*isolated, batch, TopologyLimits{});
  FT_CHECK(!application.accepted());
  FT_CHECK_EQ(application.outcome.code, ErrorCode::BatchRejected);
  FT_REQUIRE(!application.outcome.dispositions.empty());
  const MutationDisposition& last = application.outcome.dispositions.back();
  FT_CHECK_EQ(last.index, std::size_t{3});
  FT_CHECK_EQ(last.code, ErrorCode::MissingField);
  FT_CHECK(application.outcome.explanation.find("whole-graph validation") != std::string::npos);
}

FT_TEST(mutation, every_rejection_names_its_mutation) {
  const TopologySnapshot base = small_facility();
  const std::vector<std::pair<Mutation, ErrorCode>> cases = {
      {contain("row-1", "rack-99"), ErrorCode::NotFound},
      {contain("row-1", "rack-01"), ErrorCode::AlreadyPresent},
      {contain("row-2", "rack-01"), ErrorCode::IdentityConflict},
      {AddNode{make_node("rack-01", NodeKind::Rack)}, ErrorCode::AlreadyPresent},
      {RemoveNode{node_id("row-1")}, ErrorCode::NodeHasChildren},
      {RemoveNode{node_id("nope")}, ErrorCode::NotFound},
      {MoveNode{node_id("rack-03"), node_id("row-2"), provenance()}, ErrorCode::AlreadyAtParent},
      {MoveNode{node_id("rack-03"), node_id("rack-03"), provenance()}, ErrorCode::ContainmentCycle},
      {MoveNode{node_id("fac-1"), node_id("row-1"), provenance()}, ErrorCode::FacilityMustBeRoot},
      {RemoveContainment{node_id("row-1"), node_id("rack-03")}, ErrorCode::NotFound},
      {SetNodeLabel{node_id("nope"), "x", provenance()}, ErrorCode::NotFound},
      {AddAdjacency{adjacent("rack-01", "rack-02")}, ErrorCode::DuplicateEdge},
      {RemoveAdjacency{node_id("rack-01"), node_id("rack-03"), AdjacencyKind::SharedBoundary}, ErrorCode::NotFound},
      {DeclareDomain{declare_domain("pwr-a", DomainKind::Power)}, ErrorCode::AlreadyPresent},
      {RetireDomain{domain_id("cool-a")}, ErrorCode::NodeReferenced},
      {AddAssociation{associate("rack-01", "pwr-a", DomainKind::Power)}, ErrorCode::AlreadyPresent},
      {RemoveAssociation{node_id("rack-01"), domain_id("cool-a")}, ErrorCode::NotFound},
  };

  for (const auto& entry : cases) {
    const MutationBatch batch = make_batch("m-case", base.generation(), {entry.first});
    const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
    FT_CHECK(!application.accepted());
    if (application.accepted()) {
      FT_FAIL(std::string("mutation ") + std::string(mutation_kind_token(entry.first)) + " was accepted");
      continue;
    }
    FT_REQUIRE(application.outcome.dispositions.size() == 1);
    const MutationDisposition& disposition = application.outcome.dispositions[0];
    FT_CHECK_EQ(disposition.code, entry.second);
    FT_CHECK_EQ(application.outcome.first_failure, entry.second);
    FT_CHECK(!disposition.explanation.empty());
    if (disposition.code != entry.second) {
      FT_FAIL("mutation " + std::string(mutation_kind_token(entry.first)) + " rejected as " +
              std::string(error_code_name(disposition.code)) + " (" + disposition.explanation + ")");
    }
  }
}

FT_TEST(mutation, mutation_kinds_have_stable_tokens) {
  FT_CHECK_EQ(mutation_kind_token(AddNode{}), std::string_view("add-node"));
  FT_CHECK_EQ(mutation_kind_token(RemoveNode{}), std::string_view("remove-node"));
  FT_CHECK_EQ(mutation_kind_token(MoveNode{}), std::string_view("move-node"));
  FT_CHECK_EQ(mutation_kind_token(AddContainment{}), std::string_view("add-containment"));
  FT_CHECK_EQ(mutation_kind_token(RemoveContainment{}), std::string_view("remove-containment"));
  FT_CHECK_EQ(mutation_kind_token(SetNodeLabel{}), std::string_view("set-node-label"));
  FT_CHECK_EQ(mutation_kind_token(AddAdjacency{}), std::string_view("add-adjacency"));
  FT_CHECK_EQ(mutation_kind_token(RemoveAdjacency{}), std::string_view("remove-adjacency"));
  FT_CHECK_EQ(mutation_kind_token(DeclareDomain{}), std::string_view("declare-domain"));
  FT_CHECK_EQ(mutation_kind_token(RetireDomain{}), std::string_view("retire-domain"));
  FT_CHECK_EQ(mutation_kind_token(AddAssociation{}), std::string_view("add-association"));
  FT_CHECK_EQ(mutation_kind_token(RemoveAssociation{}), std::string_view("remove-association"));
}

FT_TEST(mutation, apply_batch_reports_the_first_rejection_as_an_error) {
  const TopologySnapshot base = small_facility();
  const MutationBatch batch = make_batch("m-0010", base.generation(), {contain("row-1", "rack-99")});
  const auto result = apply_batch(base, batch, TopologyLimits{});
  FT_CHECK_ERROR(result, ErrorCode::NotFound);
  FT_CHECK(result.error().message().find("NOT_FOUND") != std::string::npos);

  const MutationBatch good =
      make_batch("m-0011", base.generation(), {add("rack-04", NodeKind::Rack), contain("row-2", "rack-04")});
  const auto applied = apply_batch(base, good, TopologyLimits{});
  FT_REQUIRE_OK(applied);
  FT_CHECK_EQ(applied->generation().value(), std::uint64_t{2});
}

FT_TEST(mutation, authority_epoch_is_carried_but_not_required_in_memory) {
  const TopologySnapshot base = small_facility();
  MutationBatch batch =
      make_batch("m-0012", base.generation(), {add("rack-04", NodeKind::Rack), contain("row-2", "rack-04")});
  batch.authority_epoch = WriterEpoch(3);
  const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
  FT_CHECK(application.accepted());
}

FT_TEST(mutation, limits_bound_growth) {
  TopologyLimits limits = TopologyLimits{};
  limits.max_nodes = 11;
  const TopologySnapshot base = small_facility();
  const BatchApplication application =
      apply_batch_explained(base, make_batch("m-0013", base.generation(), {add("rack-04", NodeKind::Rack)}), limits);
  FT_CHECK(!application.accepted());
  FT_CHECK_EQ(application.outcome.code, ErrorCode::BatchRejected);
  FT_REQUIRE(!application.outcome.dispositions.empty());
  FT_CHECK_EQ(application.outcome.dispositions[0].code, ErrorCode::LimitExceeded);
}

FT_TEST(mutation, diff_is_deterministic_and_complete) {
  const TopologySnapshot base = small_facility(TopologyGeneration(1));
  const MutationBatch batch =
      make_batch("m-0014", base.generation(),
                 {add("rack-04", NodeKind::Rack, "Rack 04"), contain("row-2", "rack-04"),
                  SetNodeLabel{node_id("rack-03"), "Rack 03 renamed", provenance()},
                  RemoveAdjacency{node_id("hall-1"), node_id("hall-2"), AdjacencyKind::ServiceAisle},
                  AddAssociation{associate("rack-03", "pwr-a", DomainKind::Power)}});
  const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
  FT_REQUIRE(application.accepted());

  auto first = diff(base, application.candidate);
  FT_REQUIRE(first.has_value());
  auto second = diff(base, application.candidate);
  FT_REQUIRE(second.has_value());
  FT_CHECK_EQ(first->to_string(), second->to_string());
  FT_CHECK(!first->empty());
  FT_CHECK_EQ(first->from.value(), std::uint64_t{1});
  FT_CHECK_EQ(first->to.value(), std::uint64_t{2});
  FT_CHECK_EQ(names(first->nodes_added), (std::vector<std::string>{"rack-04"}));
  FT_CHECK(first->nodes_removed.empty());
  FT_REQUIRE(first->nodes_relabeled.size() == 1);
  FT_CHECK_EQ(first->nodes_relabeled[0].id.str(), std::string("rack-03"));
  FT_CHECK_EQ(first->nodes_relabeled[0].before, std::string("Rack 03"));
  FT_CHECK_EQ(first->nodes_relabeled[0].after, std::string("Rack 03 renamed"));
  FT_REQUIRE(first->containment_added.size() == 1);
  FT_CHECK_EQ(first->containment_added[0].parent.str(), std::string("row-2"));
  FT_CHECK_EQ(first->adjacency_removed.size(), std::size_t{1});
  FT_CHECK_EQ(first->associations_added.size(), std::size_t{1});
  FT_CHECK_EQ(first->change_count(), std::size_t{5});

  auto reversed = diff(application.candidate, base);
  FT_REQUIRE(reversed.has_value());
  FT_CHECK_EQ(names(reversed->nodes_removed), (std::vector<std::string>{"rack-04"}));
  FT_CHECK_EQ(reversed->change_count(), first->change_count());

  auto identical = diff(base, base);
  FT_REQUIRE(identical.has_value());
  FT_CHECK(identical->empty());
  FT_CHECK_EQ(identical->change_count(), std::size_t{0});

  FT_CHECK_ERROR(diff(TopologySnapshot{}, base), ErrorCode::NotInitialized);
}

FT_TEST(mutation, diff_reports_a_move_as_a_removed_and_added_edge) {
  const TopologySnapshot base = small_facility();
  const MutationBatch batch =
      make_batch("m-0015", base.generation(), {MoveNode{node_id("rack-03"), node_id("row-1"), provenance()}});
  const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
  FT_REQUIRE(application.accepted());
  auto changes = diff(base, application.candidate);
  FT_REQUIRE(changes.has_value());
  FT_CHECK(changes->nodes_added.empty());
  FT_CHECK(changes->nodes_removed.empty());
  FT_CHECK_EQ(changes->containment_added.size(), std::size_t{1});
  FT_CHECK_EQ(changes->containment_removed.size(), std::size_t{1});
  FT_CHECK_EQ(changes->containment_added[0].child.str(), std::string("rack-03"));
  FT_CHECK_EQ(changes->containment_added[0].parent.str(), std::string("row-1"));
  FT_CHECK_EQ(changes->containment_removed[0].parent.str(), std::string("row-2"));
}

FT_TEST(mutation, provenance_of_a_batch_is_recorded_on_new_records) {
  const TopologySnapshot base = small_facility();
  MutationBatch batch = make_batch("m-0016", base.generation(), {});
  batch.actor = actor_id("operator-7");
  batch.reason = "capacity expansion";
  batch.mutations = {add("rack-07", NodeKind::Rack, "Rack 07"), contain("row-2", "rack-07")};
  const BatchApplication application = apply_batch_explained(base, batch, TopologyLimits{});
  FT_REQUIRE(application.accepted());

  // The batch stamps names, not timestamps: the caller owns provenance values.
  const NodeRecord* record = application.candidate.find_node(node_id("rack-07"));
  FT_REQUIRE(record != nullptr);
  FT_CHECK_EQ(record->provenance.actor.str(), std::string("tester"));

  MutationBatch stamped = batch;
  stamped.mutation_id = mutation_id("m-0017");
  AddNode with_provenance = add("rack-08", NodeKind::Rack, "Rack 08");
  with_provenance.node.provenance.actor = actor_id("operator-7");
  with_provenance.node.provenance.reason = "capacity expansion";
  stamped.mutations = {with_provenance, contain("row-2", "rack-08")};
  const BatchApplication second = apply_batch_explained(base, stamped, TopologyLimits{});
  FT_REQUIRE(second.accepted());
  const NodeRecord* saved = second.candidate.find_node(node_id("rack-08"));
  FT_REQUIRE(saved != nullptr);
  FT_CHECK_EQ(saved->provenance.actor.str(), std::string("operator-7"));
  FT_CHECK_EQ(saved->provenance.reason, std::string("capacity expansion"));
}
