// Rack Registry - multiprocess writer fencing child process.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is a real, separate operating-system process that competes for
// writer authority over one state file. The parent test uses it to prove that
// exactly one writer can hold authority at a time and that a writer whose epoch
// was taken over cannot publish. It is built only when the test suite is
// enabled and is not installed.
//
// Subcommands
//   hold <state> <writer> <marker> <hold-file>
//       Acquire authority, record the epoch in <marker>, then block until
//       <hold-file> is removed. Exit code 0.
//   try-open <state> <writer>
//       Try to acquire authority. Print "acquired <epoch>" and exit 0, or
//       "held <state>" and exit 3.
//   mutate <state> <writer> <rack> <member> <generation> <membership-generation> <marker>
//       Acquire authority and insert one member. Print the receipt, write the
//       marker file, exit 0. Exit 3 when another writer holds authority.
//   wait-mutate <state> <writer> <go-file> <rack> <member> <generation>
//               <membership-generation> <marker>
//       Acquire authority and wait for <go-file> to appear, then attempt the
//       mutation. Print "published" and exit 0, or "fenced" and exit 4.
//   adopt <state> <writer>
//       Open writable against an abandoned lock, print the epoch, close.

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cli_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

constexpr int kExitHeld = 3;
constexpr int kExitFenced = 4;

void write_marker(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
}

bool marker_exists(const std::string& path) {
  std::ifstream stream(path);
  return stream.good();
}

Result<std::unique_ptr<RackStore>> open_writer(const std::string& state_path,
                                               const std::string& writer_text) {
  const auto writer = WriterId::parse(writer_text);
  if (!writer) {
    return writer.error();
  }
  StoreOptions options;
  options.path = state_path;
  options.writer_id = writer.value();
  options.producer = SourceReference::parse("rr_fence_child/1.0.0").value();
  return RackStore::open(options);
}

InsertMemberRequest make_insert(const std::string& rack_text, const std::string& member_text,
                                std::uint64_t generation, std::uint64_t membership_generation) {
  InsertMemberRequest request;
  request.rack_id = RackId::parse(rack_text).value();
  request.precondition.expected_generation = RackGeneration::create(generation).value();
  request.precondition.expected_membership_generation =
      MembershipGeneration::create(membership_generation).value();
  request.member_id = RackMemberId::parse(member_text).value();
  request.asset_id = AssetId::parse("asset:" + member_text.substr(3)).value();
  // Each successive membership generation is placed on its own rack unit, so a
  // sequence of child processes builds up state instead of colliding.
  const std::uint32_t unit = static_cast<std::uint32_t>(membership_generation) + 1u;
  const auto unit_index = RackUnitIndex::create(unit);
  if (!unit_index) {
    return request;
  }
  const auto span = RackUnitRange::single(unit_index.value());
  if (!span) {
    return request;
  }
  const auto mount = MountSpan::full_units(span.value());
  if (!mount) {
    return request;
  }
  request.mount = mount.value();
  request.identity.provenance.source = ProvenanceSource::System;
  request.identity.provenance.actor = ActorId::parse("fence-child").value();
  request.identity.provenance.observed_at_unix_ns = 2000;
  return request;
}

int command_hold(const std::vector<std::string>& rest) {
  if (rest.size() < 4) {
    return 1;
  }
  auto store = open_writer(rest[0], rest[1]);
  if (!store) {
    rackregistry::cli::print_error(store.error());
    return 2;
  }
  write_marker(rest[2], std::to_string(store.value()->epoch().value()));
  std::cout << "holding epoch " << store.value()->epoch().value() << std::endl;
  // Block until the parent removes the hold file, which lets a test either
  // release this holder cleanly or terminate it while it still owns authority.
  while (marker_exists(rest[3])) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  (void)store.value()->close();
  return 0;
}

int command_try_open(const std::vector<std::string>& rest) {
  if (rest.size() < 2) {
    return 1;
  }
  auto store = open_writer(rest[0], rest[1]);
  if (!store) {
    std::cout << "held " << code_name(store.error().code) << std::endl;
    return kExitHeld;
  }
  std::cout << "acquired " << store.value()->epoch().value() << std::endl;
  (void)store.value()->close();
  return 0;
}

int command_mutate(const std::vector<std::string>& rest) {
  if (rest.size() < 7) {
    return 1;
  }
  auto store = open_writer(rest[0], rest[1]);
  if (!store) {
    std::cout << "held " << code_name(store.error().code) << std::endl;
    return kExitHeld;
  }
  const auto generation = rackregistry::cli::parse_u64(rest[4], "generation");
  const auto membership = rackregistry::cli::parse_u64(rest[5], "membership-generation");
  const auto receipt =
      store.value()->insert_member(make_insert(rest[2], rest[3], generation.value_or(1),
                                               membership.value_or(0)));
  if (!receipt) {
    rackregistry::cli::print_error(receipt.error());
    return 2;
  }
  write_marker(rest[6], receipt.value().state_digest.to_hex());
  std::cout << receipt.value().to_text() << std::endl;
  (void)store.value()->close();
  return 0;
}

int command_wait_mutate(const std::vector<std::string>& rest) {
  if (rest.size() < 8) {
    return 1;
  }
  auto store = open_writer(rest[0], rest[1]);
  if (!store) {
    std::cout << "held " << code_name(store.error().code) << std::endl;
    return kExitHeld;
  }
  write_marker(rest[7], "ready:" + std::to_string(store.value()->epoch().value()));
  while (!marker_exists(rest[2])) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto generation = rackregistry::cli::parse_u64(rest[5], "generation");
  const auto membership = rackregistry::cli::parse_u64(rest[6], "membership-generation");
  const auto receipt =
      store.value()->insert_member(make_insert(rest[3], rest[4], generation.value_or(1),
                                               membership.value_or(0)));
  if (!receipt) {
    std::cout << "fenced " << code_name(receipt.error().code) << std::endl;
    return kExitFenced;
  }
  write_marker(rest[7], receipt.value().state_digest.to_hex());
  std::cout << "published" << std::endl;
  (void)store.value()->close();
  return 0;
}

int command_adopt(const std::vector<std::string>& rest) {
  if (rest.size() < 2) {
    return 1;
  }
  auto store = open_writer(rest[0], rest[1]);
  if (!store) {
    rackregistry::cli::print_error(store.error());
    return 2;
  }
  std::cout << "adopted epoch " << store.value()->epoch().value() << " racks "
            << store.value()->rack_count() << std::endl;
  (void)store.value()->close();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; ++i) {
    tokens.emplace_back(argv[i]);
  }
  if (tokens.size() < 2) {
    std::cerr << "usage: rr_fence_child <hold|try-open|mutate|wait-mutate|adopt> ...\n";
    return 1;
  }
  const std::string command = tokens.front();
  const std::vector<std::string> rest(tokens.begin() + 1, tokens.end());

  if (command == "hold") {
    return command_hold(rest);
  }
  if (command == "try-open") {
    return command_try_open(rest);
  }
  if (command == "mutate") {
    return command_mutate(rest);
  }
  if (command == "wait-mutate") {
    return command_wait_mutate(rest);
  }
  if (command == "adopt") {
    return command_adopt(rest);
  }
  std::cerr << "unknown subcommand\n";
  return 1;
}
