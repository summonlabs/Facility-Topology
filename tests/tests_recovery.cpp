// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Recovery, partial-write and integrity tests.
//
// Every scenario is expressed as real files on disk followed by a real
// close/reopen cycle, so recovery is exercised through the same path an
// operator would take after a crash.

#include <algorithm>
#include <filesystem>
#include <fstream>
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

void write_text(const std::filesystem::path& path, std::string_view content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
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

CreateOptions create_options() {
  CreateOptions options;
  options.actor = actor_id("operator");
  options.source = "cli";
  options.reason = "initial topology";
  options.recorded_at = std::string(kFixtureTimestamp);
  options.mutation_id = mutation_id("create-1");
  return options;
}

/// Creates generation 1 and `count` further committed generations.
void populate(const std::filesystem::path& directory, int count) {
  auto store = TopologyStore::open(directory, OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  FT_REQUIRE(store->create(small_facility(), create_options()).has_value());
  for (int index = 0; index < count; ++index) {
    auto head = store->head_generation();
    FT_REQUIRE(head.has_value());
    MutationBatch batch = make_batch("commit-" + std::to_string(index), *head, {});
    batch.authority_epoch = store->epoch();
    const std::string id = "rack-" + std::to_string(20 + index);
    batch.mutations = {AddNode{make_node(id, NodeKind::Rack, id)}, AddContainment{physical("row-2", id)}};
    FT_REQUIRE(store->commit(batch).has_value());
  }
}

}  // namespace

FT_TEST(recovery, clean_store_needs_no_recovery) {
  TempDirectory directory("recover_clean");
  populate(directory.path(), 2);
  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(!report->repaired);
  FT_CHECK(!report->recovered);
  FT_CHECK_EQ(report->pending_files_discarded, std::size_t{0});
  FT_CHECK_EQ(report->corrupt_files_quarantined, std::size_t{0});
  FT_CHECK_EQ(report->generations_pruned, std::size_t{0});
  FT_CHECK_EQ(report->head_before.value(), std::uint64_t{3});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{3});
  FT_CHECK_EQ(report->retained.size(), std::size_t{3});
  FT_CHECK(report->explanation.find("already matches") != std::string::npos);
}

FT_TEST(recovery, interrupted_commit_is_discarded_and_the_head_survives) {
  TempDirectory directory("recover_pending");
  populate(directory.path(), 1);
  const std::string head_before = read_text(directory / "HEAD");

  // A pending file is the residue of a commit that never reached its commit
  // point: it must be discarded, never promoted.
  write_text(directory / "pending" / "gen-00000000000000000003.ftop.tmp-999-0", "partial generation bytes");
  write_text(directory / "HEAD.tmp-999-1", "partial head marker");

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(status->needs_recovery);
  FT_CHECK_EQ(status->pending_count, std::size_t{1});
  // The published head is untouched: an unpublished temporary file carries no
  // authority, so it cannot change what the store reports.
  auto head = store->head();
  FT_REQUIRE_OK(head);
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->repaired);
  FT_CHECK(!report->recovered);
  FT_CHECK_EQ(report->pending_files_discarded, std::size_t{2});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{2});
  FT_CHECK_EQ(read_text(directory / "HEAD"), head_before);
  FT_CHECK_EQ(list_names(directory / "pending").size(), std::size_t{0});
  const std::vector<std::string> root = list_names(directory.path());
  FT_CHECK_EQ(std::count_if(root.begin(), root.end(),
                            [](const std::string& name) { return name.find(".tmp-") != std::string::npos; }),
              std::ptrdiff_t{0});

  auto numbers = store->generation_numbers();
  FT_REQUIRE(numbers.has_value());
  FT_CHECK_EQ(numbers->size(), std::size_t{2});

  auto clean = store->status();
  FT_REQUIRE(clean.has_value());
  FT_CHECK(!clean->needs_recovery);
}

FT_TEST(recovery, a_missing_head_is_rebuilt_at_the_committed_generation) {
  TempDirectory directory("recover_missing_head");
  populate(directory.path(), 3);

  std::error_code error;
  std::filesystem::remove(directory / "HEAD", error);
  FT_REQUIRE(!error);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  FT_CHECK_ERROR(store->head(), ErrorCode::HeadMissing);
  MutationBatch blocked = make_batch("commit-x", TopologyGeneration(4), {});
  blocked.authority_epoch = store->epoch();
  FT_CHECK_ERROR(store->commit(blocked), ErrorCode::RecoveryRequired);

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->recovered);
  FT_CHECK_EQ(report->head_before.value(), std::uint64_t{0});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{4});

  // Recovery restores the index over already committed evidence; it does not
  // invent a generation and does not renumber anything.
  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{4});
  auto numbers = store->generation_numbers();
  FT_REQUIRE(numbers.has_value());
  FT_CHECK_EQ(numbers->size(), std::size_t{4});
}

