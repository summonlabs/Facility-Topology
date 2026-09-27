// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stop_token>
#include <string>
#include <vector>

#include "dccp/facility_topology/store.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::vector<std::string> list_names(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return names;
  }
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

CreateOptions create_options(std::string_view id = "create-1") {
  CreateOptions options;
  options.actor = actor_id("operator");
  options.source = "cli";
  options.reason = "initial import";
  options.recorded_at = std::string(kFixtureTimestamp);
  options.mutation_id = mutation_id(id);
  return options;
}

/// Opens a store for mutation and creates generation 1.
TopologyStore create_store(const std::filesystem::path& directory) {
  auto store = TopologyStore::open(directory, OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto outcome = store->create(small_facility(), create_options());
  FT_REQUIRE(outcome.has_value());
  FT_CHECK(outcome->committed);
  FT_CHECK(outcome->published);
  FT_CHECK_EQ(outcome->new_generation.value(), std::uint64_t{1});
  return std::move(*store);
}

MutationBatch durable_batch(std::string_view id, const TopologyGeneration& base, const WriterEpoch& epoch,
                            std::vector<Mutation> mutations) {
  MutationBatch batch = make_batch(id, base, std::move(mutations));
  batch.authority_epoch = epoch;
  batch.actor = actor_id("operator");
  batch.source = "cli";
  return batch;
}

}  // namespace

FT_TEST(store, create_publishes_generation_one) {
  TempDirectory directory("store_create");
  TopologyStore store = create_store(directory.path());

  auto status = store.status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(status->initialized);
  FT_CHECK(!status->needs_recovery);
  FT_CHECK_EQ(status->head.value(), std::uint64_t{1});
  FT_CHECK_EQ(status->max_committed.value(), std::uint64_t{1});
  FT_CHECK_EQ(status->generation_count, std::size_t{1});
  FT_CHECK_EQ(status->pending_count, std::size_t{0});
  FT_CHECK(status->writable);
  FT_CHECK(store.epoch().valid());

  auto head = store.head();
  FT_REQUIRE(head.has_value());
  FT_CHECK(head->same_structure_as(small_facility()));

  // Creating twice is refused: there is exactly one root generation.
  auto again = store.create(small_facility(), create_options("create-2"));
  FT_CHECK_ERROR(again, ErrorCode::StoreNotEmpty);

  auto manifest = store.head_manifest();
  FT_REQUIRE(manifest.has_value());
  FT_CHECK_EQ(manifest->generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(manifest->parent_generation.value(), std::uint64_t{0});
  FT_CHECK_EQ(manifest->actor.str(), std::string("operator"));
  FT_CHECK_EQ(manifest->recorded_at, std::string(kFixtureTimestamp));
  FT_CHECK_EQ(manifest->retention_floor, std::size_t{1});
}

FT_TEST(store, on_disk_document_is_the_canonical_document) {
  TempDirectory directory("store_canonical");
  TopologyStore store = create_store(directory.path());
  auto document = store.head_document();
  FT_REQUIRE(document.has_value());
  auto expected = serialize_generation(document->snapshot, document->manifest);
  FT_REQUIRE(expected.has_value());

  const std::string on_disk = read_text(directory / "generations" / "gen-00000000000000000001.ftop");
  FT_CHECK_EQ(on_disk, *expected);
  FT_CHECK_EQ(document_digest(on_disk), document_digest(*expected));

  const std::vector<std::string> root = list_names(directory.path());
  FT_CHECK(std::find(root.begin(), root.end(), "generations") != root.end());
  FT_CHECK(std::find(root.begin(), root.end(), "pending") != root.end());
  FT_CHECK(std::find(root.begin(), root.end(), "quarantine") != root.end());
  FT_CHECK(std::find(root.begin(), root.end(), "HEAD") != root.end());
  FT_CHECK(std::find(root.begin(), root.end(), "EPOCH") != root.end());
  FT_CHECK(std::find(root.begin(), root.end(), "LOCK") != root.end());
  FT_CHECK_EQ(list_names(directory / "pending").size(), std::size_t{0});
}

FT_TEST(store, commits_advance_the_head_and_keep_old_generations_immutable) {
  TempDirectory directory("store_commit");
  TopologyStore store = create_store(directory.path());
  const std::filesystem::path first_file = directory / "generations" / "gen-00000000000000000001.ftop";
  const std::string first_bytes = read_text(first_file);

  const WriterEpoch epoch = store.epoch();
  auto outcome = store.commit(durable_batch("commit-1", TopologyGeneration(1), epoch,
                                            {add("rack-04", NodeKind::Rack, "Rack 04"),
                                             contain("row-2", "rack-04")}));
  FT_REQUIRE(outcome.has_value());
  FT_CHECK(outcome->committed);
  FT_CHECK(!outcome->replayed);
  FT_CHECK_EQ(outcome->base_generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(outcome->new_generation.value(), std::uint64_t{2});
  FT_CHECK_EQ(outcome->dispositions.size(), std::size_t{2});

  auto head = store.head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});
  FT_CHECK(head->contains_node(node_id("rack-04")));

  // Published generations are never rewritten.
  FT_CHECK_EQ(read_text(first_file), first_bytes);

  auto manifest = store.head_manifest();
  FT_REQUIRE(manifest.has_value());
  FT_CHECK_EQ(manifest->parent_generation.value(), std::uint64_t{1});
  FT_CHECK(!digest_is_zero(manifest->parent_digest));
  FT_CHECK_EQ(manifest->mutation_id.str(), std::string("commit-1"));
  FT_REQUIRE(manifest->applied.size() == 1);
  FT_CHECK_EQ(manifest->applied[0].generation.value(), std::uint64_t{2});

  // The previous generation is still loadable at its own number.
  auto previous = store.load_generation(TopologyGeneration(1));
  FT_REQUIRE(previous.has_value());
  FT_CHECK(!previous->contains_node(node_id("rack-04")));
}

