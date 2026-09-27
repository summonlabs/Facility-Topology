// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent-process proofs and inspection-tool integration.
//
// Every scenario in this file runs in a genuinely separate operating-system
// process: no threads pretending to be processes and no in-process simulation
// of a crash. A child reports its outcome through its exit status, and crash
// scenarios use the store's documented fault-injection seam so that the process
// stops at an exact point of the commit protocol.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "dccp/facility_topology/store.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

/// Exit status a child uses when it stops at an injected fault point.
constexpr int kFaultExit = 9;

/// Generations retained by the child processes, chosen so that the concurrent
/// scenario can assert on the complete generation list.
constexpr std::size_t kChildRetention = 64;

StoreConfig child_config(FaultPoint fault = FaultPoint::None) {
  StoreConfig config;
  config.fault_point = fault;
  config.limits.retained_generations = kChildRetention;
  return config;
}

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

CreateOptions child_create_options() {
  CreateOptions options;
  options.actor = actor_id("child");
  options.source = "test";
  options.reason = "independent process scenario";
  options.recorded_at = std::string(kFixtureTimestamp);
  options.mutation_id = mutation_id("child-create-1");
  return options;
}

/// Scenario body shared by every commit child.
int child_commit(const std::filesystem::path& directory, std::string_view identity, std::string_view rack,
                 FaultPoint fault) {
  auto store = TopologyStore::open(directory, OpenMode::ReadWrite, child_config(fault));
  if (!store.has_value()) {
    std::cout << "open-failed=" << error_code_name(store.error().code()) << "\n";
    return 1;
  }
  auto head = store->head_generation();
  if (!head.has_value()) {
    std::cout << "head-failed=" << error_code_name(head.error().code()) << "\n";
    return 1;
  }
  MutationBatch batch = make_batch(identity, *head, {});
  batch.authority_epoch = store->epoch();
  const std::string rack_id(rack);
  batch.mutations = {AddNode{make_node(rack_id, NodeKind::Rack, rack_id)},
                     AddContainment{physical("row-1", rack_id)}};
  auto outcome = store->commit(batch);
  if (!outcome.has_value()) {
    std::cout << "commit-failed=" << error_code_name(outcome.error().code()) << "\n";
    return 1;
  }
  std::cout << "committed=" << outcome->new_generation.value() << "\n";
  return 0;
}

std::size_t field_value(const std::string& output, std::string_view name) {
  const std::string needle = std::string(name) + "=";
  const std::size_t position = output.find(needle);
  if (position == std::string::npos) {
    return 0;
  }
  return static_cast<std::size_t>(std::strtoul(output.c_str() + position + needle.size(), nullptr, 10));
}

}  // namespace

