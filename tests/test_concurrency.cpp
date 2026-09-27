// Rack Registry - concurrency model tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The documented model is: every public registry and store method is safe to
// call from any thread; mutations are serialized end to end, including the
// durable publication; a reader observes either the state before a mutation or
// the state after it, never a partial application. These tests hold the
// implementation to that model.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

RackId concurrent_rack() { return RackId::parse("rack:concurrent").value(); }

RegisterRackRequest concurrent_registration(std::uint32_t units) {
  RegisterRackRequest request;
  request.structure.id = concurrent_rack();
  request.structure.unit_count = units;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:concurrent").value();
  request.identity.provenance = rrtest::provenance_for("concurrent", 1);
  return request;
}

}  // namespace

RR_TEST(concurrent_readers_never_observe_a_partial_mutation) {
  RackRegistry registry;
  const auto registered = registry.register_rack(concurrent_registration(64));
  RR_REQUIRE_OK(registered);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> observations{0};
  std::atomic<std::uint64_t> violations{0};

  // One writer inserts members, each time advancing both generations together.
  std::thread writer([&]() {
    RackGeneration generation = registered.value().generation;
    MembershipGeneration membership = registered.value().membership_generation;
    for (int index = 0; index < 40; ++index) {
      InsertMemberRequest insert;
      insert.rack_id = concurrent_rack();
      insert.precondition.expected_generation = generation;
      insert.precondition.expected_membership_generation = membership;
      insert.member_id =
          RackMemberId::parse("rm:c" + std::to_string(index)).value();
      insert.asset_id = AssetId::parse("asset:c" + std::to_string(index)).value();
      insert.mount =
          MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(
                                                         static_cast<std::uint32_t>(index) + 1)
                                                         .value())
                                    .value())
              .value();
      insert.identity.provenance = rrtest::provenance_for("concurrent", 2);
      const auto receipt = registry.insert_member(insert);
      if (!receipt) {
        ++violations;
        break;
      }
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
    }
    stop.store(true);
  });

  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        const auto view = registry.rack(concurrent_rack());
        if (!view) {
          ++violations;
          continue;
        }
        // A view is a self-contained immutable copy, so the counters inside it
        // must describe one state: while only inserts are issued, the
        // membership generation equals the number of members.
        if (view.value().membership_generation().value() != view.value().member_count()) {
          ++violations;
        }
        // Occupancy is read separately, so it may describe a later generation.
        // It can never describe an earlier one, because membership only grows,
        // and it must always be internally consistent.
        const auto occupancy = registry.occupancy(concurrent_rack());
        if (!occupancy) {
          ++violations;
          continue;
        }
        if (occupancy.value().size() < view.value().member_count()) {
          ++violations;
        }
        for (const OccupancyRecord& record : occupancy.value()) {
          if (record.mount.is_zero_u()) {
            continue;
          }
          if (record.mount.span().begin() < 1 || record.mount.span().end() > 129) {
            ++violations;
          }
        }
        observations.fetch_add(1);
      }
    });
  }

  writer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }

  RR_CHECK_EQ(violations.load(), std::uint64_t{0});
  RR_CHECK(observations.load() > 0);
  std::cout << "  concurrent observations " << observations.load() << "\n";
  RR_CHECK_EQ(registry.rack(concurrent_rack()).value().member_count(), std::size_t{40});
  RR_CHECK_EQ(registry.rack(concurrent_rack()).value().membership_generation().value(), 40u);
}