FT_TEST(store, reopening_preserves_authoritative_state) {
  TempDirectory directory("store_reopen");
  {
    TopologyStore store = create_store(directory.path());
    auto outcome = store.commit(durable_batch("commit-1", TopologyGeneration(1), store.epoch(),
                                              {add("rack-04", NodeKind::Rack, "Rack 04"),
                                               contain("row-2", "rack-04")}));
    FT_REQUIRE(outcome.has_value());
  }
  {
    auto store = TopologyStore::open(directory.path(), OpenMode::ReadOnly, deterministic_config());
    FT_REQUIRE(store.has_value());
    FT_CHECK(!store->writable());
    FT_CHECK_EQ(store->epoch().value(), std::uint64_t{0});
    auto head = store->head();
    FT_REQUIRE(head.has_value());
    FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});
    FT_CHECK(head->contains_node(node_id("rack-04")));
    FT_CHECK_ERROR(store->commit(durable_batch("commit-2", TopologyGeneration(2), WriterEpoch(1), {})),
                   ErrorCode::StoreReadOnly);
    FT_CHECK_ERROR(store->create(small_facility(), create_options("create-9")), ErrorCode::StoreReadOnly);
    FT_CHECK_ERROR(store->recover(), ErrorCode::StoreReadOnly);
  }
  {
    auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
    FT_REQUIRE(store.has_value());
    FT_CHECK(store->epoch().valid());
    auto head = store->head();
    FT_REQUIRE(head.has_value());
    FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});
    auto numbers = store->generation_numbers();
    FT_REQUIRE(numbers.has_value());
    FT_CHECK_EQ(numbers->size(), std::size_t{2});
  }
}

