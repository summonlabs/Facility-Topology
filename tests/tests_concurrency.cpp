// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency model tests.
//
// The documented model is: immutable snapshots are shared between threads,
// publication is a single atomic store guarded by a non-blocking single-writer
// flag, and a durable store handle belongs to one thread. These tests exercise
// each of those claims rather than assuming them.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "dccp/facility_topology/store.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

FT_TEST(concurrency, published_topology_serves_consistent_readers) {
  PublishedTopology published;
  FT_CHECK(published.current() == nullptr);
  // An empty snapshot holds no topology and is refused rather than published.
  FT_CHECK(!published.publish(TopologySnapshot{}));
  FT_CHECK(published.current() == nullptr);
  FT_CHECK_EQ(published.publish_count(), std::uint64_t{0});

  const TopologySnapshot base = small_facility(TopologyGeneration(1));
  FT_CHECK(published.publish(base));
  FT_CHECK_EQ(published.publish_count(), std::uint64_t{1});
  FT_CHECK(published.current() != nullptr);
  FT_CHECK_EQ(published.current()->generation().value(), std::uint64_t{1});

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> inconsistencies{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&published, &stop, &reads, &inconsistencies]() {
      // Every reader observes at least once before it may stop, so the test
      // does not depend on how quickly the publisher finishes.
      do {
        const std::shared_ptr<const TopologySnapshot> snapshot = published.current();
        if (snapshot == nullptr) {
          inconsistencies.fetch_add(1);
          continue;
        }
        // A published snapshot is immutable, so every observation of the same
        // generation must describe exactly the same structure.
        const TopologyStats stats = snapshot->stats();
        const std::uint64_t generation = snapshot->generation().value();
        if (stats.nodes != 10 + generation || stats.adjacency != 3 ||
            stats.physical_containment != 9 + generation || stats.facilities != 1) {
          inconsistencies.fetch_add(1);
        }
        const std::shared_ptr<const TopologySnapshot> again = published.current();
        if (again != nullptr && again->generation() == snapshot->generation() &&
            again->content_digest() != snapshot->content_digest()) {
          inconsistencies.fetch_add(1);
        }
        reads.fetch_add(1);
      } while (!stop.load());
    });
  }

  TopologySnapshot current = base;
  std::uint64_t published_count = 1;
  for (int generation = 2; generation <= 40; ++generation) {
    TopologyBuilder builder = TopologyBuilder::from_snapshot(current).value();
    const std::string id = "rack-" + std::to_string(generation);
    FT_REQUIRE(builder.add_node(make_node(id, NodeKind::Rack, id)).has_value());
    FT_REQUIRE(builder.add_containment(physical("row-2", id)).has_value());
    auto next = builder.build(TopologyGeneration(static_cast<std::uint64_t>(generation)));
    FT_REQUIRE(next.has_value());
    current = *next;
    for (int attempt = 0; attempt < 1000; ++attempt) {
      if (published.publish(current)) {
        ++published_count;
        break;
      }
      std::this_thread::yield();
    }
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }

  FT_CHECK_EQ(inconsistencies.load(), std::uint64_t{0});
  FT_CHECK(reads.load() > 0);
  FT_CHECK_EQ(published.publish_count(), published_count);
  const std::shared_ptr<const TopologySnapshot> final_snapshot = published.current();
  FT_REQUIRE(final_snapshot != nullptr);
  FT_CHECK_EQ(final_snapshot->generation().value(), std::uint64_t{40});
  FT_CHECK_EQ(final_snapshot->stats().nodes, std::size_t{50});
}

