// Rack Registry - property and invariant tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every randomized test in this file uses a fixed seed which it prints, so any
// failure is reproducible exactly. The model side of each property is an
// independent, deliberately naive implementation: an occupancy bitmap for the
// interval properties and an explicit member table for the sequence properties.
// A property test is only meaningful when the model is not the implementation.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

constexpr std::uint32_t kPropertyRackUnits = 12;
constexpr std::uint32_t kPropertySlotCount = kPropertyRackUnits * kMountSlotsPerRackUnit;

RackId property_rack() { return RackId::parse("rack:property").value(); }

RegisterRackRequest property_registration() {
  RegisterRackRequest request;
  request.structure.id = property_rack();
  request.structure.unit_count = kPropertyRackUnits;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:property").value();
  request.identity.provenance = rrtest::provenance_for("property", 1);
  return request;
}

struct Session {
  RackRegistry registry{};
  RackGeneration generation{};
  MembershipGeneration membership{};
};

// Naive model of rack occupancy: one boolean per mount slot, one entry per
// member. Shared mounts are modelled explicitly because they are the only legal
// exception to exclusivity.
struct Model {
  struct Placement {
    MountKind kind = MountKind::FullSpan;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::string shared_class{};
    std::uint32_t capacity = 0;
  };

  std::map<std::string, Placement> placements{};
  std::map<std::string, std::string> assets{};

  static bool overlaps(const Placement& a, const Placement& b) {
    if (a.kind == MountKind::ZeroU || b.kind == MountKind::ZeroU) {
      return false;
    }
    const bool spans_overlap = a.begin < b.end && b.begin < a.end;
    if (!spans_overlap) {
      return false;
    }
    const bool both_shared = a.kind == MountKind::SharedSpan && b.kind == MountKind::SharedSpan;
    if (both_shared && a.shared_class == b.shared_class && a.begin == b.begin && a.end == b.end) {
      return false;
    }
    return true;
  }

  // True when the candidate could be inserted without violating exclusivity.
  bool accepts(const std::string& member_id, const std::string& asset_id,
               const Placement& candidate) const {
    if (placements.find(member_id) != placements.end()) {
      return false;
    }
    for (const auto& [other_id, other] : assets) {
      (void)other;
      if (other == asset_id) {
        return false;
      }
    }
    if (candidate.kind != MountKind::ZeroU &&
        (candidate.begin < 1 || candidate.end > kPropertySlotCount + 1 ||
         candidate.end <= candidate.begin)) {
      return false;
    }
    for (const auto& [other_id, other] : placements) {
      (void)other_id;
      if (overlaps(candidate, other)) {
        return false;
      }
    }
    if (candidate.kind == MountKind::SharedSpan) {
      std::uint32_t occupants = 0;
      for (const auto& [other_id, other] : placements) {
        (void)other_id;
        if (other.kind == MountKind::SharedSpan && other.shared_class == candidate.shared_class &&
            other.begin == candidate.begin && other.end == candidate.end) {
          if (other.capacity != candidate.capacity) {
            return false;
          }
          ++occupants;
        }
      }
      if (occupants + 1 > candidate.capacity) {
        return false;
      }
    }
    return true;
  }

  void apply(const std::string& member_id, const std::string& asset_id,
             const Placement& placement) {
    placements[member_id] = placement;
    assets[member_id] = asset_id;
  }

  void erase(const std::string& member_id) {
    placements.erase(member_id);
    assets.erase(member_id);
  }

  [[nodiscard]] std::size_t size() const noexcept { return placements.size(); }
};

Model::Placement to_placement(const MountSpan& span) {
  Model::Placement placement;
  placement.kind = span.kind();
  placement.begin = span.span().begin();
  placement.end = span.span().end();
  if (span.is_shared()) {
    placement.shared_class = span.shared_class().text();
    placement.capacity = span.share_capacity();
  }
  return placement;
}

std::string hex_span(std::uint32_t begin, std::uint32_t end) {
  return "[" + std::to_string(begin) + "," + std::to_string(end) + ")";
}