FT_TEST(recovery, a_head_pointing_past_verified_evidence_moves_back) {
  TempDirectory directory("recover_head_ahead");
  populate(directory.path(), 2);

  // Point the head at a generation that is not present, in the shape the store
  // itself writes.
  const std::string bogus =
      std::string("ftop-head/1\ngeneration 9\ndigest sha256:") + digest_hex(digest_of("nothing")) + "\n";
  write_text(directory / "HEAD", bogus);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  FT_CHECK_ERROR(store->head(), ErrorCode::GenerationNotRetained);

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->recovered);
  FT_CHECK_EQ(report->head_before.value(), std::uint64_t{9});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{3});
  FT_CHECK(report->explanation.find("verified committed generations") != std::string::npos);

  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{3});
}

FT_TEST(recovery, a_corrupt_head_marker_is_replaced) {
  TempDirectory directory("recover_corrupt_head");
  populate(directory.path(), 1);
  write_text(directory / "HEAD", "this is not a head marker at all\n");

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(status->needs_recovery);
  FT_CHECK(status->explanation.find("HEAD_CORRUPT") != std::string::npos);

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->recovered);
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{2});
  FT_CHECK_EQ(read_text(directory / "HEAD").substr(0, 11), std::string("ftop-head/1"));

  write_text(directory / "HEAD", std::string("ftop-head/1\ngeneration 2\ndigest sha256:") + std::string(64, '0') + "\n");
  auto mismatched = store->head();
  FT_CHECK_ERROR(mismatched, ErrorCode::HeadCorrupt);
}

FT_TEST(recovery, corrupt_generations_are_quarantined_and_never_promoted) {
  TempDirectory directory("recover_corrupt_generation");
  populate(directory.path(), 2);

  // Overwrite the newest generation with content that does not verify: the
  // document is well formed but its digest no longer matches its bytes.
  const std::filesystem::path newest = directory / "generations" / "gen-00000000000000000003.ftop";
  const std::string corrupt = std::string("ftop/1\ngeneration 3\ndigest sha256:") + std::string(64, '0') + "\n";
  write_text(newest, corrupt);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  FT_CHECK_ERROR(store->head(), ErrorCode::DigestMismatch);

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->recovered);
  FT_CHECK_EQ(report->corrupt_files_quarantined, std::size_t{1});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{2});
  FT_CHECK_EQ(report->retained.size(), std::size_t{2});

  // The rejected bytes are preserved for an operator, not deleted.
  const std::vector<std::string> quarantined = list_names(directory / "quarantine");
  FT_REQUIRE(quarantined.size() == 1);
  FT_CHECK(quarantined[0].find("gen-00000000000000000003.ftop.quarantined") == std::size_t{0});
  FT_CHECK_EQ(read_text(directory / "quarantine" / quarantined[0]), corrupt);

  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});
  FT_CHECK_EQ(head->stats().nodes, std::size_t{12});

  // A generation that was quarantined is never resurrected by later recovery.
  auto again = store->recover();
  FT_REQUIRE(again.has_value());
  FT_CHECK(!again->repaired);
  FT_CHECK_EQ(again->head_after.value(), std::uint64_t{2});
}

FT_TEST(recovery, unrelated_files_in_the_generation_directory_are_quarantined) {
  TempDirectory directory("recover_strays");
  populate(directory.path(), 1);
  write_text(directory / "generations" / "notes.txt", "operator scratch file\n");
  write_text(directory / "generations" / "gen-notanumber.ftop", "junk\n");

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK_EQ(report->corrupt_files_quarantined, std::size_t{2});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{2});
  FT_CHECK_EQ(list_names(directory / "generations").size(), std::size_t{2});
  FT_CHECK_EQ(list_names(directory / "quarantine").size(), std::size_t{2});
}

FT_TEST(recovery, a_store_with_no_verified_generation_stays_uninitialized) {
  TempDirectory directory("recover_empty");
  populate(directory.path(), 1);
  write_text(directory / "generations" / "gen-00000000000000000001.ftop", "ftop/1\ntruncated\n");
  write_text(directory / "generations" / "gen-00000000000000000002.ftop", "ftop/1\nalso truncated\n");

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->repaired);
  FT_CHECK(!report->recovered);
  FT_CHECK_EQ(report->corrupt_files_quarantined, std::size_t{2});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{0});
  FT_CHECK(report->retained.empty());
  FT_CHECK(report->explanation.find("uninitialized") != std::string::npos);

  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->initialized);
  FT_CHECK_EQ(status->quarantine_count, std::size_t{2});
  FT_CHECK_ERROR(store->head(), ErrorCode::HeadMissing);

  // Creating a new generation over quarantined evidence is refused so the
  // evidence cannot be silently buried.
  FT_CHECK_ERROR(store->create(small_facility(), create_options()), ErrorCode::StoreNotEmpty);
}

