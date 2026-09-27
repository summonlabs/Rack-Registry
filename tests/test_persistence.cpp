// Rack Registry - durable store, publication and recovery tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

RegisterRackRequest registration_for(std::string_view body, std::uint32_t units) {
  RegisterRackRequest request;
  request.structure.id = RackId::parse("rack:" + std::string(body)).value();
  request.structure.unit_count = units;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:store").value();
  request.structure.profile.provides = TraitSet::parse("power.ac.208v").value();
  request.identity.provenance = rrtest::provenance_for("store-test", 1);
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

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

// Options for a read-only observer, which never takes writer authority and so
// does not need a writer identity at all.
StoreOptions read_only_options_for(const std::filesystem::path& path) {
  StoreOptions options;
  options.path = path;
  options.read_only = true;
  options.producer = SourceReference::parse("rack-registry-tests/1.0.0").value();
  return options;
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

}  // namespace

// ---------------------------------------------------------------------------
// Opening, publishing and reopening
// ---------------------------------------------------------------------------

RR_TEST(a_fresh_store_starts_empty_and_publishes_generations) {
  rrtest::TempDir directory("store-fresh");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);
  RR_CHECK(store.value()->recovery().action == RecoveryAction::FreshStore);
  RR_CHECK_EQ(store.value()->sequence().value(), 0u);
  RR_CHECK(store.value()->holds_writer_authority());
  RR_CHECK(!store.value()->is_read_only());
  RR_CHECK(store.value()->writer_lock().state == WriterLockState::HeldByThisProcess);
  // A fresh store publishes nothing until the first mutation, so the state file
  // does not exist yet.
  RR_CHECK(!std::filesystem::exists(state_file));

  const auto registered = store.value()->register_rack(registration_for("store-a", 4));
  RR_REQUIRE_OK(registered);
  RR_CHECK_EQ(store.value()->sequence().value(), 1u);
  RR_CHECK(std::filesystem::exists(state_file));

  const auto info = RackStore::inspect(state_file);
  RR_REQUIRE_OK(info);
  RR_CHECK_EQ(info.value().sequence.value(), 1u);
  RR_CHECK_EQ(info.value().rack_count, std::size_t{1});
  RR_CHECK_EQ(info.value().epoch.value(), store.value()->epoch().value());
  RR_CHECK_EQ(info.value().format_version, kStateFormatVersion);

  InsertMemberRequest insert;
  insert.rack_id = registration_for("store-a", 4).structure.id;
  insert.precondition.expected_generation = registered.value().generation;
  insert.precondition.expected_membership_generation = registered.value().membership_generation;
  insert.member_id = RackMemberId::parse("rm:store-1").value();
  insert.asset_id = AssetId::parse("asset:store-1").value();
  insert.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 2).value()).value();
  insert.identity.provenance = rrtest::provenance_for("store-test", 2);
  const auto published = store.value()->insert_member(insert);
  RR_REQUIRE_OK(published);
  RR_CHECK_EQ(store.value()->sequence().value(), 2u);
  RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});

  RR_REQUIRE_OK(store.value()->close());
  RR_REQUIRE_OK(store.value()->close());  // idempotent
  // Closing releases writer authority even for a rebuilt lock record.
  const auto lock_after_close = RackStore::query_writer_lock(state_file);
  RR_REQUIRE_OK(lock_after_close);
  RR_CHECK(lock_after_close.value().state == WriterLockState::Unlocked);
}

