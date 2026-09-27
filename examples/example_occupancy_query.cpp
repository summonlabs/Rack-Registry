// Rack Registry - example: occupancy, shared mounts and free ranges.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates the interval semantics precisely: half-open slot ranges, the
// boundary rule that adjacent ranges never overlap, shared-mount co-occupancy
// with a bounded capacity, and the difference between descriptive free ranges
// and capacity.

#include <iostream>
#include <string>

#include "rack_registry/rack_registry.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

constexpr std::uint64_t kBaseTimeNs = 1'800'000'000'000'000'000ull;

ProvenanceRecord system_provenance(std::uint64_t sequence) {
  ProvenanceRecord provenance;
  provenance.source = ProvenanceSource::System;
  provenance.actor = ActorId::parse("example-occupancy").value();
  provenance.source_sequence = sequence;
  provenance.observed_at_unix_ns = kBaseTimeNs + sequence;
  return provenance;
}

}  // namespace

int main() {
  RackRegistry registry;
  const auto rack_id = RackId::parse("rack:occupancy-a01").value();

  RegisterRackRequest registration;
  registration.structure.id = rack_id;
  registration.structure.unit_count = 6;
  registration.structure.profile.id = CompatibilityProfileId::parse("cp:occupancy").value();
  registration.identity.provenance = system_provenance(1);
  const auto registered = registry.register_rack(registration);

  RackGeneration generation = registered.value().generation;
  MembershipGeneration membership = registered.value().membership_generation;

  const auto insert = [&](std::string_view member, std::string_view asset, const MountSpan& mount,
                          std::uint64_t sequence) {
    InsertMemberRequest request;
    request.rack_id = rack_id;
    request.precondition.expected_generation = generation;
    request.precondition.expected_membership_generation = membership;
    request.member_id = RackMemberId::parse(member).value();
    request.asset_id = AssetId::parse(asset).value();
    request.mount = mount;
    request.identity.provenance = system_provenance(sequence);
    const auto receipt = registry.insert_member(request);
    if (!receipt) {
      std::cout << "  rejected: " << describe(receipt.error()) << "\n";
      return false;
    }
    generation = receipt.value().generation;
    membership = receipt.value().membership_generation;
    return true;
  };

  std::cout << "half-open ranges: unit U2 spans slots "
            << SlotRange::whole_units(RackUnitRange::single(RackUnitIndex::create(2).value()).value())
                   .value()
                   .to_text()
            << "\n";
  std::cout << "                 the lower half of U2 is "
            << SlotRange::half_unit(RackUnitIndex::create(2).value(), HalfSlot::Lower)
                   .value()
                   .to_text()
            << " and the upper half is "
            << SlotRange::half_unit(RackUnitIndex::create(2).value(), HalfSlot::Upper)
                   .value()
                   .to_text()
            << "\n";
  const SlotRange adjacent_left = SlotRange::create(1, 3).value();
  const SlotRange adjacent_right = SlotRange::create(3, 5).value();
  std::cout << "adjacent ranges " << adjacent_left.to_text() << " and "
            << adjacent_right.to_text() << " overlap: "
            << (adjacent_left.overlaps(adjacent_right) ? "yes" : "no") << "\n";

  std::cout << "placements:\n";
  insert("rm:server-a", "asset:server-a",
         MountSpan::full_units(RackUnitRange::inclusive(1, 2).value()).value(), 2);
  insert("rm:server-b", "asset:server-b",
         MountSpan::full_units(RackUnitRange::inclusive(3, 3).value()).value(), 3);
  // Adjacent to the previous member: allowed, because the ranges are half open.
  insert("rm:server-c", "asset:server-c",
         MountSpan::full_units(RackUnitRange::inclusive(4, 4).value()).value(), 4);
  // Two half-unit devices share one rack unit without overlapping.
  insert("rm:half-lower", "asset:half-lower",
         MountSpan::full(SlotRange::half_unit(RackUnitIndex::create(5).value(), HalfSlot::Lower)
                             .value())
             .value(),
         5);
  insert("rm:half-upper", "asset:half-upper",
         MountSpan::full(SlotRange::half_unit(RackUnitIndex::create(5).value(), HalfSlot::Upper)
                             .value())
             .value(),
         6);

  std::cout << "an overlapping placement and a shared mount:\n";
  insert("rm:overlap", "asset:overlap",
         MountSpan::full_units(RackUnitRange::inclusive(2, 3).value()).value(), 7);

  const auto shared_class = SharedMountClass::parse("smc:psu-bay").value();
  const SlotRange shared_span =
      SlotRange::whole_units(RackUnitRange::single(RackUnitIndex::create(6).value()).value())
          .value();
  insert("rm:psu-1", "asset:psu-1",
         MountSpan::shared(shared_span, shared_class, 2).value(), 8);
  insert("rm:psu-2", "asset:psu-2",
         MountSpan::shared(shared_span, shared_class, 2).value(), 9);
  std::cout << "  a third occupant of a two-place shared mount:\n";
  insert("rm:psu-3", "asset:psu-3",
         MountSpan::shared(shared_span, shared_class, 2).value(), 10);
  // A zero-U member occupies no slot and therefore never conflicts.
  insert("rm:cable-manager", "asset:cable-manager", MountSpan::zero_u(), 11);

  std::cout << "occupancy in mount order:\n";
  for (const OccupancyRecord& entry : registry.occupancy(rack_id).value()) {
    std::cout << "  " << entry.mount.to_text() << " "
              << (entry.units.has_value() ? entry.units->to_text() : std::string("zero-u")) << " "
              << entry.member_id.text() << "\n";
  }

  std::cout << "shared availability:\n";
  for (const SharedSpanAvailability& entry : registry.shared_availability(rack_id).value()) {
    std::cout << "  " << entry.to_text() << " remaining=" << entry.remaining() << "\n";
  }

  std::cout << "free structural ranges (descriptive, not a capacity commitment):\n";
  for (const FreeSpan& span : registry.free_spans(rack_id).value()) {
    std::cout << "  " << span.to_text() << "\n";
  }

  std::cout << "member enumeration is deterministic in both orders:\n";
  for (const MemberRecord& member :
       registry.members(rack_id, MemberOrder::MountOrder).value()) {
    std::cout << "  mount order    " << member.member_id.text() << "\n";
  }
  for (const MemberRecord& member :
       registry.members(rack_id, MemberOrder::IdentityOrder).value()) {
    std::cout << "  identity order " << member.member_id.text() << "\n";
  }

  std::cout << "rack digest " << registry.rack(rack_id).value().state_digest().to_hex() << "\n";
  return 0;
}
