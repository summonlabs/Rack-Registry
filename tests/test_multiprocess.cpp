// Rack Registry - multiprocess writer authority and fencing tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every test in this file starts real operating-system processes that compete
// for one state file. A single-process test cannot prove writer fencing: the
// property being proven is precisely that authority survives, and is lost,
// across process boundaries.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

constexpr std::string_view kFenceChild = "rr_fence_child";
constexpr int kExitHeld = 3;
constexpr int kExitFenced = 4;

RegisterRackRequest multiprocess_registration() {
  RegisterRackRequest request;
  request.structure.id = RackId::parse("rack:mp").value();
  request.structure.unit_count = 8;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:mp").value();
  request.identity.provenance = rrtest::provenance_for("multiprocess", 1);
  return request;
}

void prepare(const std::filesystem::path& state_file) {
  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:setup"));
  if (!store) {
    return;
  }
  (void)store.value()->register_rack(multiprocess_registration());
  (void)store.value()->close();
}

std::string read_marker(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string text;
  if (stream.good()) {
    std::getline(stream, text);
  }
  return text;
}

void write_file(const std::filesystem::path& path, std::string_view text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
}

}  // namespace

RR_TEST(a_live_writer_excludes_a_second_process) {
  rrtest::TempDir directory("mp-exclude");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  prepare(state_file);

  const std::filesystem::path marker = directory.file("held.txt");
  const std::filesystem::path hold_file = directory.file("hold.txt");
  write_file(hold_file, "hold");

  rrtest::ChildProcess holder =
      rrtest::ChildProcess::spawn(rrtest::helper_executable(kFenceChild),
                                  {"hold", state_file.string(), "wr:holder", marker.string(),
                                   hold_file.string()},
                                  directory.file("holder.out"), directory.file("holder.err"));
  RR_REQUIRE(holder.started());
  RR_REQUIRE(rrtest::wait_for_file(marker, 30000));
  const std::string held_epoch = read_marker(marker);
  RR_CHECK(!held_epoch.empty());

  // A second process cannot take authority while the first is alive.
  const rrtest::ProcessResult blocked =
      rrtest::run_process(rrtest::helper_executable(kFenceChild),
                          {"try-open", state_file.string(), "wr:challenger"});
  RR_CHECK(blocked.started);
  RR_CHECK_EQ(blocked.exit_code, kExitHeld);
  RR_CHECK(blocked.standard_output.find("held") != std::string::npos);

  // The holder still owns the store and the state is untouched.
  const auto lock = RackStore::query_writer_lock(state_file);
  RR_REQUIRE_OK(lock);
  RR_CHECK(lock.value().state == WriterLockState::HeldByAnotherProcess);
  RR_CHECK_EQ(lock.value().writer_id.text(), std::string("wr:holder"));

  std::error_code error;
  std::filesystem::remove(hold_file, error);
  RR_CHECK_EQ(holder.wait(), 0);

  // After a clean release the next process takes authority at a higher epoch.
  const rrtest::ProcessResult adopted =
      rrtest::run_process(rrtest::helper_executable(kFenceChild),
                          {"adopt", state_file.string(), "wr:successor"});
  RR_CHECK(adopted.started);
  RR_CHECK_EQ(adopted.exit_code, 0);
  RR_CHECK(adopted.standard_output.find("adopted epoch") != std::string::npos);
}

