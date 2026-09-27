// Rack Registry - example: durable store, publication and recovery.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates the durable surface: open a store, publish mutations
// transactionally, close it, reopen it, verify that the authoritative
// generation survived, and diff the current generation against the retained
// previous one. The store path is taken from the command line so the example
// never writes into the source tree by accident.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "rack_registry/rack_registry.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

ProvenanceRecord system_provenance(std::uint64_t sequence) {
  ProvenanceRecord provenance;
  provenance.source = ProvenanceSource::System;
  provenance.actor = ActorId::parse("example-durable").value();
  provenance.source_sequence = sequence;
  provenance.observed_at_unix_ns = 1'800'000'000'000'000'000ull + sequence;
  return provenance;
}

StoreOptions store_options(const std::filesystem::path& path, WriterId writer) {
  StoreOptions options;
  options.path = path;
  options.writer_id = std::move(writer);
  options.producer = SourceReference::parse("example_durable_store/1.0.0").value();
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path path =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("example-durable.rrstate");
  std::cout << "state file: " << path.string() << "\n";

  const auto rack_id = RackId::parse("rack:durable-a01").value();
  const auto writer = WriterId::parse("wr:example").value();
  const auto producer = SourceReference::parse("example_durable_store/1.0.0").value();

  // 1. Publish two generations.
  {
    auto store = RackStore::open(store_options(path, writer));
    if (!store) {
      std::cerr << "open failed: " << describe(store.error()) << "\n";
      return 1;
    }
    std::cout << "recovery: " << store.value()->recovery().to_text() << "\n";
    std::cout << "writer:   " << store.value()->writer_lock().to_text() << "\n";

    RegisterRackRequest registration;
    registration.structure.id = rack_id;
    registration.structure.unit_count = 8;
    registration.structure.profile.id = CompatibilityProfileId::parse("cp:durable").value();
    registration.structure.profile.provides =
        TraitSet::parse("power.ac.208v,rail.depth.800mm").value();
    registration.identity.provenance = system_provenance(1);

    const auto registered = store.value()->register_rack(registration);
    if (!registered) {
      std::cerr << "register rejected: " << describe(registered.error()) << "\n";
      return 1;
    }
    std::cout << "published: " << registered.value().to_text() << "\n";

    InsertMemberRequest insert;
    insert.rack_id = rack_id;
    insert.precondition.expected_generation = registered.value().generation;
    insert.precondition.expected_membership_generation = registered.value().membership_generation;
    insert.member_id = RackMemberId::parse("rm:durable-01").value();
    insert.asset_id = AssetId::parse("asset:durable-0001").value();
    insert.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 2).value()).value();
    insert.requirements.required = TraitSet::parse("power.ac.208v").value();
    insert.identity.provenance = system_provenance(2);

    const auto published = store.value()->insert_member(insert);
    if (!published) {
      std::cerr << "insert rejected: " << describe(published.error()) << "\n";
      return 1;
    }
    std::cout << "published: " << published.value().to_text() << "\n";
    std::cout << "sequence:  " << store.value()->sequence().value() << "\n";
    std::cout << "epoch:     " << store.value()->epoch().value() << "\n";
  }

  // 2. Reopen: the generation must survive, and the epoch must advance
  //    monotonically because a new writer now owns the store.
  {
    auto store = RackStore::open(store_options(path, writer));
    if (!store) {
      std::cerr << "reopen failed: " << describe(store.error()) << "\n";
      return 1;
    }
    std::cout << "recovered: " << store.value()->recovery().to_text() << "\n";
    std::cout << "epoch now: " << store.value()->epoch().value() << "\n";
    const auto view = store.value()->rack(rack_id);
    if (!view) {
      std::cerr << "rack missing after reopen\n";
      return 1;
    }
    std::cout << "rack survived with generation " << view.value().generation().value()
              << ", members " << view.value().member_count() << "\n";

    // 3. Publish one more generation and diff it against the retained previous
    //    publication.
    MoveMemberRequest move;
    move.rack_id = rack_id;
    move.precondition.expected_generation = view.value().generation();
    move.precondition.expected_membership_generation = view.value().membership_generation();
    move.member_id = RackMemberId::parse("rm:durable-01").value();
    move.target_mount = MountSpan::full_units(RackUnitRange::inclusive(3, 4).value()).value();
    move.identity.provenance = system_provenance(3);
    const auto moved = store.value()->move_member(move);
    if (!moved) {
      std::cerr << "move rejected: " << describe(moved.error()) << "\n";
      return 1;
    }
    std::cout << "published: " << moved.value().to_text() << "\n";

    const auto diff = store.value()->diff_with_previous();
    if (diff) {
      std::cout << "diff against previous publication:\n  " << diff.value().to_text() << "\n";
    }

    // 4. A read-only store can be opened while a writer owns the store, and it
    //    refuses mutation.
    StoreOptions read_only = store_options(path, WriterId{});
    read_only.read_only = true;
    read_only.producer = producer;
    auto reader = RackStore::open(read_only);
    if (!reader) {
      std::cerr << "read-only open failed: " << describe(reader.error()) << "\n";
      return 1;
    }
    std::cout << "read-only view: racks=" << reader.value()->rack_count()
              << " holds_authority=" << (reader.value()->holds_writer_authority() ? "yes" : "no")
              << "\n";
    const auto refused = reader.value()->register_rack(RegisterRackRequest{});
    std::cout << "read-only mutation rejected: " << code_name(refused.error().code) << "\n";
  }

  // 5. Verify the published file without taking authority.
  const auto info = RackStore::inspect(path);
  if (!info) {
    std::cerr << "verify failed: " << describe(info.error()) << "\n";
    return 1;
  }
  std::cout << "verified: " << info.value().to_text() << "\n";
  return 0;
}