/// Entry point for child scenarios. Returns -1 when the arguments do not name a
/// scenario, which the dispatcher turns into "not a child invocation".
int ftest_child_main(int argc, char** argv) {
  const std::vector<std::string> arguments(argv + 2, argv + argc);
  if (arguments.empty()) {
    std::cout << "child-error=missing-scenario\n";
    return 2;
  }
  const std::string& scenario = arguments[0];

  if (scenario == "create" && arguments.size() == 2) {
    auto store = TopologyStore::open(arguments[1], OpenMode::ReadWrite, child_config());
    if (!store.has_value()) {
      std::cout << "open-failed=" << error_code_name(store.error().code()) << "\n";
      return 1;
    }
    auto outcome = store->create(small_facility(), child_create_options());
    if (!outcome.has_value()) {
      std::cout << "create-failed=" << error_code_name(outcome.error().code()) << "\n";
      return 1;
    }
    std::cout << "created=" << outcome->new_generation.value() << "\n";
    return 0;
  }
  if (scenario == "commit" && arguments.size() == 4) {
    return child_commit(arguments[1], arguments[2], arguments[3], FaultPoint::None);
  }
  if (scenario == "commit-fault" && arguments.size() == 5) {
    FaultPoint point = FaultPoint::None;
    const std::string& token = arguments[4];
    if (token == "after-pending-write") {
      point = FaultPoint::AfterPendingWrite;
    } else if (token == "after-generation-commit") {
      point = FaultPoint::AfterGenerationCommit;
    } else if (token == "after-head-replace") {
      point = FaultPoint::AfterHeadReplace;
    } else {
      std::cout << "child-error=unknown-fault-point\n";
      return 2;
    }
    return child_commit(arguments[1], arguments[2], arguments[3], point);
  }
  if (scenario == "commit-loop" && arguments.size() == 4) {
    const int attempts = std::atoi(arguments[2].c_str());
    const std::string tag = arguments[3];
    int accepted = 0;
    int fenced = 0;
    int other = 0;
    for (int index = 0; index < attempts; ++index) {
      auto store = TopologyStore::open(arguments[1], OpenMode::ReadWrite, child_config());
      if (!store.has_value()) {
        ++fenced;
        continue;
      }
      auto head = store->head_generation();
      if (!head.has_value()) {
        ++other;
        continue;
      }
      MutationBatch batch = make_batch("loop-" + tag + "-" + std::to_string(index), *head, {});
      batch.authority_epoch = store->epoch();
      const std::string rack_id = "loop-rack-" + tag + "-" + std::to_string(index);
      batch.mutations = {AddNode{make_node(rack_id, NodeKind::Rack, rack_id)},
                         AddContainment{physical("row-1", rack_id)}};
      auto outcome = store->commit(batch);
      if (outcome.has_value()) {
        ++accepted;
        continue;
      }
      const ErrorCode code = outcome.error().code();
      if (code == ErrorCode::StaleAuthorityEpoch || code == ErrorCode::StaleBaseGeneration ||
          code == ErrorCode::StoreLocked || code == ErrorCode::BatchRejected) {
        ++fenced;
      } else {
        ++other;
      }
    }
    std::cout << "accepted=" << accepted << " fenced=" << fenced << " other=" << other << "\n";
    return other == 0 ? 0 : 1;
  }
  if (scenario == "epoch" && arguments.size() == 2) {
    auto store = TopologyStore::open(arguments[1], OpenMode::ReadWrite, child_config());
    if (!store.has_value()) {
      std::cout << "open-failed=" << error_code_name(store.error().code()) << "\n";
      return 1;
    }
    std::cout << "epoch=" << store->epoch().value() << "\n";
    return 0;
  }
  std::cout << "child-error=unknown-scenario\n";
  return 2;
}

namespace ftest {

int child_scenarios_main(int argc, char** argv) { return ftest_child_main(argc, argv); }

}  // namespace ftest

// ---------------------------------------------------------------------------
// Crash / partial write recovery
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, an_independent_process_creates_a_store) {
  TempDirectory directory("mp_create");
  const ChildOutcome outcome = run_child({"--ftop-child", "create", directory.path().string()});
  FT_CHECK_EQ(outcome.exit_code, 0);
  FT_CHECK(outcome.output.find("created=1") != std::string::npos);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadOnly, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{1});
  FT_CHECK(head->same_structure_as(small_facility()));
}

FT_TEST(multiprocess, a_crash_before_the_commit_point_publishes_nothing) {
  TempDirectory directory("mp_crash_pending");
  FT_REQUIRE(run_child({"--ftop-child", "create", directory.path().string()}).exit_code == 0);

  const ChildOutcome crashed = run_child({"--ftop-child", "commit-fault", directory.path().string(), "crash-1",
                                          "crash-rack-1", "after-pending-write"});
  FT_CHECK_EQ(crashed.exit_code, kFaultExit);

  // The store is intact and reports the interrupted commit.
  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(status->needs_recovery);
  FT_CHECK_EQ(status->pending_count, std::size_t{1});
  FT_CHECK_EQ(status->max_committed.value(), std::uint64_t{1});

  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{1});
  FT_CHECK(!head->contains_node(node_id("crash-rack-1")));

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->repaired);
  FT_CHECK(!report->recovered);
  FT_CHECK_EQ(report->pending_files_discarded, std::size_t{1});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{1});

  auto clean = store->status();
  FT_REQUIRE(clean.has_value());
  FT_CHECK(!clean->needs_recovery);

  // The unpublished work never became authoritative, and the same mutation
  // identity is still free to be retried by a new process.
  const ChildOutcome retry = run_child({"--ftop-child", "commit", directory.path().string(), "crash-1", "crash-rack-1"});
  FT_CHECK_EQ(retry.exit_code, 0);
  auto after = store->head();
  FT_REQUIRE(after.has_value());
  FT_CHECK_EQ(after->generation().value(), std::uint64_t{2});
  FT_CHECK(after->contains_node(node_id("crash-rack-1")));
}