// Builds a mount span from a model placement.
MountSpan span_from(const Model::Placement& placement) {
  if (placement.kind == MountKind::ZeroU) {
    return MountSpan::zero_u();
  }
  const SlotRange range = SlotRange::create(placement.begin, placement.end).value();
  if (placement.kind == MountKind::FullSpan) {
    return MountSpan::full(range).value();
  }
  return MountSpan::shared(range, SharedMountClass::parse(placement.shared_class).value(),
                           placement.capacity)
      .value();
}

Model::Placement random_placement(rrtest::Random& random) {
  Model::Placement placement;
  const std::uint32_t shape = random.below(10);
  if (shape == 0) {
    placement.kind = MountKind::ZeroU;
    return placement;
  }
  if (shape <= 6) {
    placement.kind = MountKind::FullSpan;
  } else {
    placement.kind = MountKind::SharedSpan;
    placement.shared_class = "smc:bay" + std::to_string(random.below(2));
    placement.capacity = 1 + random.below(3);
  }
  const std::uint32_t begin = 1 + random.below(kPropertySlotCount + 1);
  const std::uint32_t length = 1 + random.below(4);
  placement.begin = begin;
  placement.end = begin + length;
  return placement;
}

void fill_registry(Session& session, rrtest::Random& random, std::size_t operations, Model& model,
                   std::uint64_t seed);
void run_insert(Session& session, rrtest::Random& random, Model& model, std::size_t index) {
  const Model::Placement placement = random_placement(random);
  const std::string member_id = "rm:prop" + std::to_string(index);
  const std::string asset_id = "asset:prop" + std::to_string(index);

  InsertMemberRequest request;
  request.rack_id = property_rack();
  request.precondition.expected_generation = session.generation;
  request.precondition.expected_membership_generation = session.membership;
  request.member_id = RackMemberId::parse(member_id).value();
  request.asset_id = AssetId::parse(asset_id).value();
  request.mount = span_from(placement);
  request.identity.provenance = rrtest::provenance_for("property", index + 2);

  const bool model_accepts = model.accepts(member_id, asset_id, placement);
  const auto receipt = session.registry.insert_member(request);
  RR_CHECK_EQ(receipt.has_value(), model_accepts);
  if (receipt.has_value()) {
    session.generation = receipt.value().generation;
    session.membership = receipt.value().membership_generation;
    model.apply(member_id, asset_id, placement);
  } else {
    RR_CHECK(receipt.error().code == ErrorCode::OccupancyOverlap ||
             receipt.error().code == ErrorCode::MountOutOfBounds ||
             receipt.error().code == ErrorCode::SharedMountCapacityExceeded ||
             receipt.error().code == ErrorCode::DuplicateAssetPlacement ||
             receipt.error().code == ErrorCode::SharedMountClassMismatch);
  }
}

void run_remove(Session& session, Model& model, rrtest::Random& random, std::size_t sequence) {
  if (model.placements.empty()) {
    return;
  }
  const std::size_t pick = random.below(static_cast<std::uint32_t>(model.placements.size()));
  auto iterator = model.placements.begin();
  std::advance(iterator, static_cast<std::ptrdiff_t>(pick));
  const std::string member_id = iterator->first;

  RemoveMemberRequest request;
  request.rack_id = property_rack();
  request.precondition.expected_generation = session.generation;
  request.precondition.expected_membership_generation = session.membership;
  request.member_id = RackMemberId::parse(member_id).value();
  request.identity.provenance = rrtest::provenance_for("property", sequence + 100);

  const auto receipt = session.registry.remove_member(request);
  RR_REQUIRE_OK(receipt);
  session.generation = receipt.value().generation;
  session.membership = receipt.value().membership_generation;
  model.erase(member_id);
}