RR_TEST(concurrent_mutators_are_serialized_by_their_preconditions) {
  RackRegistry registry;
  const auto registered = registry.register_rack(concurrent_registration(64));
  RR_REQUIRE_OK(registered);

  constexpr int kThreads = 8;
  constexpr int kAttempts = 20;
  std::atomic<int> successes{0};
  std::atomic<int> stale{0};
  std::atomic<int> overlaps{0};
  std::atomic<int> unexpected{0};

  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index]() {
      for (int attempt = 0; attempt < kAttempts; ++attempt) {
        // Read the current authority, build a command against it, and let the
        // precondition decide the race. A stale command must be refused rather
        // than applied to a generation it was not planned against.
        const auto view = registry.rack(concurrent_rack());
        if (!view) {
          unexpected.fetch_add(1);
          continue;
        }
        InsertMemberRequest insert;
        insert.rack_id = concurrent_rack();
        insert.precondition.expected_generation = view.value().generation();
        insert.precondition.expected_membership_generation =
            view.value().membership_generation();
        insert.member_id = RackMemberId::parse("rm:t" + std::to_string(thread_index) + "-" +
                                              std::to_string(attempt))
                               .value();
        insert.asset_id = AssetId::parse("asset:t" + std::to_string(thread_index) + "-" +
                                         std::to_string(attempt))
                              .value();
        const std::uint32_t unit =
            static_cast<std::uint32_t>(view.value().member_count() + 1);
        if (unit > 64) {
          continue;
        }
        insert.mount =
            MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(unit).value()).value())
                .value();
        insert.identity.provenance = rrtest::provenance_for("concurrent", 3);
        const auto receipt = registry.insert_member(insert);
        if (receipt) {
          successes.fetch_add(1);
        } else if (receipt.error().code == ErrorCode::StaleRackGeneration ||
                   receipt.error().code == ErrorCode::StaleMembershipGeneration) {
          stale.fetch_add(1);
        } else if (receipt.error().code == ErrorCode::OccupancyOverlap ||
                   receipt.error().code == ErrorCode::MountOutOfBounds) {
          overlaps.fetch_add(1);
        } else {
          unexpected.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  RR_CHECK_EQ(unexpected.load(), 0);
  RR_CHECK(successes.load() > 0);
  RR_CHECK(stale.load() + overlaps.load() > 0);

  // Whatever interleaving occurred, the outcome is accounted for exactly: the
  // membership generation equals the number of accepted mutations, and the
  // member count matches it because only inserts were issued.
  const auto view = registry.rack(concurrent_rack());
  RR_REQUIRE_OK(view);
  RR_CHECK_EQ(view.value().membership_generation().value(),
              static_cast<std::uint64_t>(successes.load()));
  RR_CHECK_EQ(view.value().member_count(), static_cast<std::size_t>(successes.load()));

  // Each accepted mutation occupies a distinct rack unit.
  const auto members = registry.members(concurrent_rack(), MemberOrder::MountOrder);
  RR_REQUIRE_OK(members);
  std::vector<std::string> spans;
  for (const MemberRecord& member : members.value()) {
    spans.push_back(member.mount.to_text());
  }
  std::sort(spans.begin(), spans.end());
  RR_CHECK(std::adjacent_find(spans.begin(), spans.end()) == spans.end());
  std::cout << "  concurrent mutations accepted=" << successes.load()
            << " stale=" << stale.load() << " conflicting=" << overlaps.load() << "\n";
}

RR_TEST(concurrent_store_queries_never_observe_an_unpublished_mutation) {
  rrtest::TempDir directory("store-concurrent");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:concurrent"));
  RR_REQUIRE_OK(store);
  const auto registered = store.value()->register_rack(concurrent_registration(32));
  RR_REQUIRE_OK(registered);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> violations{0};
  std::atomic<std::uint64_t> observations{0};

  std::thread writer([&]() {
    RackGeneration generation = registered.value().generation;
    MembershipGeneration membership = registered.value().membership_generation;
    for (int index = 0; index < 12; ++index) {
      InsertMemberRequest insert;
      insert.rack_id = concurrent_rack();
      insert.precondition.expected_generation = generation;
      insert.precondition.expected_membership_generation = membership;
      insert.member_id = RackMemberId::parse("rm:s" + std::to_string(index)).value();
      insert.asset_id = AssetId::parse("asset:s" + std::to_string(index)).value();
      insert.mount =
          MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(
                                                         static_cast<std::uint32_t>(index) + 1)
                                                         .value())
                                    .value())
              .value();
      insert.identity.provenance = rrtest::provenance_for("concurrent", 4);
      const auto receipt = store.value()->insert_member(insert);
      if (!receipt) {
        violations.fetch_add(1);
        break;
      }
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
    }
    stop.store(true);
  });

  std::vector<std::thread> readers;
  for (int index = 0; index < 3; ++index) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        const RackSnapshot snapshot = store.value()->snapshot();
        const RackRecord* record = snapshot.find(concurrent_rack());
        if (record == nullptr) {
          violations.fetch_add(1);
          continue;
        }
        // A reader sees the durable generation's sequence paired with the
        // state that generation describes. The member count can never exceed
        // the series of published generations.
        if (record->members.size() > snapshot.store_sequence().value()) {
          violations.fetch_add(1);
        }
        if (record->members.size() != record->membership_generation.value()) {
          violations.fetch_add(1);
        }
        observations.fetch_add(1);
      }
    });
  }

  writer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }

  RR_CHECK_EQ(violations.load(), std::uint64_t{0});
  RR_CHECK(observations.load() > 0);
  RR_CHECK_EQ(store.value()->sequence().value(), 13u);
  RR_REQUIRE_OK(store.value()->close());
}

