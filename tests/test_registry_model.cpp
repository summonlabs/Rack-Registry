// Rack Registry - authoritative model tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

RackId rack(std::string_view body) { return RackId::parse("rack:" + std::string(body)).value(); }
RackMemberId member(std::string_view body) {
  return RackMemberId::parse("rm:" + std::string(body)).value();
}
AssetId asset(std::string_view body) { return AssetId::parse("asset:" + std::string(body)).value(); }

RegisterRackRequest registration(std::string_view body, std::uint32_t units,
                                 std::string_view provides = "power.ac.208v") {
  RegisterRackRequest request;
  request.structure.id = rack(body);
  request.structure.unit_count = units;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:test").value();
  request.structure.profile.provides = TraitSet::parse(provides).value();
  request.identity.provenance = rrtest::provenance_for("model-test", 1);
  return request;
}

struct Fixture {
  RackRegistry registry{};
  RackId rack_id = rack("a01");
  RackGeneration generation{};
  MembershipGeneration membership{};
};

// Registers a rack and reports the resulting counters.
void build(Fixture& fixture, std::uint32_t units = 6,
           std::string_view provides = "power.ac.208v") {
  const auto receipt = fixture.registry.register_rack(registration("a01", units, provides));
  RR_REQUIRE_OK(receipt);
  fixture.generation = receipt.value().generation;
  fixture.membership = receipt.value().membership_generation;
}

Result<MutationReceipt> insert(Fixture& fixture, std::string_view member_body,
                               std::string_view asset_body, const MountSpan& mount,
                               std::string_view requires_traits = "",
                               MembershipState state = MembershipState::Installed) {
  InsertMemberRequest request;
  request.rack_id = fixture.rack_id;
  request.precondition.expected_generation = fixture.generation;
  request.precondition.expected_membership_generation = fixture.membership;
  request.member_id = member(member_body);
  request.asset_id = asset(asset_body);
  request.mount = mount;
  request.state = state;
  request.requirements.required = TraitSet::parse(requires_traits).value();
  request.identity.provenance = rrtest::provenance_for("model-test", 10);
  return fixture.registry.insert_member(request);
}

MountSpan units_span(std::uint32_t first, std::uint32_t last) {
  return MountSpan::full_units(RackUnitRange::inclusive(first, last).value()).value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration and structure
// ---------------------------------------------------------------------------

RR_TEST(registering_a_rack_establishes_initial_counters) {
  Fixture fixture;
  build(fixture);
  const auto view = fixture.registry.rack(fixture.rack_id);
  RR_REQUIRE_OK(view);
  RR_CHECK_EQ(view.value().generation().value(), 1u);
  RR_CHECK_EQ(view.value().revision().value(), 1u);
  RR_CHECK_EQ(view.value().membership_generation().value(), 0u);
  RR_CHECK(view.value().lifecycle() == LifecycleState::Defined);
  RR_CHECK_EQ(view.value().member_count(), std::size_t{0});
  RR_CHECK_EQ(fixture.registry.rack_count(), std::size_t{1});
  RR_CHECK(fixture.registry.contains_rack(fixture.rack_id));
  RR_CHECK(!fixture.registry.contains_rack(rack("missing")));
}

RR_TEST(registering_the_same_rack_twice_is_refused) {
  Fixture fixture;
  build(fixture);
  const auto again = fixture.registry.register_rack(registration("a01", 6));
  RR_REQUIRE_CODE(again, ErrorCode::DuplicateRackId);
  RR_CHECK_EQ(fixture.registry.rack_count(), std::size_t{1});
}

RR_TEST(registration_rejects_impossible_definitions) {
  RackRegistry registry;
  RegisterRackRequest request = registration("a01", 6);
  request.structure.id = RackId{};
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::EmptyValue);

  request = registration("a01", 0);
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::InvalidRange);

  request = registration("a01", kMaxRackUnits + 1);
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::InvalidRange);

  request = registration("a01", 6);
  request.structure.profile.id = CompatibilityProfileId{};
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::EmptyValue);

  request = registration("a01", 6);
  request.identity.provenance.actor = ActorId{};
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::EmptyValue);

  request = registration("a01", 6);
  request.structure.power_domains = {PowerDomainReference::parse("pdu-a").value(),
                                     PowerDomainReference::parse("pdu-a").value()};
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::DuplicateDomainReference);

  request = registration("a01", 6);
  request.structure.power_domains = {PowerDomainReference::parse("pdu-b").value(),
                                     PowerDomainReference::parse("pdu-a").value()};
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::InvalidStateEncoding);

  RR_CHECK_EQ(registry.rack_count(), std::size_t{0});
}