RR_TEST(reopening_recovers_the_published_generation_and_advances_the_epoch) {
  rrtest::TempDir directory("store-reopen");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  StoreEpoch first_epoch{};
  StateDigest published_digest{};

  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    first_epoch = store.value()->epoch();
    const auto registered = store.value()->register_rack(registration_for("store-a", 4));
    RR_REQUIRE_OK(registered);
    published_digest = registered.value().state_digest;
  }

  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_CHECK(store.value()->recovery().action == RecoveryAction::LoadedCurrent);
    RR_CHECK(store.value()->epoch().value() > first_epoch.value());
    RR_CHECK_EQ(store.value()->sequence().value(), 1u);
    const auto view = store.value()->rack(RackId::parse("rack:store-a").value());
    RR_REQUIRE_OK(view);
    RR_CHECK_EQ(view.value().state_digest(), published_digest);
    RR_CHECK_EQ(view.value().generation().value(), 1u);
  }

  // Repeated open and close cycles keep the state and never leave residue.
  for (int cycle = 0; cycle < 5; ++cycle) {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    const std::size_t expected = cycle == 0 ? 1u : 2u;
    RR_CHECK_EQ(store.value()->rack_count(), expected);
    RR_CHECK(store.value()->contains_rack(RackId::parse("rack:store-a").value()));
    const auto registered = store.value()->register_rack(registration_for("cycle", 2));
    if (cycle == 0) {
      RR_REQUIRE_OK(registered);
    } else {
      RR_REQUIRE_CODE(registered, ErrorCode::DuplicateRackId);
    }
    RR_REQUIRE_OK(store.value()->close());
    RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});
  }
}

RR_TEST(a_read_only_store_serves_reads_and_refuses_mutations) {
  rrtest::TempDir directory("store-readonly");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto writer = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(writer);
  const auto registered = writer.value()->register_rack(registration_for("store-a", 4));
  RR_REQUIRE_OK(registered);

  auto reader = RackStore::open(read_only_options_for(state_file));
  RR_REQUIRE_OK(reader);
  RR_CHECK(reader.value()->is_read_only());
  RR_CHECK(!reader.value()->holds_writer_authority());
  RR_CHECK_EQ(reader.value()->rack_count(), std::size_t{1});
  RR_CHECK(reader.value()->contains_rack(RackId::parse("rack:store-a").value()));

  const auto refused = reader.value()->register_rack(registration_for("store-b", 4));
  RR_REQUIRE_CODE(refused, ErrorCode::ReadOnlyStore);

  const auto insert = reader.value()->insert_member(InsertMemberRequest{});
  RR_REQUIRE_CODE(insert, ErrorCode::ReadOnlyStore);

  // A read-only store never takes the writer lock, so the writer keeps it.
  const auto lock = RackStore::query_writer_lock(state_file);
  RR_REQUIRE_OK(lock);
  RR_CHECK(lock.value().state == WriterLockState::HeldByAnotherProcess);
}

RR_TEST(a_second_live_writer_is_refused) {
  rrtest::TempDir directory("store-two-writers");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto first = RackStore::open(rrtest::store_options_for(state_file, "wr:first"));
  RR_REQUIRE_OK(first);

  const auto second = RackStore::open(rrtest::store_options_for(state_file, "wr:second"));
  RR_REQUIRE_CODE(second, ErrorCode::WriterLockHeld);

  RR_REQUIRE_OK(first.value()->close());
  auto third = RackStore::open(rrtest::store_options_for(state_file, "wr:third"));
  RR_REQUIRE_OK(third);
  RR_CHECK(third.value()->epoch().value() > first.value()->epoch().value());
}

RR_TEST(a_store_can_be_required_to_exist) {
  rrtest::TempDir directory("store-required");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  StoreOptions options = rrtest::store_options_for(state_file, "wr:store");
  options.create_if_missing = false;
  const auto opened = RackStore::open(options);
  RR_REQUIRE_CODE(opened, ErrorCode::NoAuthoritativeState);
}

// ---------------------------------------------------------------------------
// Publication failure and rollback
// ---------------------------------------------------------------------------