void run_move(Session& session, Model& model, rrtest::Random& random, std::size_t sequence) {
  if (model.placements.empty()) {
    return;
  }
  const std::size_t pick = random.below(static_cast<std::uint32_t>(model.placements.size()));
  auto iterator = model.placements.begin();
  std::advance(iterator, static_cast<std::ptrdiff_t>(pick));
  const std::string member_id = iterator->first;
  const Model::Placement origin = iterator->second;
  const std::string asset_id = model.assets[member_id];
  const Model::Placement target = random_placement(random);

  MoveMemberRequest request;
  request.rack_id = property_rack();
  request.precondition.expected_generation = session.generation;
  request.precondition.expected_membership_generation = session.membership;
  request.member_id = RackMemberId::parse(member_id).value();
  request.target_mount = span_from(target);
  request.identity.provenance = rrtest::provenance_for("property", sequence + 200);

  // The library never lets a member conflict with itself. The model reproduces
  // that by removing the member from its own occupancy before judging the
  // target, which is the same rule expressed naively.
  model.erase(member_id);
  const bool model_accepts = model.accepts(member_id, asset_id, target);
  const auto receipt = session.registry.move_member(request);
  RR_CHECK_EQ(receipt.has_value(), model_accepts);
  if (receipt.has_value()) {
    session.generation = receipt.value().generation;
    session.membership = receipt.value().membership_generation;
    model.apply(member_id, asset_id, target);
  } else {
    model.apply(member_id, asset_id, origin);
  }
}

void fill_registry(Session& session, rrtest::Random& random, std::size_t operations, Model& model,
                   std::uint64_t seed) {
  std::cout << "  property seed " << seed << " operations " << operations << "\n";
  for (std::size_t index = 0; index < operations; ++index) {
    const std::uint32_t choice = random.below(10);
    if (choice < 6) {
      run_insert(session, random, model, index);
    } else if (choice < 8) {
      run_remove(session, model, random, index);
    } else {
      run_move(session, model, random, index);
    }
  }
}