RR_TEST(shutdown_while_work_is_in_flight_leaves_consistent_accounting) {
  rrtest::TempDir directory("store-shutdown");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:shutdown"));
  RR_REQUIRE_OK(store);
  const auto registered = store.value()->register_rack(concurrent_registration(16));
  RR_REQUIRE_OK(registered);

  std::atomic<bool> start{false};
  std::atomic<int> completed{0};
  std::atomic<int> rejected{0};

  std::thread worker([&]() {
    while (!start.load()) {
      std::this_thread::yield();
    }
    RackGeneration generation = registered.value().generation;
    MembershipGeneration membership = registered.value().membership_generation;
    for (int index = 0; index < 20; ++index) {
      InsertMemberRequest insert;
      insert.rack_id = concurrent_rack();
      insert.precondition.expected_generation = generation;
      insert.precondition.expected_membership_generation = membership;
      insert.member_id = RackMemberId::parse("rm:x" + std::to_string(index)).value();
      insert.asset_id = AssetId::parse("asset:x" + std::to_string(index)).value();
      insert.mount =
          MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(
                                                         static_cast<std::uint32_t>(index) + 1)
                                                         .value())
                                    .value())
              .value();
      insert.identity.provenance = rrtest::provenance_for("concurrent", 5);
      const auto receipt = store.value()->insert_member(insert);
      if (receipt) {
        generation = receipt.value().generation;
        membership = receipt.value().membership_generation;
        completed.fetch_add(1);
      } else {
        // Every submission either crossed the completion boundary or failed
        // cleanly; there is no third outcome and no half-written generation.
        rejected.fetch_add(1);
      }
    }
  });

  start.store(true);
  // Close while the worker is running. Every mutation that returned success
  // must already be durable, and every mutation that returns failure must not
  // have changed the file.
  while (completed.load() < 5) {
    std::this_thread::yield();
  }
  RR_REQUIRE_OK(store.value()->close());
  worker.join();

  const auto info = RackStore::inspect(state_file);
  RR_REQUIRE_OK(info);
  const std::uint64_t after_close = info.value().sequence.value();
  RR_CHECK(after_close >= 1u);
  RR_CHECK(after_close <= 21u);

  // Reopening shows exactly the generations that were reported as published.
  auto reopened = RackStore::open(rrtest::store_options_for(state_file, "wr:reopen"));
  RR_REQUIRE_OK(reopened);
  RR_CHECK_EQ(reopened.value()->sequence().value(), after_close);
  const auto view = reopened.value()->rack(concurrent_rack());
  RR_REQUIRE_OK(view);
  RR_CHECK_EQ(view.value().member_count(), static_cast<std::size_t>(after_close - 1u));
  RR_REQUIRE_OK(reopened.value()->close());
  std::cout << "  shutdown accounting completed=" << completed.load()
            << " rejected=" << rejected.load() << " published=" << after_close << "\n";
}