FT_TEST(multiprocess, a_crash_after_the_commit_point_is_recovered_at_its_own_generation) {
  TempDirectory directory("mp_crash_committed");
  FT_REQUIRE(run_child({"--ftop-child", "create", directory.path().string()}).exit_code == 0);

  const ChildOutcome crashed = run_child({"--ftop-child", "commit-fault", directory.path().string(), "crash-2",
                                          "crash-rack-2", "after-generation-commit"});
  FT_CHECK_EQ(crashed.exit_code, kFaultExit);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(status->needs_recovery);
  FT_CHECK_EQ(status->max_committed.value(), std::uint64_t{2});
  FT_CHECK_EQ(status->head.value(), std::uint64_t{1});
  FT_CHECK_EQ(status->pending_count, std::size_t{0});

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(report->recovered);
  FT_CHECK_EQ(report->head_before.value(), std::uint64_t{1});
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{2});

  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});
  FT_CHECK(head->contains_node(node_id("crash-rack-2")));

  // A commit based on the recovered generation is accepted.
  const ChildOutcome next = run_child({"--ftop-child", "commit", directory.path().string(), "after-recovery", "rack-3"});
  FT_CHECK_EQ(next.exit_code, 0);
  auto final_head = store->head();
  FT_REQUIRE(final_head.has_value());
  FT_CHECK_EQ(final_head->generation().value(), std::uint64_t{3});
}

FT_TEST(multiprocess, a_crash_after_the_head_replace_needs_no_recovery) {
  TempDirectory directory("mp_crash_head");
  FT_REQUIRE(run_child({"--ftop-child", "create", directory.path().string()}).exit_code == 0);

  const ChildOutcome crashed = run_child({"--ftop-child", "commit-fault", directory.path().string(), "crash-3",
                                          "crash-rack-3", "after-head-replace"});
  FT_CHECK_EQ(crashed.exit_code, kFaultExit);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->needs_recovery);
  FT_CHECK_EQ(status->head.value(), std::uint64_t{2});

  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{2});
  FT_CHECK(head->contains_node(node_id("crash-rack-3")));

  auto report = store->recover();
  FT_REQUIRE(report.has_value());
  FT_CHECK(!report->repaired);
  FT_CHECK_EQ(report->head_after.value(), std::uint64_t{2});
}

// ---------------------------------------------------------------------------
// Multi-process authority
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, a_stale_writer_in_this_process_is_fenced_by_another_process) {
  TempDirectory directory("mp_fencing");
  FT_REQUIRE(run_child({"--ftop-child", "create", directory.path().string()}).exit_code == 0);

  // This process takes mutation authority first.
  auto local = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(local.has_value());
  const WriterEpoch local_epoch = local->epoch();
  FT_CHECK(local_epoch.valid());

  // A separate process opens the same store, which advances the epoch.
  const ChildOutcome child = run_child({"--ftop-child", "commit", directory.path().string(), "child-commit",
                                        "child-rack"});
  FT_CHECK_EQ(child.exit_code, 0);
  FT_CHECK(child.output.find("committed=2") != std::string::npos);

  auto child_epoch = TopologyStore::open(directory.path(), OpenMode::ReadOnly, deterministic_config());
  FT_REQUIRE(child_epoch.has_value());

  // The local handle's authority is now stale: its commit must be refused even
  // though the generation it was based on still exists.
  MutationBatch batch = make_batch("stale-writer", TopologyGeneration(1), {});
  batch.authority_epoch = local_epoch;
  batch.mutations = {AddNode{make_node("stale-rack", NodeKind::Rack, "stale-rack")}};
  FT_CHECK_ERROR(local->commit(batch), ErrorCode::StaleAuthorityEpoch);

  // Reopening grants fresh authority and the same work then succeeds.
  auto reopened = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(reopened.has_value());
  FT_CHECK(reopened->epoch() > local_epoch);
  auto head = reopened->head_generation();
  FT_REQUIRE(head.has_value());
  MutationBatch retry = make_batch("stale-writer", *head, {});
  retry.authority_epoch = reopened->epoch();
  retry.mutations = {AddNode{make_node("stale-rack", NodeKind::Rack, "stale-rack")},
                     AddContainment{physical("row-1", "stale-rack")}};
  auto outcome = reopened->commit(retry);
  FT_REQUIRE(outcome.has_value());
  FT_CHECK_EQ(outcome->new_generation.value(), std::uint64_t{3});
}