RR_TEST(structure_replacement_advances_generation_and_revision) {
  Fixture fixture;
  build(fixture);

  SetRackStructureRequest request;
  request.rack_id = fixture.rack_id;
  request.precondition.expected_generation = fixture.generation;
  request.unit_count = 10;
  request.profile.id = CompatibilityProfileId::parse("cp:test").value();
  request.profile.provides = TraitSet::parse("power.ac.208v,rail.depth.800mm").value();
  request.power_domains = {PowerDomainReference::parse("pdu-a").value()};
  request.cooling_domains = {CoolingDomainReference::parse("crac-1").value()};
  request.label = DisplayLabel::parse("Rack A01").value();
  request.identity.provenance = rrtest::provenance_for("model-test", 2);

  const auto receipt = fixture.registry.set_rack_structure(request);
  RR_REQUIRE_OK(receipt);
  RR_CHECK_EQ(receipt.value().generation.value(), 2u);
  RR_CHECK_EQ(receipt.value().revision.value(), 2u);
  RR_CHECK_EQ(receipt.value().membership_generation.value(), 0u);

  const auto view = fixture.registry.rack(fixture.rack_id);
  RR_REQUIRE_OK(view);
  RR_CHECK_EQ(view.value().unit_count(), 10u);
  RR_CHECK_EQ(view.value().label().text(), std::string("Rack A01"));
  RR_CHECK_EQ(view.value().power_domains().size(), std::size_t{1});

  const auto stale = fixture.registry.set_rack_structure(request);
  RR_REQUIRE_CODE(stale, ErrorCode::StaleRackGeneration);
}

RR_TEST(shrinking_a_rack_below_an_existing_member_is_refused) {
  Fixture fixture;
  build(fixture, 10);
  const auto inserted = insert(fixture, "m1", "s1", units_span(9, 10));
  RR_REQUIRE_OK(inserted);
  fixture.generation = inserted.value().generation;
  fixture.membership = inserted.value().membership_generation;

  SetRackStructureRequest shrink;
  shrink.rack_id = fixture.rack_id;
  shrink.precondition.expected_generation = fixture.generation;
  shrink.unit_count = 8;
  shrink.profile = fixture.registry.rack(fixture.rack_id).value().profile();
  shrink.identity.provenance = rrtest::provenance_for("model-test", 3);
  RR_REQUIRE_CODE(fixture.registry.set_rack_structure(shrink), ErrorCode::RackExtentWouldEvictMembers);

  // The same shrink succeeds when the member is gone.
  RemoveMemberRequest removal;
  removal.rack_id = fixture.rack_id;
  removal.precondition.expected_generation = fixture.generation;
  removal.precondition.expected_membership_generation = fixture.membership;
  removal.member_id = member("m1");
  removal.identity.provenance = rrtest::provenance_for("model-test", 4);
  const auto removed = fixture.registry.remove_member(removal);
  RR_REQUIRE_OK(removed);
  shrink.precondition.expected_generation = removed.value().generation;
  RR_REQUIRE_OK(fixture.registry.set_rack_structure(shrink));
}