RR_TEST(a_fault_before_the_commit_boundary_rolls_back_and_leaves_the_file_untouched) {
  rrtest::TempDir directory("store-faults");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto setup = RackStore::open(rrtest::store_options_for(state_file, "wr:setup"));
  RR_REQUIRE_OK(setup);
  const auto registered = setup.value()->register_rack(registration_for("store-a", 4));
  RR_REQUIRE_OK(registered);
  RR_REQUIRE_OK(setup.value()->close());

  const std::vector<std::uint8_t> good = read_bytes(state_file);
  RR_REQUIRE(!good.empty());

  const std::vector<WriteStage> pre_commit = {
      WriteStage::AfterTempCreated, WriteStage::AfterTempWritten,
      WriteStage::AfterTempSynced,  WriteStage::AfterPreviousPublished,
      WriteStage::BeforePublishRename};

  for (const WriteStage stage : pre_commit) {
    StoreOptions options = rrtest::store_options_for(state_file, "wr:fault");
    options.fault_hook = [stage](WriteStage current) {
      if (current == stage) {
        throw std::runtime_error("injected publication fault");
      }
    };
    auto store = RackStore::open(options);
    RR_REQUIRE_OK(store);

    const auto view = store.value()->rack(RackId::parse("rack:store-a").value());
    RR_REQUIRE_OK(view);
    const std::uint64_t generation_before = view.value().generation().value();
    const std::size_t members_before = view.value().member_count();

    InsertMemberRequest insert;
    insert.rack_id = RackId::parse("rack:store-a").value();
    insert.precondition.expected_generation = view.value().generation();
    insert.precondition.expected_membership_generation = view.value().membership_generation();
    insert.member_id = RackMemberId::parse("rm:fault").value();
    insert.asset_id = AssetId::parse("asset:fault").value();
    insert.mount = MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(1).value())
                                             .value())
                       .value();
    insert.identity.provenance = rrtest::provenance_for("store-test", 3);

    const auto receipt = store.value()->insert_member(insert);
    RR_REQUIRE_CODE(receipt, ErrorCode::IoFailure);

    // The in-memory image must be exactly where it was: a mutation that could
    // not be published is rolled back.
    const auto after = store.value()->rack(RackId::parse("rack:store-a").value());
    RR_REQUIRE_OK(after);
    RR_CHECK_EQ(after.value().generation().value(), generation_before);
    RR_CHECK_EQ(after.value().member_count(), members_before);
    RR_CHECK_EQ(store.value()->sequence().value(), 1u);
    RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});
    RR_REQUIRE_OK(store.value()->close());

    // The durable file is still the last good generation, byte for byte.
    const std::vector<std::uint8_t> current = read_bytes(state_file);
    RR_CHECK_EQ(current.size(), good.size());
    RR_CHECK(std::equal(current.begin(), current.end(), good.begin()));
  }

  // Recovery after all of those faults still reports the good generation.
  auto reopened = RackStore::open(rrtest::store_options_for(state_file, "wr:after"));
  RR_REQUIRE_OK(reopened);
  RR_CHECK(reopened.value()->recovery().action == RecoveryAction::LoadedCurrent);
  RR_CHECK_EQ(reopened.value()->rack_count(), std::size_t{1});
}

RR_TEST(a_fault_after_the_commit_boundary_does_not_undo_published_work) {
  rrtest::TempDir directory("store-postcommit");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  for (const WriteStage stage : {WriteStage::AfterPublishRename, WriteStage::AfterTempRetired}) {
    StoreOptions setup = rrtest::store_options_for(state_file, "wr:setup");
    auto creating = RackStore::open(setup);
    RR_REQUIRE_OK(creating);
    const auto registered = creating.value()->register_rack(registration_for("store-a", 4));
    RR_REQUIRE_OK(registered);
    const std::uint64_t expected_generation = registered.value().generation.value();
    RR_REQUIRE_OK(creating.value()->close());

    StoreOptions options = rrtest::store_options_for(state_file, "wr:postcommit");
    options.fault_hook = [stage](WriteStage current) {
      if (current == stage) {
        throw std::runtime_error("injected post-commit fault");
      }
    };
    auto store = RackStore::open(options);
    RR_REQUIRE_OK(store);
    const auto view = store.value()->rack(RackId::parse("rack:store-a").value());
    RR_REQUIRE_OK(view);
    InsertMemberRequest insert;
    insert.rack_id = RackId::parse("rack:store-a").value();
    insert.precondition.expected_generation = view.value().generation();
    insert.precondition.expected_membership_generation = view.value().membership_generation();
    insert.member_id = RackMemberId::parse("rm:committed").value();
    insert.asset_id = AssetId::parse("asset:committed").value();
    insert.mount = MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(1).value())
                                             .value())
                       .value();
    insert.identity.provenance = rrtest::provenance_for("store-test", 6);

    // The generation crossed the completion boundary, so the mutation stands
    // and the durable file carries it.
    const auto receipt = store.value()->insert_member(insert);
    RR_REQUIRE_OK(receipt);
    RR_CHECK_EQ(receipt.value().generation.value(), expected_generation + 1);
    RR_CHECK_EQ(store.value()->sequence().value(), 2u);
    RR_REQUIRE_OK(store.value()->close());

    const auto info = RackStore::inspect(state_file);
    RR_REQUIRE_OK(info);
    RR_CHECK_EQ(info.value().sequence.value(), 2u);
    RR_CHECK_EQ(info.value().member_count, std::size_t{1});

    std::error_code error;
    std::filesystem::remove(state_file, error);
    std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
    std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);
  }
}