FT_TEST(multiprocess, concurrent_committers_never_tear_the_store) {
  TempDirectory directory("mp_concurrent");
  FT_REQUIRE(run_child({"--ftop-child", "create", directory.path().string()}).exit_code == 0);

  // Two independent processes hammer the same store at the same time. Every
  // attempt must end in a completed commit or a deterministic rejection.
  std::vector<ChildOutcome> outcomes;
  outcomes.push_back(run_child({"--ftop-child", "commit-loop", directory.path().string(), "12", "a"}));
  outcomes.push_back(run_child({"--ftop-child", "commit-loop", directory.path().string(), "12", "b"}));

  std::size_t total_accepted = 0;
  for (const ChildOutcome& outcome : outcomes) {
    FT_CHECK_EQ(outcome.exit_code, 0);
    FT_CHECK(outcome.output.find("other=0") != std::string::npos);
    const std::size_t accepted = field_value(outcome.output, "accepted");
    const std::size_t fenced = field_value(outcome.output, "fenced");
    total_accepted += accepted;
    FT_CHECK_EQ(accepted + fenced, std::size_t{12});
  }
  FT_CHECK(total_accepted > 0);

  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, child_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->needs_recovery);
  FT_CHECK_EQ(status->pending_count, std::size_t{0});
  FT_CHECK_EQ(status->quarantine_count, std::size_t{0});

  // Exactly one additional rack per accepted commit, and the head verifies.
  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), static_cast<std::uint64_t>(total_accepted) + 1);
  FT_CHECK_EQ(head->stats().nodes, static_cast<std::size_t>(11 + total_accepted));

  auto numbers = store->generation_numbers();
  FT_REQUIRE(numbers.has_value());
  FT_CHECK_EQ(numbers->back().value(), static_cast<std::uint64_t>(total_accepted) + 1);
  FT_CHECK(numbers->size() == std::min<std::size_t>(total_accepted + 1, kChildRetention));
}

FT_TEST(multiprocess, repeated_open_and_close_from_many_processes_leaves_a_clean_store) {
  TempDirectory directory("mp_reopen");
  FT_REQUIRE(run_child({"--ftop-child", "create", directory.path().string()}).exit_code == 0);
  for (int iteration = 0; iteration < 6; ++iteration) {
    const ChildOutcome outcome =
        run_child({"--ftop-child", "commit", directory.path().string(), "round-" + std::to_string(iteration),
                   "round-rack-" + std::to_string(iteration)});
    FT_CHECK_EQ(outcome.exit_code, 0);
  }
  auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(store.has_value());
  auto status = store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->needs_recovery);
  FT_CHECK_EQ(status->generation_count, std::size_t{7});
  auto head = store->head();
  FT_REQUIRE(head.has_value());
  FT_CHECK_EQ(head->generation().value(), std::uint64_t{7});
  FT_CHECK_EQ(head->stats().nodes, std::size_t{17});
}

// ---------------------------------------------------------------------------
// Inspection tool
// ---------------------------------------------------------------------------

#ifdef FACILITY_TOPOLOGY_CLI_PATH

