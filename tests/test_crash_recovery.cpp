// Rack Registry - crash injection and last-known-good recovery tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These tests crash a *real* operating-system process at each named durable
// step and then reopen the store in the test process. Nothing here is a
// simulation: the child is terminated by the operating system inside the
// publication sequence, so the recovery path is exercised exactly as a power
// loss or a kill would exercise it.

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

constexpr std::string_view kCrashChild = "rr_crash_child";

RegisterRackRequest crash_setup_registration() {
  RegisterRackRequest request;
  request.structure.id = RackId::parse("rack:crash-base").value();
  request.structure.unit_count = 4;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:crash").value();
  request.identity.provenance = rrtest::provenance_for("crash-test", 1);
  return request;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::uint8_t> bytes;
  if (!stream.good()) {
    return bytes;
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  stream.seekg(0, std::ios::beg);
  if (size > 0) {
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
  }
  return bytes;
}

std::size_t count_temp_files(const std::filesystem::path& state_file) {
  std::size_t count = 0;
  const std::string prefix = state_file.filename().string() + ".tmp-";
  const std::string lock_prefix = state_file.filename().string() + ".lock.tmp-";
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(state_file.parent_path(), error)) {
    if (error) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 || name.rfind(lock_prefix, 0) == 0) {
      ++count;
    }
  }
  return count;
}

struct CrashOutcome {
  bool crashed = false;
  int exit_code = -1;
  std::string output{};
  std::string error{};
};

CrashOutcome crash_at(const std::filesystem::path& state_file, WriteStage stage,
                      std::string_view rack_body, std::string_view operation,
                      std::string_view member, std::uint64_t generation,
                      std::uint64_t membership_generation) {
  CrashOutcome outcome;
  std::vector<std::string> arguments = {state_file.string(), "wr:crash",
                                        std::string(write_stage_name(stage)),
                                        "rack:" + std::string(rack_body)};
  if (!operation.empty()) {
    arguments.push_back("--operation");
    arguments.push_back(std::string(operation));
  }
  if (!member.empty()) {
    arguments.push_back("--member");
    arguments.push_back(std::string(member));
    arguments.push_back("--asset");
    arguments.push_back("asset:crash-" + std::string(member.substr(3)));
    arguments.push_back("--mount");
    arguments.push_back("U1");
    arguments.push_back("--generation");
    arguments.push_back(std::to_string(generation));
    arguments.push_back("--membership-generation");
    arguments.push_back(std::to_string(membership_generation));
  } else {
    arguments.push_back("--units");
    arguments.push_back("4");
  }
  const rrtest::ProcessResult result =
      rrtest::run_process(rrtest::helper_executable(kCrashChild), arguments);
  outcome.crashed = result.started && result.exit_code != 0;
  outcome.exit_code = result.exit_code;
  outcome.output = result.standard_output;
  outcome.error = result.standard_error;
  return outcome;
}

// Prepares a store holding one rack, and returns the byte image of that
// generation so a test can prove it survived unchanged.
std::vector<std::uint8_t> prepare(const std::filesystem::path& state_file) {
  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:setup"));
  if (!store) {
    return {};
  }
  if (!store.value()->register_rack(crash_setup_registration())) {
    return {};
  }
  if (!store.value()->close()) {
    return {};
  }
  return read_bytes(state_file);
}

}  // namespace

