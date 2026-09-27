// Rack Registry - example: rack lifecycle and membership.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates the stable public mutation surface of RackRegistry: register a
// rack, commission it, insert members, move and replace one, then retire it.
// Every mutation states the generation it expects, and the rejection the
// library produces when that expectation is stale is shown explicitly.

#include <iostream>
#include <string>

#include "rack_registry/rack_registry.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

ProvenanceRecord operator_provenance(std::string_view actor, std::uint64_t sequence) {
  ProvenanceRecord provenance;
  provenance.source = ProvenanceSource::Operator;
  provenance.actor = ActorId::parse(actor).value();
  provenance.source_sequence = sequence;
  provenance.observed_at_unix_ns = 1'800'000'000'000'000'000ull + sequence;
  return provenance;
}

void show(const RackRegistry& registry, const RackId& rack_id) {
  const auto view = registry.rack(rack_id);
  if (!view) {
    std::cerr << "unexpected: " << describe(view.error()) << "\n";
    return;
  }
  std::cout << "  generation=" << view.value().generation().value()
            << " revision=" << view.value().revision().value()
            << " membership_generation=" << view.value().membership_generation().value()
            << " lifecycle=" << lifecycle_state_name(view.value().lifecycle())
            << " members=" << view.value().member_count()
            << " digest=" << view.value().state_digest().to_hex().substr(0, 16) << "...\n";
}

}  // namespace