FT_TEST(concurrency, concurrent_publishers_are_refused_rather_than_blocked) {
  PublishedTopology published;
  const TopologySnapshot first = small_facility(TopologyGeneration(1));
  FT_CHECK(published.publish(first));

  std::atomic<int> successes{0};
  std::atomic<int> refusals{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([&published, &first, &successes, &refusals]() {
      for (int attempt = 0; attempt < 200; ++attempt) {
        if (published.publish(first)) {
          successes.fetch_add(1);
        } else {
          refusals.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FT_CHECK_EQ(successes.load() + refusals.load(), 800);
  FT_CHECK_EQ(published.publish_count(), static_cast<std::uint64_t>(successes.load()) + 1);
  FT_CHECK(published.current() != nullptr);
}

FT_TEST(concurrency, concurrent_read_only_handles_observe_the_same_generation) {
  TempDirectory directory("concurrency_readers");
  {
    auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
    FT_REQUIRE(store.has_value());
    CreateOptions options;
    options.actor = actor_id("operator");
    options.source = "test";
    options.mutation_id = mutation_id("create-1");
    FT_REQUIRE(store->create(small_facility(), options).has_value());
  }

  std::atomic<std::uint64_t> failures{0};
  std::atomic<std::uint64_t> reads{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&directory, &failures, &reads]() {
      auto store = TopologyStore::open(directory.path(), OpenMode::ReadOnly, deterministic_config());
      if (!store.has_value()) {
        failures.fetch_add(1);
        return;
      }
      for (int attempt = 0; attempt < 20; ++attempt) {
        auto head = store->head();
        if (!head.has_value()) {
          failures.fetch_add(1);
          return;
        }
        if (head->stats().nodes != 11 || head->generation().value() != 1) {
          failures.fetch_add(1);
          return;
        }
        reads.fetch_add(1);
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  FT_CHECK_EQ(failures.load(), std::uint64_t{0});
  FT_CHECK_EQ(reads.load(), std::uint64_t{80});
}

FT_TEST(concurrency, a_shared_read_write_handle_is_serialized_by_the_caller) {
  // Two independent handles in one process still fence each other through the
  // durable epoch, which is exactly the cross-process behaviour.
  TempDirectory directory("concurrency_fencing");
  auto first = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(first.has_value());
  CreateOptions options;
  options.actor = actor_id("operator");
  options.source = "test";
  options.mutation_id = mutation_id("create-1");
  FT_REQUIRE(first->create(small_facility(), options).has_value());

  std::atomic<int> accepted{0};
  std::atomic<int> fenced{0};
  const auto attempt = [&directory, &accepted, &fenced](int index) {
    auto store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
    if (!store.has_value()) {
      fenced.fetch_add(1);
      return;
    }
    auto head = store->head_generation();
    if (!head.has_value()) {
      fenced.fetch_add(1);
      return;
    }
    MutationBatch batch = make_batch("thread-" + std::to_string(index), *head, {});
    batch.authority_epoch = store->epoch();
    const std::string id = "rack-" + std::to_string(100 + index);
    batch.mutations = {AddNode{make_node(id, NodeKind::Rack, id)}, AddContainment{physical("row-2", id)}};
    const auto outcome = store->commit(batch);
    if (outcome.has_value()) {
      accepted.fetch_add(1);
    } else {
      // Losing the race is a deterministic rejection, never a partial write.
      const ErrorCode code = outcome.error().code();
      if (code == ErrorCode::StaleAuthorityEpoch || code == ErrorCode::StaleBaseGeneration ||
          code == ErrorCode::BatchRejected || code == ErrorCode::IdentityConflict) {
        fenced.fetch_add(1);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back(attempt, index);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  FT_CHECK_EQ(accepted.load() + fenced.load(), 4);
  FT_CHECK(accepted.load() >= 1);
  auto final_store = TopologyStore::open(directory.path(), OpenMode::ReadWrite, deterministic_config());
  FT_REQUIRE(final_store.has_value());
  auto status = final_store->status();
  FT_REQUIRE(status.has_value());
  FT_CHECK(!status->needs_recovery);
  FT_CHECK_EQ(status->pending_count, std::size_t{0});
  auto head = final_store->head();
  FT_REQUIRE(head.has_value());
  // Every accepted commit added exactly one rack and nothing was torn.
  FT_CHECK_EQ(head->stats().nodes, static_cast<std::size_t>(11 + accepted.load()));
}