RR_TEST(a_crash_before_the_rename_keeps_the_previous_generation_authoritative) {
  rrtest::TempDir directory("crash-before");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  const std::vector<std::uint8_t> good = prepare(state_file);
  RR_REQUIRE(!good.empty());

  const std::vector<WriteStage> stages = {
      WriteStage::AfterLockAcquired,      WriteStage::AfterTempCreated,
      WriteStage::AfterTempWritten,       WriteStage::AfterTempSynced,
      WriteStage::AfterPreviousPublished, WriteStage::BeforePublishRename};

  for (const WriteStage stage : stages) {
    const CrashOutcome outcome =
        crash_at(state_file, stage, "crash-new", "", "", 0, 0);
    RR_CHECK(outcome.crashed);
    RR_CHECK(outcome.exit_code != 0);

    // The abandoned writer lock is adopted and the previous generation is
    // authoritative and byte-identical.
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:recover"));
    RR_REQUIRE_OK(store);
    RR_CHECK(store.value()->recovery().action == RecoveryAction::LoadedCurrent);
    RR_CHECK(store.value()->contains_rack(RackId::parse("rack:crash-base").value()));
    RR_CHECK(!store.value()->contains_rack(RackId::parse("rack:crash-new").value()));
    RR_CHECK_EQ(store.value()->rack_count(), std::size_t{1});
    RR_REQUIRE_OK(store.value()->close());

    const std::vector<std::uint8_t> after = read_bytes(state_file);
    RR_CHECK_EQ(after.size(), good.size());
    RR_CHECK(std::equal(after.begin(), after.end(), good.begin()));
    // Recovery retires whatever the interrupted publication left behind.
    RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});
  }
}

RR_TEST(a_crash_after_the_rename_leaves_the_new_generation_authoritative) {
  rrtest::TempDir directory("crash-after");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  RR_REQUIRE(!prepare(state_file).empty());

  for (const WriteStage stage : {WriteStage::AfterPublishRename, WriteStage::AfterTempRetired}) {
    std::error_code error;
    // Start from a clean store for each stage so the assertion is exact.
    std::filesystem::remove(state_file, error);
    std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
    std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);
    RR_REQUIRE(!prepare(state_file).empty());

    const CrashOutcome outcome = crash_at(state_file, stage, "crash-published", "", "", 0, 0);
    RR_CHECK(outcome.crashed);

    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:recover"));
    RR_REQUIRE_OK(store);
    RR_CHECK(store.value()->recovery().action == RecoveryAction::LoadedCurrent);
    RR_CHECK(store.value()->contains_rack(RackId::parse("rack:crash-base").value()));
    RR_CHECK(store.value()->contains_rack(RackId::parse("rack:crash-published").value()));
    RR_CHECK_EQ(store.value()->rack_count(), std::size_t{2});
    RR_CHECK_EQ(store.value()->sequence().value(), 2u);
    RR_REQUIRE_OK(store.value()->close());
    RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});
  }
}