FT_TEST(store, stale_base_and_stale_authority_are_rejected) {
  TempDirectory directory("store_stale");
  TopologyStore store = create_store(directory.path());
  const WriterEpoch epoch = store.epoch();

  auto stale_base = store.commit(durable_batch("commit-stale", TopologyGeneration(9), epoch,
                                               {add("rack-04", NodeKind::Rack)}));
  FT_CHECK_ERROR(stale_base, ErrorCode::StaleBaseGeneration);

  auto wrong_epoch = store.commit(durable_batch("commit-epoch", TopologyGeneration(1), WriterEpoch(999),
                                                {add("rack-04", NodeKind::Rack)}));
  FT_CHECK_ERROR(wrong_epoch, ErrorCode::StaleAuthorityEpoch);

  auto no_epoch = store.commit(make_batch("commit-noepoch", TopologyGeneration(1), {add("rack-04", NodeKind::Rack)}));
  FT_CHECK_ERROR(no_epoch, ErrorCode::StaleAuthorityEpoch);

  // An absent identity cannot be invented by the store.
  MutationBatch anonymous = durable_batch("commit-anon", TopologyGeneration(1), epoch, {add("rack-04", NodeKind::Rack)});
  anonymous.mutation_id = MutationId();
  FT_CHECK_ERROR(store.commit(anonymous), ErrorCode::MissingField);

  // A successful commit still works after the rejected attempts.
  auto accepted = store.commit(durable_batch("commit-ok", TopologyGeneration(1), epoch,
                                             {add("rack-04", NodeKind::Rack), contain("row-2", "rack-04")}));
  FT_REQUIRE_OK(accepted);
  FT_CHECK_EQ(accepted->new_generation.value(), std::uint64_t{2});

  // The store no longer accepts the superseded base.
  auto after = store.commit(durable_batch("commit-late", TopologyGeneration(1), epoch,
                                          {add("rack-05", NodeKind::Rack)}));
  FT_CHECK_ERROR(after, ErrorCode::StaleBaseGeneration);
}

FT_TEST(store, a_new_writer_fences_the_previous_epoch) {
  TempDirectory directory("store_fence");
  auto first = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(first.has_value());
  FT_REQUIRE(first->create(small_facility(), create_options()).has_value());
  const WriterEpoch first_epoch = first->epoch();

  auto second = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(second.has_value());
  FT_CHECK(second->epoch() > first_epoch);

  // The first handle still holds a stale epoch: its commits must be rejected
  // even though its base generation is current.
  auto fenced = first->commit(durable_batch("commit-fenced", TopologyGeneration(1), first_epoch,
                                            {add("rack-04", NodeKind::Rack)}));
  FT_CHECK_ERROR(fenced, ErrorCode::StaleAuthorityEpoch);

  auto accepted = second->commit(durable_batch("commit-current", TopologyGeneration(1), second->epoch(),
                                               {add("rack-04", NodeKind::Rack), contain("row-2", "rack-04")}));
  FT_REQUIRE(accepted.has_value());
  FT_CHECK_EQ(accepted->new_generation.value(), std::uint64_t{2});
}

FT_TEST(store, replayed_batches_are_idempotent) {
  TempDirectory directory("store_idempotent");
  TopologyStore store = create_store(directory.path());
  const WriterEpoch epoch = store.epoch();
  const MutationBatch batch = durable_batch("commit-repeat", TopologyGeneration(1), epoch,
                                            {add("rack-04", NodeKind::Rack, "Rack 04"),
                                             contain("row-2", "rack-04")});

  auto first = store.commit(batch);
  FT_REQUIRE_OK(first);
  FT_CHECK(!first->replayed);
  FT_CHECK_EQ(first->new_generation.value(), std::uint64_t{2});

  auto replay = store.commit(batch);
  FT_REQUIRE_OK(replay);
  FT_CHECK(replay->replayed);
  FT_CHECK(!replay->published);
  FT_CHECK(replay->accepted());
  FT_CHECK_EQ(replay->new_generation.value(), std::uint64_t{2});
  FT_CHECK(replay->explanation.find("already applied") != std::string::npos);

  // Nothing was written: the head is still generation 2 and the store has two
  // generation files.
  auto status = store.status();
  FT_REQUIRE(status.has_value());
  FT_CHECK_EQ(status->max_committed.value(), std::uint64_t{2});
  FT_CHECK_EQ(status->generation_count, std::size_t{2});

  // Reusing the identity for different content is a conflict, not a replay.
  MutationBatch different = batch;
  different.mutations.push_back(add("rack-05", NodeKind::Rack));
  FT_CHECK_ERROR(store.commit(different), ErrorCode::IdentityConflict);

  // The replay survives a restart: the identity window is durable.
  auto reopened = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(reopened.has_value());
  MutationBatch replayed_after_restart = batch;
  replayed_after_restart.authority_epoch = reopened->epoch();
  auto restart_replay = reopened->commit(replayed_after_restart);
  FT_REQUIRE(restart_replay.has_value());
  FT_CHECK(restart_replay->replayed);
  FT_CHECK_EQ(restart_replay->new_generation.value(), std::uint64_t{2});
}