RR_TEST(a_corrupt_current_generation_falls_back_to_the_retained_previous_one) {
  rrtest::TempDir directory("store-fallback");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  const std::filesystem::path previous_file = std::filesystem::path(state_file.string() + ".prev");

  std::vector<std::uint8_t> first_generation;
  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-a", 4)));
    RR_REQUIRE_OK(store.value()->close());
    first_generation = read_bytes(state_file);
    RR_REQUIRE(!first_generation.empty());
  }
  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-b", 4)));
    RR_REQUIRE_OK(store.value()->close());
    // After the second publication the retained file is the first generation.
    RR_CHECK(std::filesystem::exists(previous_file));
  }

  // Corrupt the current generation and reopen: the retained previous one must
  // become authoritative, and the corrupt file must be left untouched.
  std::vector<std::uint8_t> damaged = read_bytes(state_file);
  RR_REQUIRE(!damaged.empty());
  damaged[damaged.size() / 2] = static_cast<std::uint8_t>(damaged[damaged.size() / 2] ^ 0xFFu);
  write_bytes(state_file, damaged);

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);
  RR_CHECK(store.value()->recovery().action == RecoveryAction::LoadedPrevious);
  RR_CHECK(store.value()->recovery().current_rejection.has_value());
  RR_CHECK(store.value()->contains_rack(RackId::parse("rack:store-a").value()));
  RR_CHECK(!store.value()->contains_rack(RackId::parse("rack:store-b").value()));
  RR_CHECK_EQ(store.value()->rack_count(), std::size_t{1});
  RR_REQUIRE_OK(store.value()->close());

  // The rejected file was not rewritten behind the operator's back.
  const std::vector<std::uint8_t> still_damaged = read_bytes(state_file);
  RR_CHECK(std::equal(still_damaged.begin(), still_damaged.end(), damaged.begin()));
}

RR_TEST(a_corrupt_current_generation_without_a_fallback_refuses_to_open) {
  rrtest::TempDir directory("store-nofallback");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-a", 4)));
    RR_REQUIRE_OK(store.value()->close());
  }
  std::vector<std::uint8_t> damaged = read_bytes(state_file);
  RR_REQUIRE(!damaged.empty());
  damaged[damaged.size() - 1] = 0x00;
  write_bytes(state_file, damaged);

  const auto opened = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_CHECK(!opened.has_value());
  if (!opened.has_value()) {
    RR_CHECK(opened.error().code == ErrorCode::IntegrityCheckFailed ||
             opened.error().code == ErrorCode::CorruptState ||
             opened.error().code == ErrorCode::TruncatedState);
  }
}

RR_TEST(stale_temporary_files_are_retired_when_a_writer_opens_the_store) {
  rrtest::TempDir directory("store-temps");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-a", 4)));
    RR_REQUIRE_OK(store.value()->close());
  }

  // Simulate the residue an interrupted publication would leave behind.
  const std::filesystem::path temp_a =
      directory.file(state_file.filename().string() + ".tmp-state-deadbeef-1");
  const std::filesystem::path temp_b =
      directory.file(state_file.filename().string() + ".lock.tmp-deadbeef-1");
  write_bytes(temp_a, {1, 2, 3});
  write_bytes(temp_b, {4, 5, 6});
  RR_CHECK_EQ(count_temp_files(state_file), std::size_t{2});

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);
  RR_CHECK_EQ(store.value()->recovery().retired_temp_files.size(), std::size_t{2});
  RR_CHECK_EQ(count_temp_files(state_file), std::size_t{0});
  RR_CHECK_EQ(store.value()->rack_count(), std::size_t{1});
  RR_REQUIRE_OK(store.value()->close());
}