RR_TEST(a_crash_during_a_multi_record_occupancy_update_applies_all_or_nothing) {
  rrtest::TempDir directory("crash-occupancy");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  RR_REQUIRE(!prepare(state_file).empty());

  // The member insertion writes several records at once: the member, its
  // provenance, the rack provenance and the generation evidence. A crash in the
  // middle of publication must leave either none or all of them.
  const std::vector<WriteStage> before_commit = {
      WriteStage::AfterTempCreated, WriteStage::AfterTempWritten, WriteStage::AfterTempSynced,
      WriteStage::AfterPreviousPublished, WriteStage::BeforePublishRename};

  for (const WriteStage stage : before_commit) {
    std::error_code error;
    std::filesystem::remove(state_file, error);
    std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
    std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);
    RR_REQUIRE(!prepare(state_file).empty());

    const CrashOutcome outcome =
        crash_at(state_file, stage, "crash-base", "insert", "rm:crash-member", 1, 0);
    RR_CHECK(outcome.crashed);

    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:recover"));
    RR_REQUIRE_OK(store);
    const auto view = store.value()->rack(RackId::parse("rack:crash-base").value());
    RR_REQUIRE_OK(view);
    // Nothing was published, so the member, the membership generation and the
    // member's provenance are all absent together.
    RR_CHECK_EQ(view.value().member_count(), std::size_t{0});
    RR_CHECK_EQ(view.value().membership_generation().value(), 0u);
    RR_CHECK_EQ(view.value().generation().value(), 1u);
    const auto members = store.value()->members(RackId::parse("rack:crash-base").value(),
                                                MemberOrder::MountOrder);
    RR_REQUIRE_OK(members);
    RR_CHECK(members.value().empty());
    RR_REQUIRE_OK(store.value()->close());
  }

  // The same insertion without a fault applies every record together, and the
  // result survives a reopen.
  const CrashOutcome clean =
      crash_at(state_file, WriteStage::AfterLockAcquired, "crash-base", "insert",
               "rm:crash-member", 1, 0);
  RR_CHECK(clean.crashed);
  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:finish"));
  RR_REQUIRE_OK(store);
  InsertMemberRequest insert;
  insert.rack_id = RackId::parse("rack:crash-base").value();
  const auto view = store.value()->rack(insert.rack_id);
  RR_REQUIRE_OK(view);
  insert.precondition.expected_generation = view.value().generation();
  insert.precondition.expected_membership_generation = view.value().membership_generation();
  insert.member_id = RackMemberId::parse("rm:crash-member").value();
  insert.asset_id = AssetId::parse("asset:crash-member").value();
  insert.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 1).value()).value();
  insert.identity.provenance = rrtest::provenance_for("crash-test", 2);
  const auto receipt = store.value()->insert_member(insert);
  RR_REQUIRE_OK(receipt);
  RR_REQUIRE_OK(store.value()->close());

  auto reopened = RackStore::open(rrtest::store_options_for(state_file, "wr:verify"));
  RR_REQUIRE_OK(reopened);
  const auto after = reopened.value()->rack(insert.rack_id);
  RR_REQUIRE_OK(after);
  RR_CHECK_EQ(after.value().member_count(), std::size_t{1});
  RR_CHECK_EQ(after.value().membership_generation().value(), 1u);
  const auto member = reopened.value()->member(insert.rack_id, insert.member_id);
  RR_REQUIRE_OK(member);
  RR_REQUIRE(member.value().has_value());
  RR_CHECK(!member.value()->provenance.empty());
  RR_REQUIRE_OK(reopened.value()->close());
}

RR_TEST(a_repeated_crash_and_reopen_cycle_converges) {
  rrtest::TempDir directory("crash-cycle");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  RR_REQUIRE(!prepare(state_file).empty());

  std::uint64_t expected_racks = 1;
  for (int cycle = 0; cycle < 6; ++cycle) {
    const std::string body = "cycle-" + std::to_string(cycle);
    const CrashOutcome outcome =
        crash_at(state_file, WriteStage::BeforePublishRename, body, "", "", 0, 0);
    RR_CHECK(outcome.crashed);

    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:cycle"));
    RR_REQUIRE_OK(store);
    RR_CHECK_EQ(store.value()->rack_count(), static_cast<std::size_t>(expected_racks));
    RR_CHECK(store.value()->contains_rack(RackId::parse("rack:crash-base").value()));
    RR_CHECK(!store.value()->contains_rack(RackId::parse("rack:" + body).value()));

    // Now publish a generation successfully so the next cycle starts from a
    // larger state.
    RegisterRackRequest request;
    request.structure.id = RackId::parse("rack:" + body).value();
    request.structure.unit_count = 4;
    request.structure.profile.id = CompatibilityProfileId::parse("cp:crash").value();
    request.identity.provenance = rrtest::provenance_for("crash-test", 3);
    const auto registered = store.value()->register_rack(request);
    RR_REQUIRE_OK(registered);
    ++expected_racks;
    RR_REQUIRE_OK(store.value()->close());
  }

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:final"));
  RR_REQUIRE_OK(store);
  RR_CHECK_EQ(store.value()->rack_count(), static_cast<std::size_t>(expected_racks));
  RR_CHECK_EQ(store.value()->recovery().action, RecoveryAction::LoadedCurrent);
  RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});
  RR_REQUIRE_OK(store.value()->close());
}