FT_TEST(store, rejected_batches_leave_the_store_untouched) {
  TempDirectory directory("store_rejected");
  TopologyStore store = create_store(directory.path());
  const std::string head_before = read_text(directory / "HEAD");
  const std::string epoch_before = read_text(directory / "EPOCH");
  const std::string generation_before = read_text(directory / "generations" / "gen-00000000000000000001.ftop");

  auto rejected = store.commit(durable_batch("commit-bad", TopologyGeneration(1), store.epoch(),
                                             {add("rack-04", NodeKind::Rack), contain("row-1", "rack-04"),
                                              contain("row-2", "rack-04")}));
  FT_CHECK_ERROR(rejected, ErrorCode::IdentityConflict);
  FT_CHECK(rejected.error().message().find("STALE") == std::string::npos);

  // A whole-graph failure is reported as a batch rejection with its own code.
  auto structural = store.commit(durable_batch("commit-bad2", TopologyGeneration(1), store.epoch(),
                                               {RemoveNode{node_id("rack-01")}}));
  FT_CHECK_ERROR(structural, ErrorCode::NodeReferenced);

  FT_CHECK_EQ(read_text(directory / "HEAD"), head_before);
  FT_CHECK_EQ(read_text(directory / "EPOCH"), epoch_before);
  FT_CHECK_EQ(read_text(directory / "generations" / "gen-00000000000000000001.ftop"), generation_before);
  FT_CHECK_EQ(list_names(directory / "pending").size(), std::size_t{0});
  auto status = store.status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->needs_recovery);
}

FT_TEST(store, retention_bounds_persisted_generation_growth) {
  TempDirectory directory("store_retention");
  StoreConfig config = deterministic_config();
  config.limits.retained_generations = 2;
  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, config);
  FT_REQUIRE(store.has_value());
  FT_REQUIRE(store->create(small_facility(), create_options()).has_value());

  for (int index = 0; index < 6; ++index) {
    const auto head = store->head_generation();
    FT_REQUIRE(head.has_value());
    const std::string id = "commit-" + std::to_string(index);
    auto outcome = store->commit(durable_batch(id, *head, store->epoch(),
                                               {add("rack-" + std::to_string(20 + index), NodeKind::Rack),
                                                contain("row-2", "rack-" + std::to_string(20 + index))}));
    FT_REQUIRE(outcome.has_value());
  }

  auto numbers = store->generation_numbers();
  FT_REQUIRE(numbers.has_value());
  FT_CHECK_EQ(numbers->size(), std::size_t{2});
  FT_CHECK_EQ(numbers->front().value(), std::uint64_t{6});
  FT_CHECK_EQ(numbers->back().value(), std::uint64_t{7});

  // The pruned generation is reported as not retained rather than as corrupt.
  FT_CHECK_ERROR(store->load_generation(TopologyGeneration(1)), ErrorCode::GenerationNotRetained);
}