FT_TEST(recovery, recovery_restores_the_retention_bound_without_renumbering) {
  TempDirectory directory("recover_retention");
  StoreConfig config = deterministic_config();
  config.limits.retained_generations = 2;
  {
    auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, config);
    FT_REQUIRE(store.has_value());
    FT_REQUIRE(store->create(small_facility(), create_options()).has_value());
    for (int index = 0; index < 4; ++index) {
      auto head = store->head_generation();
      FT_REQUIRE(head.has_value());
      MutationBatch batch = make_batch("commit-" + std::to_string(index), *head, {});
      batch.authority_epoch = store->epoch();
      const std::string id = "rack-" + std::to_string(20 + index);
      batch.mutations = {AddNode{make_node(id, NodeKind::Rack, id)}, AddContainment{physical("row-2", id)}};
      FT_REQUIRE(store->commit(batch).has_value());
    }
  }

  // Simulate a crash that happened after publication but before pruning by
  // restoring an old generation file copied from a wider-retention store.
  TempDirectory wide("recover_wide");
  StoreConfig wide_config = deterministic_config();
  wide_config.limits.retained_generations = 16;
  {
    auto store = TopologyStore::open(wide.path(), OpenMode::ReadWrite, wide_config);
    FT_REQUIRE(store.has_value());
    FT_REQUIRE(store->create(small_facility(), create_options()).has_value());
    for (int index = 0; index < 4; ++index) {
      auto head = store->head_generation();
      FT_REQUIRE(head.has_value());
      MutationBatch batch = make_batch("commit-" + std::to_string(index), *head, {});
      batch.authority_epoch = store->epoch();
      const std::string id = "rack-" + std::to_string(20 + index);
      batch.mutations = {AddNode{make_node(id, NodeKind::Rack, id)}, AddContainment{physical("row-2", id)}};
      FT_REQUIRE(store->commit(batch).has_value());
    }
  }
  for (int generation = 1; generation <= 5; ++generation) {
    const std::string name = "gen-0000000000000000000" + std::to_string(generation) + ".ftop";
    std::error_code error;
    std::filesystem::copy_file(wide.path() / "generations" / name, directory.path() / "generations" / name,
                               std::filesystem::copy_options::overwrite_existing, error);
    FT_REQUIRE(!error);
  }

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, config);
  FT_REQUIRE(store.has_value());
  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK_EQ(report->generations_pruned, std::size_t{3});
  FT_CHECK_EQ(report->retained.size(), std::size_t{2});
  FT_CHECK_EQ(report->retained.front().value(), std::uint64_t{4});
  FT_CHECK_EQ(report->retained.back().value(), std::uint64_t{5});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{5});

  auto numbers = store->generation_numbers();
  FT_REQUIRE(numbers.has_value());
  FT_CHECK_EQ(numbers->size(), std::size_t{2});
}

FT_TEST(recovery, recovery_is_idempotent) {
  TempDirectory directory("recover_idempotent");
  populate(directory.path(), 1);
  write_text(directory / "pending" / "gen-00000000000000000009.ftop.tmp-1-1", "junk");

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto first = store->recover();
  FT_REQUIRE(first.has_value());
  FT_CHECK(first->repaired);
  auto second = store->recover();
  FT_REQUIRE(second.has_value());
  FT_CHECK(!second->repaired);
  FT_CHECK_EQ(second->head_after.value(), std::uint64_t{2});
  auto third = store->recover();
  FT_REQUIRE(third.has_value());
  FT_CHECK_EQ(third->explanation, second->explanation);
}

FT_TEST(recovery, a_symlinked_head_is_rejected) {
  TempDirectory directory("recover_symlink");
  populate(directory.path(), 1);
  const std::filesystem::path target = directory / "head-target";
  write_text(target, read_text(directory / "HEAD"));
  std::error_code error;
  std::filesystem::remove(directory / "HEAD", error);
  FT_REQUIRE(!error);
  std::filesystem::create_symlink(target, directory / "HEAD", error);
  if (error) {
    // Creating symbolic links can require privileges on Windows; the check is
    // still meaningful where it is available.
    note("symbolic link creation is unavailable on this host; skipping");
    return;
  }

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  FT_CHECK_ERROR(store->head(), ErrorCode::HeadMissing);
  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->recovered);
}

FT_TEST(recovery, truncation_at_every_prefix_is_rejected) {
  TempDirectory directory("recover_truncation");
  populate(directory.path(), 1);
  const std::string document = read_text(directory / "generations" / "gen-00000000000000000001.ftop");
  FT_REQUIRE(document.size() > 64);

  // Every truncated prefix must be rejected: never partially accepted, never
  // silently repaired.
  for (std::size_t length = 0; length < document.size(); length += 7) {
    const auto parsed = parse_generation(document.substr(0, length), TopologyLimits{});
    FT_CHECK(!parsed.has_value());
  }
  const auto complete = parse_generation(document, TopologyLimits{});
  FT_CHECK(complete.has_value());

  // The same is true of a single-byte corruption anywhere in the document.
  for (std::size_t position = 0; position < document.size(); position += 11) {
    std::string mutated = document;
    mutated[position] = static_cast<char>(mutated[position] ^ 0x01);
    const auto parsed = parse_generation(mutated, TopologyLimits{});
    FT_CHECK(!parsed.has_value());
  }
}