// ---------------------------------------------------------------------------
// Writer authority
// ---------------------------------------------------------------------------

RR_TEST(a_lock_whose_holder_is_not_running_is_adopted_with_a_higher_epoch) {
  rrtest::TempDir directory("store-abandoned");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-a", 4)));
    RR_REQUIRE_OK(store.value()->close());
  }

  // Operator fencing leaves a lock that names no running process, which is
  // exactly the shape a crashed writer leaves behind. The real crash case is
  // proven by the process-level suite.
  const auto taken = RackStore::force_takeover(state_file, WriterId::parse("wr:gone").value(), 7);
  RR_REQUIRE_OK(taken);
  const auto waiting = RackStore::query_writer_lock(state_file);
  RR_REQUIRE_OK(waiting);
  RR_CHECK(waiting.value().state == WriterLockState::Unlocked);
  RR_CHECK(waiting.value().detail.find("released") != std::string::npos ||
           waiting.value().detail.find("not running") != std::string::npos ||
           !waiting.value().detail.empty());

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:recover"));
  RR_REQUIRE_OK(store);
  RR_CHECK(store.value()->epoch().value() > taken.value().epoch.value());
  RR_CHECK(store.value()->writer_lock().state == WriterLockState::HeldByThisProcess);
  RR_CHECK_EQ(store.value()->rack_count(), std::size_t{1});
  RR_REQUIRE_OK(store.value()->close());
}

RR_TEST(an_unverifiable_writer_lock_is_replaced_only_when_adoption_is_permitted) {
  rrtest::TempDir directory("store-badlock");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  const std::filesystem::path lock_file = std::filesystem::path(state_file.string() + ".lock");

  write_bytes(lock_file, {0x41, 0x42, 0x43});

  StoreOptions strict = rrtest::store_options_for(state_file, "wr:store");
  strict.adopt_abandoned_lock = false;
  RR_REQUIRE_CODE(RackStore::open(strict), ErrorCode::WriterLockInvalid);

  const auto status = RackStore::query_writer_lock(state_file);
  RR_REQUIRE_OK(status);
  RR_CHECK(status.value().state == WriterLockState::Invalid);

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);
  RR_CHECK(store.value()->writer_lock().state == WriterLockState::HeldByThisProcess);
  RR_REQUIRE_OK(store.value()->close());
}

RR_TEST(operator_fencing_advances_the_epoch_and_records_the_previous_holder) {
  rrtest::TempDir directory("store-takeover");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);
  const StoreEpoch before = store.value()->epoch();
  RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-a", 4)));
  RR_CHECK_EQ(store.value()->sequence().value(), 1u);

  const auto taken = RackStore::force_takeover(state_file, WriterId::parse("wr:operator").value(),
                                               1234567);
  RR_REQUIRE_OK(taken);
  RR_CHECK(taken.value().epoch.value() > before.value());
  RR_CHECK_EQ(taken.value().taken_over_from.text(), std::string("wr:store"));
  RR_CHECK_EQ(taken.value().taken_over_epoch.value(), before.value());
  RR_CHECK_EQ(taken.value().acquired_at_unix_ns, 1234567u);

  // The fenced writer cannot publish any more: the publication fence rejects it
  // before the atomic replace, so the durable file keeps the other writer's
  // generation.
  const auto blocked = store.value()->register_rack(registration_for("blocked", 2));
  RR_REQUIRE_CODE(blocked, ErrorCode::StaleWriterFenced);
  RR_CHECK_EQ(store.value()->sequence().value(), 1u);

  // The takeover lock names no live process, so a new writer adopts it.
  auto successor = RackStore::open(rrtest::store_options_for(state_file, "wr:successor"));
  RR_REQUIRE_OK(successor);
  RR_CHECK(successor.value()->epoch().value() > taken.value().epoch.value());
  RR_REQUIRE_OK(successor.value()->close());
  RR_REQUIRE_OK(store.value()->close());
}

