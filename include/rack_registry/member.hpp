// Rack Registry - rack membership records.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_registry/compatibility.hpp"
#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/mount.hpp"
#include "rack_registry/provenance.hpp"

namespace rackregistry {

// Whether a membership record describes hardware that is installed in the rack
// or a coordinate that is held for an installation that has not completed.
// A reservation here is a *structural coordinate hold*: it occupies its mount
// span for overlap purposes so that two parties cannot be promised the same
// place. It is not a capacity commitment, an allocation, or a scheduling
// decision; those belong to Facility Capacity Reservation.
enum class MembershipState : std::uint8_t { Installed = 0, Reserved = 1 };

inline constexpr std::size_t kMembershipStateCount = 2;

[[nodiscard]] RACK_REGISTRY_API std::string_view membership_state_name(
    MembershipState state) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<MembershipState> parse_membership_state(
    std::string_view text);
[[nodiscard]] RACK_REGISTRY_API Result<MembershipState> membership_state_from_value(
    std::uint8_t value);

// Authoritative membership record. Exactly one record exists per governed
// member in a rack generation.
struct MemberRecord {
  RackMemberId member_id{};
  AssetId asset_id{};
  MountSpan mount{};
  MembershipState state = MembershipState::Installed;
  MemberRequirements requirements{};
  // Rack generation at which this record's mount span last changed.
  RackGeneration mount_generation{};
  // Rack generation at which this record's asset reference last changed.
  RackGeneration asset_generation{};
  // Membership generation this record was created at.
  MembershipGeneration created_at_membership_generation{};
  // Bounded provenance trail, oldest first, with the number of records evicted
  // from the front of it so that bounded coverage stays visible.
  std::vector<ProvenanceRecord> provenance{};
  std::uint64_t provenance_dropped = 0;

  [[nodiscard]] bool operator==(const MemberRecord& other) const noexcept;
  [[nodiscard]] bool operator!=(const MemberRecord& other) const noexcept {
    return !(*this == other);
  }
};

// Read-only projection of one occupancy, returned by occupancy queries. It
// carries exactly what is needed to describe what is where, including the
// rack-unit projection of the mount span.
struct OccupancyRecord {
  RackMemberId member_id{};
  AssetId asset_id{};
  MountSpan mount{};
  MembershipState state = MembershipState::Installed;
  // Rack units the mount span touches; nullopt for a zero-U mount.
  std::optional<RackUnitRange> units{};
};

// How member enumeration is ordered. Both orders are total and deterministic,
// so no enumeration of members depends on insertion history.
enum class MemberOrder : std::uint8_t {
  // Ascending by mount span (zero-U members last), then by member identity.
  MountOrder = 0,
  // Ascending by member identity.
  IdentityOrder = 1,
};

[[nodiscard]] RACK_REGISTRY_API std::string_view member_order_name(MemberOrder order) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<MemberOrder> parse_member_order(std::string_view text);

// One slot interval that is not covered by any non-zero-U member, together
// with the shared-mount availability inside it. This is descriptive structural
// state: it reports what is physically unoccupied, not what may be allocated.
struct FreeSpan {
  SlotRange span{};
  std::optional<RackUnitRange> units{};

  [[nodiscard]] std::string to_text() const;
};

// Availability of one shared-mount coordinate. `capacity` is the capacity all
// current co-occupants declare; `occupied` is how many members hold it.
struct SharedSpanAvailability {
  SlotRange span{};
  SharedMountClass shared_class{};
  std::uint32_t capacity = 0;
  std::uint32_t occupied = 0;

  [[nodiscard]] std::uint32_t remaining() const noexcept {
    return capacity > occupied ? capacity - occupied : 0u;
  }
  [[nodiscard]] std::string to_text() const;
};

}  // namespace rackregistry