RR_TEST(a_writer_that_dies_releases_authority_to_the_next_process) {
  rrtest::TempDir directory("mp-death");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  prepare(state_file);

  const std::filesystem::path marker = directory.file("held.txt");
  const std::filesystem::path hold_file = directory.file("hold.txt");
  write_file(hold_file, "hold");

  rrtest::ChildProcess holder =
      rrtest::ChildProcess::spawn(rrtest::helper_executable(kFenceChild),
                                  {"hold", state_file.string(), "wr:doomed", marker.string(),
                                   hold_file.string()},
                                  directory.file("holder.out"), directory.file("holder.err"));
  RR_REQUIRE(holder.started());
  RR_REQUIRE(rrtest::wait_for_file(marker, 30000));
  const std::uint64_t doomed_epoch = std::stoull(read_marker(marker));

  // Terminate the holder without giving it a chance to clean up, exactly as a
  // power loss or an external kill would.
  holder.terminate();
  const int exit_code = holder.wait();
  RR_CHECK(exit_code != 0);

  const auto lock_before = RackStore::query_writer_lock(state_file);
  RR_REQUIRE_OK(lock_before);
  RR_CHECK(lock_before.value().state == WriterLockState::Invalid);

  const rrtest::ProcessResult adopted =
      rrtest::run_process(rrtest::helper_executable(kFenceChild),
                          {"adopt", state_file.string(), "wr:survivor"});
  RR_CHECK(adopted.started);
  RR_CHECK_EQ(adopted.exit_code, 0);
  const std::size_t position = adopted.standard_output.find("adopted epoch ");
  RR_REQUIRE(position != std::string::npos);
  const std::uint64_t survivor_epoch = std::stoull(adopted.standard_output.substr(position + 14));
  RR_CHECK(survivor_epoch > doomed_epoch);

  // The state written before the death is intact.
  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:verify"));
  RR_REQUIRE_OK(store);
  RR_CHECK(store.value()->contains_rack(RackId::parse("rack:mp").value()));
  RR_REQUIRE_OK(store.value()->close());
}

RR_TEST(operator_fencing_stops_a_live_writer_from_publishing) {
  rrtest::TempDir directory("mp-fencing");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  prepare(state_file);

  const std::filesystem::path marker = directory.file("state.txt");
  const std::filesystem::path go_file = directory.file("go.txt");
  const std::filesystem::path quiet_file = directory.file("quiet.txt");

  // The child takes authority, then waits. It will only attempt its mutation
  // after the parent has fenced it.
  rrtest::ChildProcess child = rrtest::ChildProcess::spawn(
      rrtest::helper_executable(kFenceChild),
      {"wait-mutate", state_file.string(), "wr:writer", go_file.string(),
       "rack:mp", "rm:fenced", "1", "0", marker.string()},
      directory.file("child.out"), directory.file("child.err"));
  RR_REQUIRE(child.started());
  RR_REQUIRE(rrtest::wait_for_file(marker, 30000));
  const std::string ready = read_marker(marker);
  RR_CHECK(ready.rfind("ready:", 0) == 0);
  const std::uint64_t writer_epoch = std::stoull(ready.substr(6));

  // Operator fencing takes authority away from the live writer.
  const auto taken = RackStore::force_takeover(state_file, WriterId::parse("wr:operator").value(),
                                              4242);
  RR_REQUIRE_OK(taken);
  RR_CHECK(taken.value().epoch.value() > writer_epoch);

  write_file(go_file, "go");
  const int exit_code = child.wait();
  RR_CHECK_EQ(exit_code, kExitFenced);

  std::ifstream output(directory.file("child.out"));
  const std::string text((std::istreambuf_iterator<char>(output)),
                         std::istreambuf_iterator<char>());
  RR_CHECK(text.find("fenced") != std::string::npos);

  // Nothing the fenced writer attempted reached the durable state.
  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:verify"));
  RR_REQUIRE_OK(store);
  RR_CHECK(!store.value()->contains_rack(RackId::parse("rack:ghost").value()));
  const auto view = store.value()->rack(RackId::parse("rack:mp").value());
  RR_REQUIRE_OK(view);
  RR_CHECK_EQ(view.value().member_count(), std::size_t{0});
  RR_CHECK_EQ(view.value().membership_generation().value(), 0u);
  RR_REQUIRE_OK(store.value()->close());
  (void)quiet_file;
}