RR_TEST(a_damaged_current_generation_is_never_promoted_to_the_retained_one) {
  rrtest::TempDir directory("store-retain");
  const std::filesystem::path state_file = directory.file("state.rrstate");
  const std::filesystem::path previous_file = std::filesystem::path(state_file.string() + ".prev");

  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-a", 4)));
    RR_REQUIRE_OK(store.value()->close());
  }
  {
    auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
    RR_REQUIRE_OK(store);
    RR_REQUIRE_OK(store.value()->register_rack(registration_for("store-b", 4)));
    RR_REQUIRE_OK(store.value()->close());
  }
  const std::vector<std::uint8_t> good_previous = read_bytes(previous_file);
  RR_REQUIRE(!good_previous.empty());

  // Damage the current generation, then recover from the retained one and
  // publish a new generation. The damaged file must not be promoted into the
  // retained slot, because that would destroy the last known-good fallback.
  std::vector<std::uint8_t> damaged = read_bytes(state_file);
  RR_REQUIRE(!damaged.empty());
  damaged[damaged.size() / 3] = static_cast<std::uint8_t>(damaged[damaged.size() / 3] ^ 0x5Au);
  write_bytes(state_file, damaged);

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);
  RR_CHECK(store.value()->recovery().action == RecoveryAction::LoadedPrevious);
  RR_REQUIRE_OK(store.value()->register_rack(registration_for("published-after-recovery", 2)));
  RR_REQUIRE_OK(store.value()->close());

  // The current generation is the newly published one and verifies.
  const auto info = RackStore::inspect(state_file);
  RR_REQUIRE_OK(info);
  RR_CHECK_EQ(info.value().rack_count, std::size_t{2});

  // The retained generation is still the previous known-good one.
  const std::vector<std::uint8_t> retained = read_bytes(previous_file);
  RR_CHECK_EQ(retained.size(), good_previous.size());
  RR_CHECK(std::equal(retained.begin(), retained.end(), good_previous.begin()));

  const auto retained_info = RackStore::inspect(previous_file);
  RR_REQUIRE_OK(retained_info);
  RR_CHECK_EQ(retained_info.value().rack_count, std::size_t{1});
}

RR_TEST(diffing_against_the_retained_previous_publication_works) {
  rrtest::TempDir directory("store-diff");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  auto store = RackStore::open(rrtest::store_options_for(state_file, "wr:store"));
  RR_REQUIRE_OK(store);

  const auto missing = store.value()->diff_with_previous();
  RR_REQUIRE_CODE(missing, ErrorCode::NoAuthoritativeState);

  const auto registered = store.value()->register_rack(registration_for("store-a", 4));
  RR_REQUIRE_OK(registered);
  const auto no_previous_yet = store.value()->diff_with_previous();
  RR_REQUIRE_CODE(no_previous_yet, ErrorCode::NoAuthoritativeState);

  InsertMemberRequest insert;
  insert.rack_id = RackId::parse("rack:store-a").value();
  insert.precondition.expected_generation = registered.value().generation;
  insert.precondition.expected_membership_generation = registered.value().membership_generation;
  insert.member_id = RackMemberId::parse("rm:diff-1").value();
  insert.asset_id = AssetId::parse("asset:diff-1").value();
  insert.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 1).value()).value();
  insert.identity.provenance = rrtest::provenance_for("store-test", 5);
  const auto published = store.value()->insert_member(insert);
  RR_REQUIRE_OK(published);

  const auto diff = store.value()->diff_with_previous();
  RR_REQUIRE_OK(diff);
  RR_CHECK(!diff.value().identical());
  RR_REQUIRE(diff.value().racks_changed.size() == 1);
  RR_CHECK_EQ(diff.value().racks_changed[0].rack_id.text(), std::string("rack:store-a"));
  RR_CHECK_EQ(diff.value().racks_changed[0].member_changes.size(), std::size_t{1});
  RR_CHECK(has_flag(diff.value().racks_changed[0].member_changes[0].flags,
                    MemberChangeFlag::Added));

  RR_REQUIRE_OK(store.value()->close());
}