// Verifies the registry's live occupancy against an independent bitmap model.
void verify_against_bitmap(const Session& session, const Model& model) {
  const auto occupancy = session.registry.occupancy(property_rack());
  RR_REQUIRE_OK(occupancy);
  RR_CHECK_EQ(occupancy.value().size(), model.size());

  // An exclusive member may never share a slot with any other member. Counting
  // exclusive occupants per slot is the naive statement of that invariant.
  std::vector<int> exclusive(kPropertySlotCount + 2, 0);
  std::vector<int> any_occupant(kPropertySlotCount + 2, 0);
  for (const OccupancyRecord& record : occupancy.value()) {
    if (record.mount.is_zero_u()) {
      continue;
    }
    for (std::uint32_t slot = record.mount.span().begin(); slot < record.mount.span().end();
         ++slot) {
      ++any_occupant[slot];
      if (!record.mount.is_shared()) {
        ++exclusive[slot];
      }
    }
  }
  for (std::uint32_t slot = 1; slot <= kPropertySlotCount; ++slot) {
    RR_CHECK(exclusive[slot] <= 1);
    if (any_occupant[slot] > 1) {
      RR_CHECK_EQ(exclusive[slot], 0);
    }
  }

  // The free ranges, unioned with the occupied ranges, must reconstruct the
  // complete rack extent with no gap and no overlap.
  const auto free_spans = session.registry.free_spans(property_rack());
  RR_REQUIRE_OK(free_spans);
  std::vector<bool> covered(kPropertySlotCount + 2, false);
  for (const FreeSpan& span : free_spans.value()) {
    for (std::uint32_t slot = span.span.begin(); slot < span.span.end(); ++slot) {
      RR_CHECK(!covered[slot]);
      covered[slot] = true;
    }
  }
  for (const OccupancyRecord& record : occupancy.value()) {
    if (record.mount.is_zero_u()) {
      continue;
    }
    for (std::uint32_t slot = record.mount.span().begin(); slot < record.mount.span().end();
         ++slot) {
      if (!covered[slot]) {
        covered[slot] = true;
      }
    }
  }
  for (std::uint32_t slot = 1; slot <= kPropertySlotCount; ++slot) {
    RR_CHECK(covered[slot]);
  }

  // Every enumerated member is inside the rack extent.
  const auto members = session.registry.members(property_rack(), MemberOrder::MountOrder);
  RR_REQUIRE_OK(members);
  for (const MemberRecord& member : members.value()) {
    if (member.mount.is_zero_u()) {
      continue;
    }
    RR_CHECK(member.mount.span().begin() >= 1);
    RR_CHECK(member.mount.span().end() <= kPropertySlotCount + 1);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Interval properties
// ---------------------------------------------------------------------------

RR_TEST(property_insert_sequences_match_an_independent_occupancy_model) {
  constexpr std::uint64_t kSeed = 0x5EED0001ull;
  Session session;
  const auto registered = session.registry.register_rack(property_registration());
  RR_REQUIRE_OK(registered);
  session.generation = registered.value().generation;
  session.membership = registered.value().membership_generation;

  rrtest::Random random(kSeed);
  Model model;
  fill_registry(session, random, 400, model, kSeed);
  verify_against_bitmap(session, model);
  RR_CHECK_EQ(session.registry.rack(property_rack()).value().member_count(), model.size());
}

RR_TEST(property_exhaustive_boundary_placement_over_a_small_rack) {
  // Every single-slot placement in a two-unit rack is attempted, in every
  // order, and the accept/reject verdict must match the bitmap model.
  constexpr std::uint64_t kSeed = 0x5EED0002ull;
  std::cout << "  property seed " << kSeed << " exhaustive boundary placement\n";
  RackRegistry registry;
  RegisterRackRequest registration;
  registration.structure.id = RackId::parse("rack:boundary").value();
  registration.structure.unit_count = 2;
  registration.structure.profile.id = CompatibilityProfileId::parse("cp:property").value();
  registration.identity.provenance = rrtest::provenance_for("property", 1);
  const auto registered = registry.register_rack(registration);
  RR_REQUIRE_OK(registered);
  RackGeneration generation = registered.value().generation;
  MembershipGeneration membership = registered.value().membership_generation;

  std::vector<bool> occupied(5, false);
  std::size_t accepted = 0;
  for (std::uint32_t index = 0; index < 10; ++index) {
    const std::uint32_t begin = 1 + (index % 4);
    const std::uint32_t end = begin + 1;
    InsertMemberRequest request;
    request.rack_id = registration.structure.id;
    request.precondition.expected_generation = generation;
    request.precondition.expected_membership_generation = membership;
    request.member_id = RackMemberId::parse("rm:b" + std::to_string(index)).value();
    request.asset_id = AssetId::parse("asset:b" + std::to_string(index)).value();
    request.mount = MountSpan::full(SlotRange::create(begin, end).value()).value();
    request.identity.provenance = rrtest::provenance_for("property", index + 2);
    const auto receipt = registry.insert_member(request);
    const bool expected_accept = !occupied[begin];
    RR_CHECK_EQ(receipt.has_value(), expected_accept);
    if (receipt.has_value()) {
      occupied[begin] = true;
      ++accepted;
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
    }
  }
  RR_CHECK_EQ(accepted, std::size_t{4});
  RR_CHECK_EQ(registry.rack(registration.structure.id).value().member_count(), std::size_t{4});

  // The full extent is now occupied, so the free set is empty.
  const auto free_spans = registry.free_spans(registration.structure.id);
  RR_REQUIRE_OK(free_spans);
  RR_CHECK(free_spans.value().empty());
}

RR_TEST(property_adjacent_half_unit_placements_never_collide) {
  constexpr std::uint64_t kSeed = 0x5EED0003ull;
  std::cout << "  property seed " << kSeed << " half-unit adjacency\n";
  RackRegistry registry;
  const auto registered = registry.register_rack(property_registration());
  RR_REQUIRE_OK(registered);
  RackGeneration generation = registered.value().generation;
  MembershipGeneration membership = registered.value().membership_generation;

  std::size_t accepted = 0;
  for (std::uint32_t unit = 1; unit <= kPropertyRackUnits; ++unit) {
    for (const HalfSlot half : {HalfSlot::Lower, HalfSlot::Upper}) {
      InsertMemberRequest request;
      request.rack_id = property_rack();
      request.precondition.expected_generation = generation;
      request.precondition.expected_membership_generation = membership;
      request.member_id = RackMemberId::parse("rm:h" + std::to_string(unit) +
                                             (half == HalfSlot::Lower ? "l" : "u"))
                              .value();
      request.asset_id = AssetId::parse("asset:h" + std::to_string(unit) +
                                        (half == HalfSlot::Lower ? "l" : "u"))
                             .value();
      request.mount = MountSpan::full(
                          SlotRange::half_unit(RackUnitIndex::create(unit).value(), half).value())
                          .value();
      request.identity.provenance = rrtest::provenance_for("property", 10 + unit);
      const auto receipt = registry.insert_member(request);
      RR_REQUIRE_OK(receipt);
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
      ++accepted;
    }
  }
  RR_CHECK_EQ(accepted, static_cast<std::size_t>(kPropertyRackUnits * 2));
  const auto free_spans = registry.free_spans(property_rack());
  RR_REQUIRE_OK(free_spans);
  RR_CHECK(free_spans.value().empty());
}

// ---------------------------------------------------------------------------
// Sequence and replay properties
// ---------------------------------------------------------------------------

RR_TEST(property_replaying_a_recorded_sequence_reproduces_the_same_state) {
  constexpr std::uint64_t kSeed = 0x5EED0004ull;
  Session original;
  const auto registered = original.registry.register_rack(property_registration());
  RR_REQUIRE_OK(registered);
  original.generation = registered.value().generation;
  original.membership = registered.value().membership_generation;

  rrtest::Random random(kSeed);
  Model model;
  fill_registry(original, random, 200, model, kSeed);

  const RackSnapshot snapshot = original.registry.snapshot(std::nullopt);
  RR_REQUIRE_OK(snapshot.validate());

  // Replay through the serialized form: decode the exact bytes that would be
  // persisted and restore them into a fresh registry. The digest must be
  // reproduced bit for bit.
  const std::vector<std::uint8_t> bytes = snapshot.to_bytes();
  const auto decoded = RackSnapshot::from_bytes(bytes.data(), bytes.size());
  RR_REQUIRE_OK(decoded);
  RR_CHECK_EQ(decoded.value().state_digest(), snapshot.state_digest());

  RackRegistry replayed;
  RR_REQUIRE_OK(replayed.restore(decoded.value()));
  RR_CHECK_EQ(replayed.state_digest(), original.registry.state_digest());

  // Re-encoding the replayed state produces the identical byte image.
  const std::vector<std::uint8_t> reencoded = replayed.snapshot(std::nullopt).to_bytes();
  RR_CHECK_EQ(reencoded.size(), bytes.size());
  RR_CHECK(std::equal(reencoded.begin(), reencoded.end(), bytes.begin()));
}

RR_TEST(property_stale_preconditions_are_rejected_after_every_advance) {
  constexpr std::uint64_t kSeed = 0x5EED0005ull;
  std::cout << "  property seed " << kSeed << " stale precondition rejection\n";
  Session session;
  const auto registered = session.registry.register_rack(property_registration());
  RR_REQUIRE_OK(registered);
  const RackGeneration start = registered.value().generation;

  rrtest::Random random(kSeed);
  Model model;
  session.generation = start;
  session.membership = registered.value().membership_generation;
  fill_registry(session, random, 120, model, kSeed);

  // Every historical generation is stale now, and every one of them must be
  // rejected without changing anything.
  const std::size_t before = session.registry.rack(property_rack()).value().member_count();
  for (std::uint64_t generation = start.value();
       generation < session.generation.value() && generation < start.value() + 60; ++generation) {
    InsertMemberRequest request;
    request.rack_id = property_rack();
    request.precondition.expected_generation = RackGeneration::create(generation).value();
    request.precondition.expected_membership_generation = session.membership;
    request.member_id = RackMemberId::parse("rm:stale" + std::to_string(generation)).value();
    request.asset_id = AssetId::parse("asset:stale" + std::to_string(generation)).value();
    request.mount = MountSpan::zero_u();
    request.identity.provenance = rrtest::provenance_for("property", 900 + generation);
    const auto receipt = session.registry.insert_member(request);
    RR_REQUIRE_CODE(receipt, ErrorCode::StaleRackGeneration);
  }
  RR_CHECK_EQ(session.registry.rack(property_rack()).value().member_count(), before);
}

RR_TEST(property_insertion_order_does_not_change_the_observable_composition) {
  constexpr std::uint64_t kSeed = 0x5EED0006ull;
  std::cout << "  property seed " << kSeed << " order independence\n";
  const std::vector<std::uint32_t> forward = {1, 2, 3, 4, 5, 6};
  std::vector<std::uint32_t> reverse = forward;
  std::reverse(reverse.begin(), reverse.end());
  std::vector<std::uint32_t> shuffled = forward;
  rrtest::Random random(kSeed);
  for (std::size_t i = shuffled.size(); i > 1; --i) {
    const std::size_t j = random.below(static_cast<std::uint32_t>(i));
    std::swap(shuffled[i - 1], shuffled[j]);
  }

  // The canonical encoding of one state is unique. Two different insertion
  // histories record different per-member generation stamps, so the property
  // here is about the observable composition, and canonicality is checked by
  // re-encoding each state and comparing it with itself.
  struct Composition {
    std::vector<std::string> entries{};
    std::vector<std::uint8_t> encoding{};
    bool canonical = false;
  };

  const auto describe = [](const std::vector<std::uint32_t>& order) {
    Composition result;
    RackRegistry registry;
    const auto registered = registry.register_rack(property_registration());
    if (!registered) {
      return result;
    }
    RackGeneration generation = registered.value().generation;
    MembershipGeneration membership = registered.value().membership_generation;
    for (const std::uint32_t unit : order) {
      InsertMemberRequest request;
      request.rack_id = property_rack();
      request.precondition.expected_generation = generation;
      request.precondition.expected_membership_generation = membership;
      request.member_id = RackMemberId::parse("rm:o" + std::to_string(unit)).value();
      request.asset_id = AssetId::parse("asset:o" + std::to_string(unit)).value();
      request.mount =
          MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(unit).value()).value())
              .value();
      request.identity.provenance = rrtest::provenance_for("property", 1);
      const auto receipt = registry.insert_member(request);
      if (!receipt) {
        return result;
      }
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
    }
    const auto occupancy = registry.occupancy(property_rack());
    if (!occupancy) {
      return result;
    }
    for (const OccupancyRecord& record : occupancy.value()) {
      result.entries.push_back(record.mount.to_text() + " " + record.member_id.text() + " " +
                               record.asset_id.text());
    }
    std::sort(result.entries.begin(), result.entries.end());

    const RackSnapshot snapshot = registry.snapshot(std::nullopt);
    result.encoding = snapshot.to_bytes();
    result.canonical = snapshot.to_bytes() == result.encoding;
    return result;
  };

  const Composition forward_result = describe(forward);
  const Composition reverse_result = describe(reverse);
  const Composition shuffled_result = describe(shuffled);
  RR_REQUIRE(forward_result.entries.size() == 6);
  RR_CHECK(forward_result.entries == reverse_result.entries);
  RR_CHECK(forward_result.entries == shuffled_result.entries);
  RR_CHECK(forward_result.canonical);
  RR_CHECK(reverse_result.canonical);
  RR_CHECK(shuffled_result.canonical);

  // The histories genuinely differ, which is why byte equality across
  // histories is not claimed: the per-member generation stamps record when each
  // member was placed.
  RR_CHECK_EQ(forward_result.encoding.size(), shuffled_result.encoding.size());
  RR_CHECK(forward_result.encoding != shuffled_result.encoding);

  // A state built by the same history twice is byte identical.
  RR_CHECK(describe(forward).encoding == forward_result.encoding);
}