RR_TEST(two_processes_racing_for_authority_produce_exactly_one_winner) {
  rrtest::TempDir directory("mp-race");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  prepare(state_file);

  // Both children plan against generation 1 and membership generation 0. One
  // of them wins the lock and publishes; the other is either refused the lock
  // or refused by its precondition. It can never be applied twice.
  const std::filesystem::path marker_a = directory.file("a.txt");
  const std::filesystem::path marker_b = directory.file("b.txt");

  rrtest::ChildProcess first = rrtest::ChildProcess::spawn(
      rrtest::helper_executable(kFenceChild),
      {"mutate", state_file.string(), "wr:a", "rack:mp", "rm:racer-a", "1", "0",
       marker_a.string()},
      directory.file("a.out"), directory.file("a.err"));
  rrtest::ChildProcess second = rrtest::ChildProcess::spawn(
      rrtest::helper_executable(kFenceChild),
      {"mutate", state_file.string(), "wr:b", "rack:mp", "rm:racer-b", "1", "0",
       marker_b.string()},
      directory.file("b.out"), directory.file("b.err"));
  RR_REQUIRE(first.started());
  RR_REQUIRE(second.started());

  const int exit_a = first.wait();
  const int exit_b = second.wait();
  const int winners = (exit_a == 0 ? 1 : 0) + (exit_b == 0 ? 1 : 0);
  RR_CHECK_EQ(winners, 1);
  RR_CHECK(exit_a == 0 || exit_a == kExitHeld || exit_a == 2);
  RR_CHECK(exit_b == 0 || exit_b == kExitHeld || exit_b == 2);

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:verify"));
  RR_REQUIRE_OK(store);
  const auto view = store.value()->rack(RackId::parse("rack:mp").value());
  RR_REQUIRE_OK(view);
  // Exactly one member exists and the membership generation advanced once.
  RR_CHECK_EQ(view.value().member_count(), std::size_t{1});
  RR_CHECK_EQ(view.value().membership_generation().value(), 1u);
  RR_CHECK_EQ(store.value()->sequence().value(), 2u);
  const bool has_a = store.value()->member(RackId::parse("rack:mp").value(),
                                           RackMemberId::parse("rm:racer-a").value())
                         .value()
                         .has_value();
  const bool has_b = store.value()->member(RackId::parse("rack:mp").value(),
                                           RackMemberId::parse("rm:racer-b").value())
                         .value()
                         .has_value();
  RR_CHECK(has_a != has_b);
  RR_REQUIRE_OK(store.value()->close());
  std::cout << "  race exit codes: a=" << exit_a << " b=" << exit_b << "\n";
}

RR_TEST(sequential_writers_accumulate_state_across_processes) {
  rrtest::TempDir directory("mp-sequence");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  prepare(state_file);

  std::uint64_t generation = 1;
  std::uint64_t membership = 0;
  for (int index = 0; index < 4; ++index) {
    const std::string member = "rm:seq" + std::to_string(index);
    const std::filesystem::path marker = directory.file("seq" + std::to_string(index) + ".txt");
    const rrtest::ProcessResult result = rrtest::run_process(
        rrtest::helper_executable(kFenceChild),
        {"mutate", state_file.string(), "wr:seq" + std::to_string(index), "rack:mp", member,
         std::to_string(generation), std::to_string(membership), marker.string()});
    RR_CHECK(result.started);
    RR_CHECK_EQ(result.exit_code, 0);
    ++generation;
    ++membership;
  }

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:verify"));
  RR_REQUIRE_OK(store);
  const auto view = store.value()->rack(RackId::parse("rack:mp").value());
  RR_REQUIRE_OK(view);
  RR_CHECK_EQ(view.value().member_count(), std::size_t{4});
  RR_CHECK_EQ(view.value().membership_generation().value(), membership);
  RR_CHECK_EQ(store.value()->sequence().value(), 5u);
  const auto members =
      store.value()->members(RackId::parse("rack:mp").value(), MemberOrder::IdentityOrder);
  RR_REQUIRE_OK(members);
  for (std::size_t index = 0; index < members.value().size(); ++index) {
    RR_CHECK_EQ(members.value()[index].member_id.text(),
                std::string("rm:seq") + std::to_string(index));
  }
  RR_REQUIRE_OK(store.value()->close());
}