namespace {

const std::filesystem::path kCliPath = FACILITY_TOPOLOGY_CLI_PATH;

ChildOutcome cli(const std::vector<std::string>& arguments) { return run_program(kCliPath, arguments); }

bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

FT_TEST(multiprocess, cli_reports_version_and_usage) {
  const ChildOutcome version = cli({"version"});
  FT_CHECK_EQ(version.exit_code, 0);
  FT_CHECK(contains(version.output, "library-version=1.0.0"));
  FT_CHECK(contains(version.output, "canonical-banner=ftop/1"));
  FT_CHECK(contains(version.output, "node-kinds=facility,building,hall,room,row,rack,zone"));

  const ChildOutcome help = cli({"help"});
  FT_CHECK_EQ(help.exit_code, 0);
  FT_CHECK(contains(help.output, "usage: ftopctl"));

  const ChildOutcome unknown = cli({"frobnicate"});
  FT_CHECK_EQ(unknown.exit_code, 2);
  FT_CHECK(contains(unknown.output, "unknown command"));
}

FT_TEST(multiprocess, cli_creates_inspects_and_evolves_a_store) {
  TempDirectory directory("cli_flow");
  const std::string store_path = directory.path().string();

  const ChildOutcome demo = cli({"demo", store_path, "--racks", "2"});
  FT_CHECK_EQ(demo.exit_code, 0);
  FT_CHECK(contains(demo.output, "generation=1"));
  FT_CHECK(contains(demo.output, "topology=synthetic"));

  const ChildOutcome status = cli({"status", store_path});
  FT_CHECK_EQ(status.exit_code, 0);
  FT_CHECK(contains(status.output, "initialized=yes"));
  FT_CHECK(contains(status.output, "head=1"));
  FT_CHECK(contains(status.output, "needs-recovery=no"));

  const ChildOutcome show = cli({"show", store_path});
  FT_CHECK_EQ(show.exit_code, 0);
  FT_CHECK(contains(show.output, "generation=1"));
  FT_CHECK(contains(show.output, "content-digest=sha256:"));

  const ChildOutcome generations = cli({"generations", store_path});
  FT_CHECK_EQ(generations.exit_code, 0);
  FT_CHECK(contains(generations.output, "generation=1"));

  const ChildOutcome nodes = cli({"nodes", store_path, "--kind", "rack"});
  FT_CHECK_EQ(nodes.exit_code, 0);
  FT_CHECK(contains(nodes.output, "kind=rack"));

  const ChildOutcome node = cli({"node", store_path, "--id", "bld-1-hall-1-room-row-1-rack-1"});
  FT_CHECK_EQ(node.exit_code, 0);
  FT_CHECK(contains(node.output, "kind=rack"));
  FT_CHECK(contains(node.output, "physical-parent=bld-1-hall-1-room-row-1"));
  FT_CHECK(contains(node.output, "facility=fac-demo"));

  const ChildOutcome ancestry = cli({"ancestry", store_path, "--id", "bld-1-hall-1-room-row-1-rack-1"});
  FT_CHECK_EQ(ancestry.exit_code, 0);
  FT_CHECK(contains(ancestry.output, "ancestor=fac-demo depth=0"));
  FT_CHECK(contains(ancestry.output, "ancestor=bld-1 depth=1"));

  const ChildOutcome descendants = cli({"descendants", store_path, "--id", "fac-demo", "--order", "dfs"});
  FT_CHECK_EQ(descendants.exit_code, 0);
  FT_CHECK(contains(descendants.output, "order=dfs"));

  const ChildOutcome neighbors = cli({"neighbors", store_path, "--id", "bld-1-hall-1-room-row-1-rack-1"});
  FT_CHECK_EQ(neighbors.exit_code, 0);
  FT_CHECK(contains(neighbors.output, "neighbor=bld-1-hall-1-room-row-1-rack-2"));

  const ChildOutcome domain = cli({"domain", store_path, "--id", "pwr-a"});
  FT_CHECK_EQ(domain.exit_code, 0);
  FT_CHECK(contains(domain.output, "declared=yes"));
  FT_CHECK(contains(domain.output, "member=bld-1-hall-1-room-row-1-rack-1"));

  // A real mutation through the normal authority path.
  const std::filesystem::path script = directory / "mutation.ftopscript";
  write_text(script, "add-node cli-rack-1 rack \"CLI rack\"\n"
                     "add-containment bld-1-hall-1-room-row-1 cli-rack-1\n");
  const ChildOutcome applied = cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-1", "--base", "1",
                                    "--script", script.string()});
  FT_CHECK_EQ(applied.exit_code, 0);
  FT_CHECK(contains(applied.output, "published=yes"));

  // Repeating the identical request is a replay: the identity is tied to the
  // request content, so a retry names the same base as the original attempt.
  const ChildOutcome replayed = cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-1", "--base", "1",
                                     "--script", script.string()});
  FT_CHECK_EQ(replayed.exit_code, 0);
  FT_CHECK(contains(replayed.output, "replayed=yes"));

  // The same identity with different content is a conflict, not a replay.
  const std::filesystem::path second = directory / "mutation2.ftopscript";
  write_text(second, "add-node cli-rack-2 rack \"CLI rack 2\"\n"
                     "add-containment bld-1-hall-1-room-row-1 cli-rack-2\n");
  const ChildOutcome conflicting =
      cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-1", "--base", "1", "--script", second.string()});
  FT_CHECK_EQ(conflicting.exit_code, 1);
  FT_CHECK(contains(conflicting.output, "error-code=IDENTITY_CONFLICT"));

  const ChildOutcome applied_second =
      cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-2", "--script", second.string()});
  FT_CHECK_EQ(applied_second.exit_code, 0);

  const ChildOutcome changes = cli({"diff", store_path, "--from", "1", "--to", "2"});
  FT_CHECK_EQ(changes.exit_code, 0);
  FT_CHECK(contains(changes.output, "nodes-added 1 cli-rack-1"));

  const std::filesystem::path exported = directory / "exported.ftop";
  const ChildOutcome export_result = cli({"export", store_path, "--out", exported.string()});
  FT_CHECK_EQ(export_result.exit_code, 0);

  const ChildOutcome validated = cli({"validate", exported.string()});
  FT_CHECK_EQ(validated.exit_code, 0);
  FT_CHECK(contains(validated.output, "valid=yes"));
  FT_CHECK(contains(validated.output, "generation=3"));

  const ChildOutcome recovered = cli({"recover", store_path});
  FT_CHECK_EQ(recovered.exit_code, 0);
  FT_CHECK(contains(recovered.output, "repaired=no"));
}