FT_TEST(store, export_and_import_round_trip) {
  TempDirectory directory("store_export");
  TopologyStore store = create_store(directory.path());
  auto outcome = store.commit(durable_batch("commit-1", TopologyGeneration(1), store.epoch(),
                                            {add("rack-04", NodeKind::Rack, "Rack 04"),
                                             contain("row-2", "rack-04")}));
  FT_REQUIRE(outcome.has_value());

  const std::filesystem::path exported = directory / "exported.ftop";
  FT_CHECK_OK(store.export_head_to(exported));
  FT_CHECK(std::filesystem::is_regular_file(exported));

  auto imported = TopologyStore::import_from(exported);
  FT_REQUIRE(imported.has_value());
  FT_CHECK_EQ(imported->manifest.generation.value(), std::uint64_t{2});
  FT_CHECK(imported->snapshot.contains_node(node_id("rack-04")));

  auto head = store.head();
  FT_REQUIRE(head.has_value());
  FT_CHECK(imported->snapshot.same_structure_as(*head));

  // Importing a tampered export fails with the digest finding.
  std::string tampered = read_text(exported);
  tampered[20] = tampered[20] == 'x' ? 'y' : 'x';
  const std::filesystem::path broken = directory / "broken.ftop";
  {
    std::ofstream stream(broken, std::ios::binary);
    stream << tampered;
  }
  FT_CHECK_ERROR(TopologyStore::import_from(broken), ErrorCode::DigestMismatch);
  FT_CHECK_ERROR(TopologyStore::import_from(directory / "missing.ftop"), ErrorCode::StoreNotFound);
  FT_CHECK_ERROR(store.export_head_to(std::filesystem::path()), ErrorCode::PathInvalid);
}

FT_TEST(store, an_exported_document_can_seed_a_fresh_store) {
  TempDirectory source_directory("store_seed_source");
  TopologyStore source = create_store(source_directory.path());
  const std::filesystem::path exported = source_directory / "facility.ftop";
  FT_CHECK_OK(source.export_head_to(exported));

  auto imported = TopologyStore::import_from(exported);
  FT_REQUIRE(imported.has_value());

  TempDirectory target_directory("store_seed_target");
  auto target = TopologyStore::open(target_directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(target.has_value());
  CreateOptions options = create_options("seed-1");
  options.actor = actor_id("importer");
  auto seeded = target->create(imported->snapshot, options);
  FT_REQUIRE(seeded.has_value());
  FT_CHECK_EQ(seeded->new_generation.value(), std::uint64_t{1});

  auto head = target->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK(head->same_structure_as(imported->snapshot));

  auto manifest = target->head_manifest();
  FT_REQUIRE(manifest.has_value());
  FT_CHECK_EQ(manifest->actor.str(), std::string("importer"));
  FT_CHECK_EQ(manifest->parent_generation.value(), std::uint64_t{0});
}

FT_TEST(store, open_rejects_invalid_locations_and_modes) {
  TempDirectory directory("store_open");
  FT_CHECK_ERROR(TopologyStore::open(directory / "absent", OpenMode::ReadOnly, deterministic_config()),
                 ErrorCode::StoreNotFound);

  StoreConfig no_create = deterministic_config();
  no_create.create_directory = false;
  FT_CHECK_ERROR(TopologyStore::open(directory / "absent", OpenMode::ReadWrite, no_create), ErrorCode::StoreNotFound);

  const std::filesystem::path file = directory / "a-file";
  {
    std::ofstream stream(file);
    stream << "not a directory";
  }
  FT_CHECK_ERROR(TopologyStore::open(file, OpenMode::ReadWrite, deterministic_config()), ErrorCode::PathInvalid);
  FT_CHECK_ERROR(TopologyStore::open(std::filesystem::path(), OpenMode::ReadWrite, deterministic_config()),
                 ErrorCode::PathInvalid);

  StoreConfig broken = deterministic_config();
  broken.limits.max_nodes = 0;
  FT_CHECK_ERROR(TopologyStore::open(directory / "store", OpenMode::ReadWrite, broken), ErrorCode::InvalidArgument);
}

FT_TEST(store, uninitialized_stores_report_precisely) {
  TempDirectory directory("store_uninitialized");
  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());

  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->initialized);
  FT_CHECK(status->needs_recovery);

  FT_CHECK_ERROR(store->head(), ErrorCode::HeadMissing);
  FT_CHECK_ERROR(store->head_generation(), ErrorCode::HeadMissing);
  FT_CHECK_ERROR(store->head_document(), ErrorCode::HeadMissing);
  FT_CHECK_ERROR(store->export_head_to(directory / "out.ftop"), ErrorCode::HeadMissing);
  FT_CHECK_ERROR(store->commit(durable_batch("commit-1", TopologyGeneration(0), store->epoch(),
                                             {add("rack-1", NodeKind::Rack)})),
                 ErrorCode::RecoveryRequired);

  auto numbers = store->generation_numbers();
  FT_REQUIRE(numbers.has_value());
  FT_CHECK(numbers->empty());

  auto closed = store->close();
  FT_CHECK_OK(closed);
  FT_CHECK(!store->is_open());
  FT_CHECK_ERROR(store->head(), ErrorCode::StoreClosed);
  FT_CHECK_ERROR(store->status(), ErrorCode::StoreClosed);
  FT_CHECK_OK(store->close());
}