RR_TEST(a_profile_change_that_strands_a_member_is_refused) {
  Fixture fixture;
  build(fixture, 6, "power.ac.208v");
  const auto inserted = insert(fixture, "m1", "s1", units_span(1, 1), "power.ac.208v");
  RR_REQUIRE_OK(inserted);

  SetRackStructureRequest request;
  request.rack_id = fixture.rack_id;
  request.precondition.expected_generation = inserted.value().generation;
  request.unit_count = 6;
  request.profile.id = CompatibilityProfileId::parse("cp:test").value();
  request.profile.provides = TraitSet::parse("power.dc.48v").value();
  request.identity.provenance = rrtest::provenance_for("model-test", 5);
  RR_REQUIRE_CODE(fixture.registry.set_rack_structure(request), ErrorCode::CompatibilityUnsatisfied);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RR_TEST(lifecycle_transitions_follow_the_table) {
  Fixture fixture;
  build(fixture);

  const auto illegal = [&](LifecycleState expected_state, LifecycleState target) {
    TransitionLifecycleRequest request;
    request.rack_id = fixture.rack_id;
    request.precondition.expected_generation = fixture.generation;
    request.precondition.expected_state = expected_state;
    request.target = target;
    request.identity.provenance = rrtest::provenance_for("model-test", 6);
    return fixture.registry.transition_lifecycle(request);
  };

  // Defined -> Active is not in the table.
  RR_REQUIRE_CODE(illegal(LifecycleState::Defined, LifecycleState::Active),
                  ErrorCode::LifecycleTransitionNotAllowed);

  auto commission = illegal(LifecycleState::Defined, LifecycleState::Commissioned);
  RR_REQUIRE_OK(commission);
  fixture.generation = commission.value().generation;

  // A stale expected state is refused even when the target is legal.
  RR_REQUIRE_CODE(illegal(LifecycleState::Defined, LifecycleState::Active),
                  ErrorCode::StaleLifecycleState);

  auto activate = illegal(LifecycleState::Commissioned, LifecycleState::Active);
  RR_REQUIRE_OK(activate);
  fixture.generation = activate.value().generation;

  auto maintain = illegal(LifecycleState::Active, LifecycleState::Maintenance);
  RR_REQUIRE_OK(maintain);
  fixture.generation = maintain.value().generation;

  // Maintenance may not change the physical structure but may change domains.
  SetRackStructureRequest structure;
  structure.rack_id = fixture.rack_id;
  structure.precondition.expected_generation = fixture.generation;
  structure.unit_count = 8;
  structure.profile = fixture.registry.rack(fixture.rack_id).value().profile();
  structure.identity.provenance = rrtest::provenance_for("model-test", 7);
  RR_REQUIRE_CODE(fixture.registry.set_rack_structure(structure),
                  ErrorCode::LifecycleMutationForbidden);

  // Retiring is allowed from Maintenance and freezes the rack.
  auto retire = illegal(LifecycleState::Maintenance, LifecycleState::Retired);
  RR_REQUIRE_OK(retire);
  fixture.generation = retire.value().generation;
  RR_CHECK(is_immutable(LifecycleState::Retired));

  const auto frozen = illegal(LifecycleState::Retired, LifecycleState::Active);
  RR_REQUIRE_CODE(frozen, ErrorCode::LifecycleTransitionNotAllowed);
}

RR_TEST(a_retired_rack_refuses_every_mutation) {
  Fixture fixture;
  build(fixture);

  TransitionLifecycleRequest retire;
  retire.rack_id = fixture.rack_id;
  retire.precondition.expected_generation = fixture.generation;
  retire.precondition.expected_state = LifecycleState::Defined;
  retire.target = LifecycleState::Retired;
  retire.identity.provenance = rrtest::provenance_for("model-test", 8);
  const auto retired = fixture.registry.transition_lifecycle(retire);
  RR_REQUIRE_OK(retired);
  fixture.generation = retired.value().generation;
  fixture.membership = retired.value().membership_generation;

  const auto inserted = insert(fixture, "m1", "s1", units_span(1, 1));
  RR_REQUIRE_CODE(inserted, ErrorCode::LifecycleMutationForbidden);

  RemoveMemberRequest removal;
  removal.rack_id = fixture.rack_id;
  removal.precondition.expected_generation = fixture.generation;
  removal.precondition.expected_membership_generation = fixture.membership;
  removal.member_id = member("m1");
  removal.identity.provenance = rrtest::provenance_for("model-test", 9);
  RR_REQUIRE_CODE(fixture.registry.remove_member(removal), ErrorCode::LifecycleMutationForbidden);

  SetRackStructureRequest structure;
  structure.rack_id = fixture.rack_id;
  structure.precondition.expected_generation = fixture.generation;
  structure.unit_count = 6;
  structure.profile = fixture.registry.rack(fixture.rack_id).value().profile();
  structure.power_domains = {PowerDomainReference::parse("pdu-a").value()};
  structure.identity.provenance = rrtest::provenance_for("model-test", 10);
  RR_REQUIRE_CODE(fixture.registry.set_rack_structure(structure),
                  ErrorCode::LifecycleMutationForbidden);

  // The rack is still fully readable.
  RR_REQUIRE_OK(fixture.registry.rack(fixture.rack_id));
}

// ---------------------------------------------------------------------------
// Membership
// ---------------------------------------------------------------------------

RR_TEST(membership_mutations_advance_both_counters) {
  Fixture fixture;
  build(fixture, 6);

  const auto first = insert(fixture, "m1", "s1", units_span(1, 2));
  RR_REQUIRE_OK(first);
  RR_CHECK_EQ(first.value().generation.value(), 2u);
  RR_CHECK_EQ(first.value().membership_generation.value(), 1u);
  RR_CHECK_EQ(first.value().revision.value(), 1u);

  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  MoveMemberRequest move;
  move.rack_id = fixture.rack_id;
  move.precondition.expected_generation = fixture.generation;
  move.precondition.expected_membership_generation = fixture.membership;
  move.member_id = member("m1");
  move.target_mount = units_span(3, 4);
  move.identity.provenance = rrtest::provenance_for("model-test", 11);
  const auto moved = fixture.registry.move_member(move);
  RR_REQUIRE_OK(moved);
  RR_CHECK_EQ(moved.value().membership_generation.value(), 2u);
  RR_CHECK_EQ(moved.value().revision.value(), 1u);

  fixture.generation = moved.value().generation;
  fixture.membership = moved.value().membership_generation;

  ReplaceMemberRequest replace;
  replace.rack_id = fixture.rack_id;
  replace.precondition.expected_generation = fixture.generation;
  replace.precondition.expected_membership_generation = fixture.membership;
  replace.member_id = member("m1");
  replace.replacement_asset = asset("s2");
  replace.identity.provenance = rrtest::provenance_for("model-test", 12);
  const auto replaced = fixture.registry.replace_member(replace);
  RR_REQUIRE_OK(replaced);
  RR_CHECK_EQ(replaced.value().membership_generation.value(), 3u);

  fixture.generation = replaced.value().generation;
  fixture.membership = replaced.value().membership_generation;

  const auto stored = fixture.registry.member(fixture.rack_id, member("m1"));
  RR_REQUIRE_OK(stored);
  RR_REQUIRE(stored.value().has_value());
  RR_CHECK_EQ(stored.value()->asset_id.text(), std::string("asset:s2"));
  RR_CHECK_EQ(stored.value()->mount.to_text(), std::string("full:[5,9)"));
}

RR_TEST(overlapping_and_out_of_bounds_placements_are_refused) {
  Fixture fixture;
  build(fixture, 6);

  const auto first = insert(fixture, "m1", "s1", units_span(2, 3));
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  const auto overlap = insert(fixture, "m2", "s2", units_span(3, 4));
  RR_REQUIRE_CODE(overlap, ErrorCode::OccupancyOverlap);

  const auto outside = insert(fixture, "m3", "s3", units_span(6, 7));
  RR_REQUIRE_CODE(outside, ErrorCode::MountOutOfBounds);

  // A half-unit neighbour of the same unit is legal when it does not overlap.
  const auto half = insert(fixture, "m4", "s4",
                           MountSpan::full(SlotRange::half_unit(
                                                       RackUnitIndex::create(1).value(),
                                                       HalfSlot::Upper)
                                               .value())
                               .value());
  RR_REQUIRE_OK(half);
}

RR_TEST(shared_mounts_enforce_class_and_capacity) {
  Fixture fixture;
  build(fixture, 6);

  const SlotRange span = SlotRange::whole_units(RackUnitRange::single(
                                                    RackUnitIndex::create(6).value())
                                                    .value())
                             .value();
  const SharedMountClass bay = SharedMountClass::parse("smc:bay").value();

  const auto first = insert(fixture, "p1", "s1", MountSpan::shared(span, bay, 2).value());
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  const auto second = insert(fixture, "p2", "s2", MountSpan::shared(span, bay, 2).value());
  RR_REQUIRE_OK(second);
  fixture.generation = second.value().generation;
  fixture.membership = second.value().membership_generation;

  const auto third = insert(fixture, "p3", "s3", MountSpan::shared(span, bay, 2).value());
  RR_REQUIRE_CODE(third, ErrorCode::SharedMountCapacityExceeded);

  const auto other_class =
      insert(fixture, "p4", "s4",
             MountSpan::shared(span, SharedMountClass::parse("smc:other").value(), 2).value());
  RR_REQUIRE_CODE(other_class, ErrorCode::OccupancyOverlap);

  const auto other_capacity = insert(fixture, "p5", "s5", MountSpan::shared(span, bay, 4).value());
  RR_REQUIRE_CODE(other_capacity, ErrorCode::SharedMountClassMismatch);

  const auto exclusive = insert(fixture, "p6", "s6", MountSpan::full(span).value());
  RR_REQUIRE_CODE(exclusive, ErrorCode::OccupancyOverlap);

  const auto availability = fixture.registry.shared_availability(fixture.rack_id);
  RR_REQUIRE_OK(availability);
  RR_CHECK_EQ(availability.value().size(), std::size_t{1});
  RR_CHECK_EQ(availability.value()[0].occupied, 2u);
  RR_CHECK_EQ(availability.value()[0].remaining(), 0u);
}

RR_TEST(zero_u_members_never_conflict) {
  Fixture fixture;
  build(fixture, 2);
  auto receipt = insert(fixture, "z1", "s1", MountSpan::zero_u());
  RR_REQUIRE_OK(receipt);
  fixture.generation = receipt.value().generation;
  fixture.membership = receipt.value().membership_generation;

  receipt = insert(fixture, "z2", "s2", MountSpan::zero_u());
  RR_REQUIRE_OK(receipt);
  fixture.generation = receipt.value().generation;
  fixture.membership = receipt.value().membership_generation;

  receipt = insert(fixture, "m1", "s3", units_span(1, 2));
  RR_REQUIRE_OK(receipt);

  const auto free = fixture.registry.free_spans(fixture.rack_id);
  RR_REQUIRE_OK(free);
  RR_CHECK(free.value().empty());

  const auto occupancy = fixture.registry.occupancy(fixture.rack_id);
  RR_REQUIRE_OK(occupancy);
  RR_CHECK_EQ(occupancy.value().size(), std::size_t{3});
  // Zero-U members sort last in mount order.
  RR_CHECK(occupancy.value().back().mount.is_zero_u());
}

RR_TEST(one_authoritative_placement_per_asset_in_a_rack) {
  Fixture fixture;
  build(fixture, 6);
  const auto first = insert(fixture, "m1", "shared-asset", units_span(1, 1));
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  const auto duplicate = insert(fixture, "m2", "shared-asset", units_span(2, 2));
  RR_REQUIRE_CODE(duplicate, ErrorCode::DuplicateAssetPlacement);

  const auto duplicate_member = insert(fixture, "m1", "other-asset", units_span(2, 2));
  RR_REQUIRE_CODE(duplicate_member, ErrorCode::DuplicateMemberId);

  InsertMemberRequest empty_member;
  empty_member.rack_id = fixture.rack_id;
  empty_member.precondition.expected_generation = fixture.generation;
  empty_member.precondition.expected_membership_generation = fixture.membership;
  empty_member.asset_id = asset("a");
  empty_member.mount = units_span(2, 2);
  empty_member.identity.provenance = rrtest::provenance_for("model-test", 13);
  RR_REQUIRE_CODE(fixture.registry.insert_member(empty_member), ErrorCode::EmptyValue);

  InsertMemberRequest empty_asset = empty_member;
  empty_asset.member_id = member("m9");
  empty_asset.asset_id = AssetId{};
  RR_REQUIRE_CODE(fixture.registry.insert_member(empty_asset), ErrorCode::EmptyValue);
}

RR_TEST(stale_preconditions_are_refused_before_anything_changes) {
  Fixture fixture;
  build(fixture, 6);
  const auto first = insert(fixture, "m1", "s1", units_span(1, 1));
  RR_REQUIRE_OK(first);

  InsertMemberRequest request;
  request.rack_id = fixture.rack_id;
  request.precondition.expected_generation = fixture.generation;  // stale: the insert advanced it
  request.precondition.expected_membership_generation = fixture.membership;
  request.member_id = member("m2");
  request.asset_id = asset("s2");
  request.mount = units_span(2, 2);
  request.identity.provenance = rrtest::provenance_for("model-test", 14);
  RR_REQUIRE_CODE(fixture.registry.insert_member(request), ErrorCode::StaleRackGeneration);

  request.precondition.expected_generation = first.value().generation;
  RR_REQUIRE_CODE(fixture.registry.insert_member(request), ErrorCode::StaleMembershipGeneration);

  request.precondition.expected_membership_generation = first.value().membership_generation;
  RR_REQUIRE_OK(fixture.registry.insert_member(request));
  RR_CHECK_EQ(fixture.registry.rack(fixture.rack_id).value().member_count(), std::size_t{2});
}

RR_TEST(moving_a_member_onto_its_own_span_is_permitted_and_atomic) {
  Fixture fixture;
  build(fixture, 6);
  const auto first = insert(fixture, "m1", "s1", units_span(1, 2));
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  MoveMemberRequest move;
  move.rack_id = fixture.rack_id;
  move.precondition.expected_generation = fixture.generation;
  move.precondition.expected_membership_generation = fixture.membership;
  move.member_id = member("m1");
  move.target_mount = units_span(1, 2);
  move.identity.provenance = rrtest::provenance_for("model-test", 15);
  const auto moved = fixture.registry.move_member(move);
  RR_REQUIRE_OK(moved);
  RR_CHECK_EQ(moved.value().membership_generation.value(), 2u);

  // Moving an unknown member is refused and changes nothing.
  MoveMemberRequest unknown = move;
  unknown.precondition = MembershipPrecondition{moved.value().generation,
                                                 moved.value().membership_generation};
  unknown.member_id = member("nope");
  RR_REQUIRE_CODE(fixture.registry.move_member(unknown), ErrorCode::UnknownMemberId);
  RR_CHECK_EQ(fixture.registry.rack(fixture.rack_id).value().generation().value(),
              moved.value().generation.value());
}

RR_TEST(replacement_keeps_identity_and_mount_and_validates_requirements) {
  Fixture fixture;
  build(fixture, 6, "power.ac.208v");
  const auto first = insert(fixture, "m1", "s1", units_span(2, 2), "power.ac.208v");
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  ReplaceMemberRequest replace;
  replace.rack_id = fixture.rack_id;
  replace.precondition.expected_generation = fixture.generation;
  replace.precondition.expected_membership_generation = fixture.membership;
  replace.member_id = member("m1");
  replace.replacement_asset = asset("s2");
  replace.requirements.required = TraitSet::parse("power.dc.48v").value();
  replace.identity.provenance = rrtest::provenance_for("model-test", 16);
  RR_REQUIRE_CODE(fixture.registry.replace_member(replace), ErrorCode::CompatibilityUnsatisfied);

  replace.requirements.required = TraitSet::parse("power.ac.208v").value();
  const auto replaced = fixture.registry.replace_member(replace);
  RR_REQUIRE_OK(replaced);
  const auto stored = fixture.registry.member(fixture.rack_id, member("m1"));
  RR_REQUIRE_OK(stored);
  RR_REQUIRE(stored.value().has_value());
  RR_CHECK_EQ(stored.value()->member_id.text(), std::string("rm:m1"));
  RR_CHECK_EQ(stored.value()->asset_id.text(), std::string("asset:s2"));
  RR_CHECK_EQ(stored.value()->mount.to_text(), std::string("full:[3,5)"));
}

RR_TEST(reserved_members_hold_their_coordinate) {
  Fixture fixture;
  build(fixture, 4);
  const auto reserved =
      insert(fixture, "r1", "s1", units_span(1, 2), "", MembershipState::Reserved);
  RR_REQUIRE_OK(reserved);
  fixture.generation = reserved.value().generation;
  fixture.membership = reserved.value().membership_generation;

  const auto clash = insert(fixture, "m1", "s2", units_span(2, 3));
  RR_REQUIRE_CODE(clash, ErrorCode::OccupancyOverlap);

  ReplaceMemberRequest install;
  install.rack_id = fixture.rack_id;
  install.precondition.expected_generation = fixture.generation;
  install.precondition.expected_membership_generation = fixture.membership;
  install.member_id = member("r1");
  install.replacement_asset = asset("s3");
  install.target_state = MembershipState::Installed;
  install.identity.provenance = rrtest::provenance_for("model-test", 17);
  const auto installed = fixture.registry.replace_member(install);
  RR_REQUIRE_OK(installed);
  const auto stored = fixture.registry.member(fixture.rack_id, member("r1"));
  RR_REQUIRE_OK(stored);
  RR_REQUIRE(stored.value().has_value());
  RR_CHECK(stored.value()->state == MembershipState::Installed);
}

// ---------------------------------------------------------------------------
// Idempotency, determinism and diffing
// ---------------------------------------------------------------------------

RR_TEST(request_identity_makes_retries_idempotent) {
  Fixture fixture;
  build(fixture, 6);

  InsertMemberRequest request;
  request.rack_id = fixture.rack_id;
  request.precondition.expected_generation = fixture.generation;
  request.precondition.expected_membership_generation = fixture.membership;
  request.member_id = member("m1");
  request.asset_id = asset("s1");
  request.mount = units_span(1, 1);
  request.identity.provenance = rrtest::provenance_for("model-test", 18);
  request.identity.request_id = RequestId::parse("request-1").value();

  const auto applied = fixture.registry.insert_member(request);
  RR_REQUIRE_OK(applied);
  RR_CHECK(!applied.value().replayed);

  // The exact same request, with the same stale precondition, is answered from
  // the idempotency table rather than reapplied.
  const auto replayed = fixture.registry.insert_member(request);
  RR_REQUIRE_OK(replayed);
  RR_CHECK(replayed.value().replayed);
  RR_CHECK_EQ(replayed.value().generation.value(), applied.value().generation.value());
  RR_CHECK_EQ(fixture.registry.rack(fixture.rack_id).value().member_count(), std::size_t{1});

  // Reusing the identity with different content is a conflict.
  InsertMemberRequest conflicting = request;
  conflicting.mount = units_span(2, 2);
  RR_REQUIRE_CODE(fixture.registry.insert_member(conflicting), ErrorCode::RequestIdConflict);

  // A different request identity with the same content is a fresh application
  // and is refused because the member already exists.
  InsertMemberRequest other = request;
  other.identity.request_id = RequestId::parse("request-2").value();
  other.precondition.expected_generation = applied.value().generation;
  other.precondition.expected_membership_generation = applied.value().membership_generation;
  RR_REQUIRE_CODE(fixture.registry.insert_member(other), ErrorCode::DuplicateMemberId);
}

RR_TEST(enumeration_order_is_total_and_independent_of_insertion_history) {
  Fixture fixture;
  build(fixture, 8);

  const std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>> placements = {
      {"zeta", {7, 7}}, {"alpha", {1, 1}}, {"mid", {4, 4}}, {"beta", {2, 2}}};
  for (const auto& [name, span] : placements) {
    const auto receipt = insert(fixture, name, name, units_span(span.first, span.second));
    RR_REQUIRE_OK(receipt);
    fixture.generation = receipt.value().generation;
    fixture.membership = receipt.value().membership_generation;
  }

  const auto by_mount = fixture.registry.members(fixture.rack_id, MemberOrder::MountOrder);
  RR_REQUIRE_OK(by_mount);
  RR_CHECK_EQ(by_mount.value().size(), std::size_t{4});
  RR_CHECK_EQ(by_mount.value()[0].member_id.text(), std::string("rm:alpha"));
  RR_CHECK_EQ(by_mount.value()[1].member_id.text(), std::string("rm:beta"));
  RR_CHECK_EQ(by_mount.value()[2].member_id.text(), std::string("rm:mid"));
  RR_CHECK_EQ(by_mount.value()[3].member_id.text(), std::string("rm:zeta"));

  const auto by_identity = fixture.registry.members(fixture.rack_id, MemberOrder::IdentityOrder);
  RR_REQUIRE_OK(by_identity);
  RR_CHECK_EQ(by_identity.value()[0].member_id.text(), std::string("rm:alpha"));
  RR_CHECK_EQ(by_identity.value()[3].member_id.text(), std::string("rm:zeta"));

  // Repeated enumeration returns identical sequences.
  const auto again = fixture.registry.members(fixture.rack_id, MemberOrder::MountOrder);
  RR_REQUIRE_OK(again);
  RR_CHECK_EQ(again.value().size(), by_mount.value().size());
  for (std::size_t i = 0; i < again.value().size(); ++i) {
    RR_CHECK_EQ(again.value()[i].member_id.text(), by_mount.value()[i].member_id.text());
  }
}

RR_TEST(rack_enumeration_is_ordered_by_identity) {
  RackRegistry registry;
  for (const std::string body : {"c-rack", "a-rack", "b-rack"}) {
    RR_REQUIRE_OK(registry.register_rack(registration(body, 4)));
  }
  const std::vector<RackView> racks = registry.racks();
  RR_CHECK_EQ(racks.size(), std::size_t{3});
  RR_CHECK_EQ(racks[0].id().text(), std::string("rack:a-rack"));
  RR_CHECK_EQ(racks[1].id().text(), std::string("rack:b-rack"));
  RR_CHECK_EQ(racks[2].id().text(), std::string("rack:c-rack"));
}

RR_TEST(generation_diff_reports_each_kind_of_change) {
  Fixture fixture;
  build(fixture, 6);
  const RackGeneration initial = fixture.generation;

  const auto first = insert(fixture, "m1", "s1", units_span(1, 1));
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;
  const RackGeneration after_insert = fixture.generation;

  MoveMemberRequest move;
  move.rack_id = fixture.rack_id;
  move.precondition.expected_generation = fixture.generation;
  move.precondition.expected_membership_generation = fixture.membership;
  move.member_id = member("m1");
  move.target_mount = units_span(2, 2);
  move.identity.provenance = rrtest::provenance_for("model-test", 19);
  const auto moved = fixture.registry.move_member(move);
  RR_REQUIRE_OK(moved);
  fixture.generation = moved.value().generation;
  fixture.membership = moved.value().membership_generation;
  const RackGeneration after_move = fixture.generation;

  const auto second = insert(fixture, "m2", "s2", units_span(3, 3));
  RR_REQUIRE_OK(second);
  fixture.generation = second.value().generation;
  fixture.membership = second.value().membership_generation;
  const RackGeneration after_second_insert = fixture.generation;

  RemoveMemberRequest removal;
  removal.rack_id = fixture.rack_id;
  removal.precondition.expected_generation = fixture.generation;
  removal.precondition.expected_membership_generation = fixture.membership;
  removal.member_id = member("m2");
  removal.identity.provenance = rrtest::provenance_for("model-test", 20);
  const auto removed = fixture.registry.remove_member(removal);
  RR_REQUIRE_OK(removed);

  // Each adjacent pair of retained generations isolates one kind of change.
  const auto added = fixture.registry.diff_generations(fixture.rack_id, initial, after_insert);
  RR_REQUIRE_OK(added);
  RR_CHECK_EQ(added.value().member_changes.size(), std::size_t{1});
  RR_CHECK(has_flag(added.value().member_changes[0].flags, MemberChangeFlag::Added));
  RR_CHECK_EQ(added.value().member_changes[0].member_id.text(), std::string("rm:m1"));

  const auto moved_diff = fixture.registry.diff_generations(fixture.rack_id, after_insert,
                                                            after_move);
  RR_REQUIRE_OK(moved_diff);
  RR_CHECK_EQ(moved_diff.value().member_changes.size(), std::size_t{1});
  const MemberChange& move_change = moved_diff.value().member_changes[0];
  RR_CHECK(has_flag(move_change.flags, MemberChangeFlag::Moved));
  RR_REQUIRE(move_change.previous_mount.has_value());
  RR_REQUIRE(move_change.current_mount.has_value());
  RR_CHECK_EQ(move_change.previous_mount->to_text(), std::string("full:[1,3)"));
  RR_CHECK_EQ(move_change.current_mount->to_text(), std::string("full:[3,5)"));

  const auto second_added = fixture.registry.diff_generations(fixture.rack_id, after_move,
                                                              after_second_insert);
  RR_REQUIRE_OK(second_added);
  RR_CHECK_EQ(second_added.value().member_changes.size(), std::size_t{1});
  RR_CHECK(has_flag(second_added.value().member_changes[0].flags, MemberChangeFlag::Added));
  RR_CHECK_EQ(second_added.value().member_changes[0].member_id.text(), std::string("rm:m2"));

  const auto removal_diff = fixture.registry.diff_generations(
      fixture.rack_id, after_second_insert, removed.value().generation);
  RR_REQUIRE_OK(removal_diff);
  RR_CHECK_EQ(removal_diff.value().member_changes.size(), std::size_t{1});
  RR_CHECK(has_flag(removal_diff.value().member_changes[0].flags, MemberChangeFlag::Removed));
  RR_CHECK_EQ(removal_diff.value().member_changes[0].member_id.text(), std::string("rm:m2"));

  // A whole-span diff from the very first generation sees m1 as added and a
  // membership generation that advanced by four.
  const auto whole = fixture.registry.diff_generations(fixture.rack_id, initial);
  RR_REQUIRE_OK(whole);
  RR_CHECK_EQ(whole.value().member_changes.size(), std::size_t{1});
  RR_CHECK_EQ(whole.value().from_membership_generation.value(), 0u);
  RR_CHECK_EQ(whole.value().to_membership_generation.value(), 4u);

  const auto same = fixture.registry.diff_generations(fixture.rack_id, removed.value().generation);
  RR_REQUIRE_OK(same);
  RR_CHECK(same.value().empty());

  const auto unknown = fixture.registry.diff_generations(
      fixture.rack_id, RackGeneration::create(900).value());
  RR_REQUIRE_CODE(unknown, ErrorCode::GenerationNotRetained);
}

RR_TEST(unknown_racks_and_members_report_precise_codes) {
  Fixture fixture;
  build(fixture);
  RR_REQUIRE_CODE(fixture.registry.rack(rack("nope")), ErrorCode::UnknownRackId);
  RR_REQUIRE_CODE(fixture.registry.occupancy(rack("nope")), ErrorCode::UnknownRackId);
  RR_REQUIRE_CODE(fixture.registry.free_spans(rack("nope")), ErrorCode::UnknownRackId);
  RR_REQUIRE_CODE(fixture.registry.members(rack("nope"), MemberOrder::MountOrder),
                  ErrorCode::UnknownRackId);

  const auto missing = fixture.registry.member(fixture.rack_id, member("ghost"));
  RR_REQUIRE_OK(missing);
  RR_CHECK(!missing.value().has_value());

  RemoveMemberRequest removal;
  removal.rack_id = fixture.rack_id;
  removal.precondition.expected_generation = fixture.generation;
  removal.precondition.expected_membership_generation = fixture.membership;
  removal.member_id = member("ghost");
  removal.identity.provenance = rrtest::provenance_for("model-test", 21);
  RR_REQUIRE_CODE(fixture.registry.remove_member(removal), ErrorCode::UnknownMemberId);
}

RR_TEST(rejections_are_recorded_with_stable_codes) {
  Fixture fixture;
  build(fixture);
  fixture.registry.clear_rejections();
  const std::size_t before = fixture.registry.stats().total_rejections;

  RR_REQUIRE_CODE(fixture.registry.rack(rack("nope")), ErrorCode::UnknownRackId);
  // A query failure is not a rejected mutation and must not enter the journal.
  RR_CHECK_EQ(fixture.registry.stats().total_rejections, before);
  RR_CHECK(fixture.registry.rejections().empty());

  const auto bad = insert(fixture, "m1", "s1", units_span(20, 21));
  RR_REQUIRE_CODE(bad, ErrorCode::MountOutOfBounds);

  const std::vector<RejectionRecord> rejections = fixture.registry.rejections();
  RR_REQUIRE(!rejections.empty());
  RR_CHECK(rejections.back().code == ErrorCode::MountOutOfBounds);
  RR_CHECK_EQ(rejections.back().operation, std::string("insert_member"));
  RR_CHECK_EQ(rejections.back().subject, std::string("rack:a01"));
  RR_CHECK(fixture.registry.stats().total_rejections > before);
  RR_CHECK(!rejections.back().to_text().empty());
}

RR_TEST(snapshot_and_restore_reproduce_the_same_state) {
  Fixture fixture;
  build(fixture, 6);
  const auto first = insert(fixture, "m1", "s1", units_span(1, 2));
  RR_REQUIRE_OK(first);
  fixture.generation = first.value().generation;
  fixture.membership = first.value().membership_generation;

  const auto producer = SourceReference::parse("model-test/1.0.0").value();
  const RackSnapshot snapshot = fixture.registry.snapshot(producer);
  RR_REQUIRE_OK(snapshot.validate());
  RR_CHECK_EQ(snapshot.rack_count(), std::size_t{1});
  RR_CHECK_EQ(snapshot.member_count(), std::size_t{1});
  RR_CHECK_EQ(snapshot.produced_by()->text(), std::string("model-test/1.0.0"));

  RackRegistry restored;
  RR_REQUIRE_OK(restored.restore(snapshot));
  RR_CHECK_EQ(restored.state_digest(), fixture.registry.state_digest());
  RR_CHECK_EQ(restored.rack(fixture.rack_id).value().generation().value(),
              fixture.generation.value());

  // A snapshot with a different producer has a different digest.
  const RackSnapshot other = fixture.registry.snapshot(std::nullopt);
  RR_CHECK_NE(other.state_digest(), snapshot.state_digest());
  RR_CHECK_EQ(other.racks()[0].structure.id.text(), snapshot.racks()[0].structure.id.text());
}

RR_TEST(restore_replaces_the_whole_registry_atomically) {
  Fixture fixture;
  build(fixture, 4);
  const auto first = insert(fixture, "m1", "s1", units_span(1, 2));
  RR_REQUIRE_OK(first);

  RackRegistry other;
  RR_REQUIRE_OK(other.register_rack(registration("b02", 6)));
  RR_CHECK_EQ(other.rack_count(), std::size_t{1});

  const RackSnapshot snapshot = fixture.registry.snapshot(std::nullopt);
  RR_REQUIRE_OK(snapshot.validate());
  RR_REQUIRE_OK(other.restore(snapshot));
  RR_CHECK_EQ(other.rack_count(), std::size_t{1});
  RR_CHECK(other.contains_rack(fixture.rack_id));
  RR_CHECK(!other.contains_rack(rack("b02")));
  RR_CHECK_EQ(other.state_digest(), fixture.registry.state_digest());

  // A snapshot with a different producer identity has a different digest while
  // describing the same racks.
  const RackSnapshot produced = fixture.registry.snapshot(
      SourceReference::parse("model-test/1.0.0").value());
  RR_REQUIRE_OK(produced.validate());
  RR_CHECK_NE(produced.state_digest(), snapshot.state_digest());
  RR_CHECK_EQ(produced.racks()[0].structure.id.text(), snapshot.racks()[0].structure.id.text());
}

RR_TEST(a_snapshot_carries_the_generation_evidence_chain) {
  Fixture fixture;
  build(fixture, 4);
  auto receipt = insert(fixture, "m1", "s1", units_span(1, 1));
  RR_REQUIRE_OK(receipt);
  fixture.generation = receipt.value().generation;
  fixture.membership = receipt.value().membership_generation;
  receipt = insert(fixture, "m2", "s2", units_span(2, 2));
  RR_REQUIRE_OK(receipt);

  const auto view = fixture.registry.rack(fixture.rack_id);
  RR_REQUIRE_OK(view);
  const std::vector<GenerationEvidence>& evidence = view.value().generation_evidence();
  RR_REQUIRE(!evidence.empty());
  RR_CHECK_EQ(evidence.back().generation.value(), view.value().generation().value());
  RR_CHECK_EQ(evidence.back().member_count, 2u);
  for (std::size_t i = 1; i < evidence.size(); ++i) {
    RR_CHECK(evidence[i - 1].generation.value() < evidence[i].generation.value());
  }
  RR_CHECK_EQ(evidence.back().state_digest, view.value().state_digest());
}