FT_TEST(multiprocess, cli_rejections_are_stable_and_explained) {
  TempDirectory directory("cli_rejections");
  const std::string store_path = directory.path().string();

  const ChildOutcome missing = cli({"status", (directory / "absent").string()});
  FT_CHECK_EQ(missing.exit_code, 1);
  FT_CHECK(contains(missing.output, "error-code=STORE_NOT_FOUND"));

  const ChildOutcome usage = cli({"apply", store_path, "--actor", "cli"});
  FT_CHECK_EQ(usage.exit_code, 1);

  FT_REQUIRE(cli({"demo", store_path}).exit_code == 0);

  // A mutation without an identity is refused rather than invented.
  const ChildOutcome no_identity = cli({"apply", store_path, "--actor", "cli"});
  FT_CHECK_EQ(no_identity.exit_code, 1);
  FT_CHECK(contains(no_identity.output, "error-code=MISSING_FIELD"));

  // A stale base generation is refused deterministically.
  const ChildOutcome stale = cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-stale", "--base", "9"});
  FT_CHECK_EQ(stale.exit_code, 1);
  FT_CHECK(contains(stale.output, "error-code=STALE_BASE_GENERATION"));

  // A script that would violate the schema is refused with the library's code.
  const std::filesystem::path script = directory / "bad.ftopscript";
  write_text(script, "add-node bad-rack rack \"Bad\"\n"
                     "add-containment bld-1-hall-1-room-row-1-rack-1 bad-rack\n");
  const ChildOutcome invalid = cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-bad", "--script",
                                    script.string()});
  FT_CHECK_EQ(invalid.exit_code, 1);
  FT_CHECK(contains(invalid.output, "error-code="));

  // A malformed script is refused with its line number.
  const std::filesystem::path malformed = directory / "malformed.ftopscript";
  write_text(malformed, "nonsense command here\n");
  const ChildOutcome malformed_result =
      cli({"apply", store_path, "--actor", "cli", "--mutation-id", "cli-malformed", "--script", malformed.string()});
  FT_CHECK_EQ(malformed_result.exit_code, 1);
  FT_CHECK(contains(malformed_result.output, "error-code=MALFORMED_RECORD"));

  // A corrupt document is rejected by the digest.
  const std::filesystem::path exported = directory / "exported.ftop";
  FT_REQUIRE(cli({"export", store_path, "--out", exported.string()}).exit_code == 0);
  std::string tampered = read_text(exported);
  FT_REQUIRE(tampered.size() > 40);
  tampered[30] = tampered[30] == 'x' ? 'y' : 'x';
  const std::filesystem::path broken = directory / "broken.ftop";
  write_text(broken, tampered);
  const ChildOutcome corrupt = cli({"validate", broken.string()});
  FT_CHECK_EQ(corrupt.exit_code, 1);
  FT_CHECK(contains(corrupt.output, "error-code=DIGEST_MISMATCH"));

  // The store is untouched by every rejection above.
  const ChildOutcome status = cli({"status", store_path});
  FT_CHECK_EQ(status.exit_code, 0);
  FT_CHECK(contains(status.output, "head=1"));
  FT_CHECK(contains(status.output, "pending=0"));
}

#endif  // FACILITY_TOPOLOGY_CLI_PATH