FT_TEST(store, closed_and_moved_handles_behave) {
  TempDirectory directory("store_moved");
  TopologyStore store = create_store(directory.path());
  TopologyStore moved = std::move(store);
  FT_CHECK(moved.is_open());
  FT_CHECK(!store.is_open());
  FT_CHECK_ERROR(store.status(), ErrorCode::StoreClosed);
  auto head = moved.head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{1});

  TopologyStore assigned;
  assigned = std::move(moved);
  FT_CHECK(assigned.is_open());
  FT_CHECK(!moved.is_open());
  FT_CHECK_OK(assigned.close());
  FT_CHECK(!assigned.is_open());
  FT_CHECK_OK(assigned.close());
  FT_CHECK_ERROR(assigned.head(), ErrorCode::StoreClosed);
}

FT_TEST(store, cancellation_stops_a_commit_before_it_publishes) {
  TempDirectory directory("store_cancel");
  TopologyStore store = create_store(directory.path());
  const std::string head_before = read_text(directory / "HEAD");

  std::stop_source source;
  source.request_stop();
  CommitOptions options;
  options.stop = source.get_token();

  auto cancelled = store.commit(durable_batch("commit-cancelled", TopologyGeneration(1), store.epoch(),
                                              {add("rack-04", NodeKind::Rack), contain("row-2", "rack-04")}),
                                options);
  FT_CHECK_ERROR(cancelled, ErrorCode::Cancelled);

  // Nothing was written and no temporary state was left behind.
  FT_CHECK_EQ(read_text(directory / "HEAD"), head_before);
  FT_CHECK_EQ(list_names(directory / "pending").size(), std::size_t{0});
  auto status = store.status();
  FT_REQUIRE_OK(status);
  FT_CHECK(!status->needs_recovery);
  FT_CHECK_EQ(status->max_committed.value(), std::uint64_t{1});

  // A later, uncancelled attempt with the same identity is still fresh: the
  // cancelled attempt never claimed it.
  auto accepted = store.commit(durable_batch("commit-cancelled", TopologyGeneration(1), store.epoch(),
                                             {add("rack-04", NodeKind::Rack), contain("row-2", "rack-04")}));
  FT_REQUIRE_OK(accepted);
  FT_CHECK(!accepted->replayed);
  FT_CHECK_EQ(accepted->new_generation.value(), std::uint64_t{2});
}

FT_TEST(store, store_status_is_advisory_and_deterministic_when_idle) {
  TempDirectory directory("store_status");
  TopologyStore store = create_store(directory.path());
  auto first = store.status();
  auto second = store.status();
  FT_REQUIRE(first.has_value());
  FT_REQUIRE(second.has_value());
  FT_CHECK_EQ(first->explanation, second->explanation);
  FT_CHECK_EQ(first->generation_count, second->generation_count);
  FT_CHECK_EQ(first->quarantine_count, std::size_t{0});
  FT_CHECK(first->open);
}