int main() {
  RackRegistry registry;

  const auto rack_id = RackId::parse("rack:example-a01").value();
  const auto profile_id = CompatibilityProfileId::parse("cp:example-general").value();
  const auto provides = TraitSet::parse("power.ac.208v,cooling.rear-intake,rail.depth.800mm").value();

  // 1. Register. A new rack starts at generation 1, revision 1, membership
  //    generation 0, lifecycle "defined".
  RegisterRackRequest registration;
  registration.structure.id = rack_id;
  registration.structure.unit_count = 12;
  registration.structure.profile.id = profile_id;
  registration.structure.profile.provides = provides;
  registration.structure.power_domains = {
      PowerDomainReference::parse("pdu-a/feed-1").value()};
  registration.structure.cooling_domains = {
      CoolingDomainReference::parse("crac-3/loop-b").value()};
  registration.identity.provenance = operator_provenance("example", 1);
  const auto registered = registry.register_rack(registration);
  std::cout << "registered: " << registered.value().to_text() << "\n";
  show(registry, rack_id);

  // 2. Commission it. The precondition makes this a compare-and-swap.
  TransitionLifecycleRequest commission;
  commission.rack_id = rack_id;
  commission.precondition.expected_generation = registered.value().generation;
  commission.precondition.expected_state = LifecycleState::Defined;
  commission.target = LifecycleState::Commissioned;
  commission.identity.provenance = operator_provenance("example", 2);
  const auto commissioned = registry.transition_lifecycle(commission);
  std::cout << "commissioned: " << commissioned.value().to_text() << "\n";

  // 3. Insert two members. Members state what they need from the rack, and the
  //    library rejects the placement when the profile cannot satisfy it.
  InsertMemberRequest first;
  first.rack_id = rack_id;
  first.precondition.expected_generation = commissioned.value().generation;
  first.precondition.expected_membership_generation = commissioned.value().membership_generation;
  first.member_id = RackMemberId::parse("rm:compute-01").value();
  first.asset_id = AssetId::parse("asset:server-0001").value();
  first.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 2).value()).value();
  first.requirements.required = TraitSet::parse("power.ac.208v,rail.depth.800mm").value();
  first.identity.provenance = operator_provenance("example", 3);
  first.identity.request_id = RequestId::parse("example-insert-1").value();
  const auto inserted = registry.insert_member(first);
  std::cout << "inserted:   " << inserted.value().to_text() << "\n";

  // 3a. Retrying the same request identity is answered from the idempotency
  //     table instead of being applied a second time.
  const auto replayed = registry.insert_member(first);
  std::cout << "replayed:   " << replayed.value().to_text() << "\n";

  InsertMemberRequest incompatible = first;
  incompatible.member_id = RackMemberId::parse("rm:compute-02").value();
  incompatible.asset_id = AssetId::parse("asset:server-0002").value();
  incompatible.mount = MountSpan::full_units(RackUnitRange::inclusive(3, 4).value()).value();
  incompatible.requirements.required = TraitSet::parse("power.dc.48v").value();
  incompatible.precondition.expected_generation = inserted.value().generation;
  incompatible.precondition.expected_membership_generation =
      inserted.value().membership_generation;
  incompatible.identity.request_id.reset();
  const auto rejected = registry.insert_member(incompatible);
  std::cout << "incompatible placement rejected: " << describe(rejected.error()) << "\n";

  // 4. Move the member, then replace its asset in place.
  MoveMemberRequest move;
  move.rack_id = rack_id;
  move.precondition.expected_generation = inserted.value().generation;
  move.precondition.expected_membership_generation = inserted.value().membership_generation;
  move.member_id = first.member_id;
  move.target_mount = MountSpan::full_units(RackUnitRange::inclusive(5, 6).value()).value();
  move.identity.provenance = operator_provenance("example", 4);
  const auto moved = registry.move_member(move);
  std::cout << "moved:      " << moved.value().to_text() << "\n";

  ReplaceMemberRequest replace;
  replace.rack_id = rack_id;
  replace.precondition.expected_generation = moved.value().generation;
  replace.precondition.expected_membership_generation = moved.value().membership_generation;
  replace.member_id = first.member_id;
  replace.replacement_asset = AssetId::parse("asset:server-0009").value();
  replace.requirements.required = TraitSet::parse("power.ac.208v").value();
  replace.target_state = MembershipState::Installed;
  replace.identity.provenance = operator_provenance("example", 5);
  const auto replaced = registry.replace_member(replace);
  std::cout << "replaced:   " << replaced.value().to_text() << "\n";

  // 5. A stale precondition is refused and explains itself.
  MoveMemberRequest stale = move;
  stale.target_mount = MountSpan::full_units(RackUnitRange::inclusive(7, 8).value()).value();
  stale.identity.request_id.reset();
  const auto refused = registry.move_member(stale);
  std::cout << "stale move rejected: " << describe(refused.error()) << "\n";

  // 6. Show the structural occupancy the library reports.
  std::cout << "occupancy:\n";
  for (const OccupancyRecord& entry : registry.occupancy(rack_id).value()) {
    std::cout << "  " << entry.mount.to_text() << " "
              << (entry.units.has_value() ? entry.units->to_text() : std::string("zero-u")) << " "
              << entry.member_id.text() << " asset=" << entry.asset_id.text() << "\n";
  }
  std::cout << "free (descriptive, not capacity):\n";
  for (const FreeSpan& span : registry.free_spans(rack_id).value()) {
    std::cout << "  " << span.to_text() << "\n";
  }

  // 7. Retire the rack. From here every mutation is refused.
  TransitionLifecycleRequest retire;
  retire.rack_id = rack_id;
  retire.precondition.expected_generation = replaced.value().generation;
  retire.precondition.expected_state = LifecycleState::Commissioned;
  retire.target = LifecycleState::Retired;
  retire.identity.provenance = operator_provenance("example", 6);
  const auto retired = registry.transition_lifecycle(retire);
  std::cout << "retired:    " << retired.value().to_text() << "\n";
  show(registry, rack_id);

  InsertMemberRequest after_retirement = incompatible;
  after_retirement.precondition.expected_generation = retired.value().generation;
  after_retirement.precondition.expected_membership_generation =
      retired.value().membership_generation;
  const auto gated = registry.insert_member(after_retirement);
  std::cout << "post-retirement insert rejected: " << describe(gated.error()) << "\n";

  // 8. Diff the retained history.
  const auto diff = registry.diff_generations(rack_id, inserted.value().generation);
  std::cout << "diff from member insertion to now:\n  " << diff.value().to_text() << "\n";
  return 0;
}
