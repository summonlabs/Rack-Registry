// Rack Registry - generation and snapshot diffing.
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
#include "rack_registry/lifecycle.hpp"
#include "rack_registry/member.hpp"
#include "rack_registry/mount.hpp"
#include "rack_registry/rack.hpp"

namespace rackregistry {

// Everything that can differ about one member between two generations.
enum class MemberChangeFlag : std::uint16_t {
  None = 0,
  Added = 1u << 0,
  Removed = 1u << 1,
  Moved = 1u << 2,
  AssetReplaced = 1u << 3,
  StateChanged = 1u << 4,
  RequirementsChanged = 1u << 5,
};

[[nodiscard]] constexpr MemberChangeFlag operator|(MemberChangeFlag left,
                                                   MemberChangeFlag right) noexcept {
  return static_cast<MemberChangeFlag>(static_cast<std::uint16_t>(left) |
                                       static_cast<std::uint16_t>(right));
}
[[nodiscard]] constexpr MemberChangeFlag operator&(MemberChangeFlag left,
                                                   MemberChangeFlag right) noexcept {
  return static_cast<MemberChangeFlag>(static_cast<std::uint16_t>(left) &
                                       static_cast<std::uint16_t>(right));
}
[[nodiscard]] constexpr bool has_flag(MemberChangeFlag value, MemberChangeFlag flag) noexcept {
  return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
}
[[nodiscard]] RACK_REGISTRY_API std::string member_change_flag_text(MemberChangeFlag value);
[[nodiscard]] RACK_REGISTRY_API std::uint16_t member_change_flag_value(MemberChangeFlag value);

struct MemberChange {
  RackMemberId member_id{};
  MemberChangeFlag flags = MemberChangeFlag::None;
  std::optional<AssetId> previous_asset{};
  std::optional<AssetId> current_asset{};
  std::optional<MountSpan> previous_mount{};
  std::optional<MountSpan> current_mount{};
  std::optional<MembershipState> previous_state{};
  std::optional<MembershipState> current_state{};
  std::optional<MemberRequirements> previous_requirements{};
  std::optional<MemberRequirements> current_requirements{};

  [[nodiscard]] std::string to_text() const;
};

// The complete difference between two generations of one rack. Member changes
// are ordered by member identity; domain and trait changes are ordered
// byte-wise. Two diffs of the same pair of records render identically.
//
// A rack that exists on only one side of a comparison is reported by
// SnapshotDiff, not here: a RackDiff always describes two records of the same
// rack.
struct RackDiff {
  RackId rack_id{};

  RackGeneration from_generation{};
  RackGeneration to_generation{};
  RackRevision from_revision{};
  RackRevision to_revision{};
  MembershipGeneration from_membership_generation{};
  MembershipGeneration to_membership_generation{};

  bool lifecycle_changed = false;
  LifecycleState from_lifecycle = LifecycleState::Defined;
  LifecycleState to_lifecycle = LifecycleState::Defined;

  bool extent_changed = false;
  std::uint32_t from_unit_count = 0;
  std::uint32_t to_unit_count = 0;

  bool profile_changed = false;
  CompatibilityProfileId from_profile{};
  CompatibilityProfileId to_profile{};
  std::vector<Trait> traits_added{};
  std::vector<Trait> traits_removed{};

  std::vector<PowerDomainReference> power_domains_added{};
  std::vector<PowerDomainReference> power_domains_removed{};
  std::vector<CoolingDomainReference> cooling_domains_added{};
  std::vector<CoolingDomainReference> cooling_domains_removed{};

  bool label_changed = false;
  DisplayLabel from_label{};
  DisplayLabel to_label{};

  std::vector<MemberChange> member_changes{};

  // True when nothing at all differs between the two records.
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::string to_text() const;
};

// Difference between two whole snapshots. Rack-level changes are ordered by
// rack identity.
struct SnapshotDiff {
  StateDigest from_digest{};
  StateDigest to_digest{};
  StoreEpoch from_epoch{};
  StoreEpoch to_epoch{};
  StoreSequence from_sequence{};
  StoreSequence to_sequence{};
  std::vector<RackId> racks_added{};
  std::vector<RackId> racks_removed{};
  std::vector<RackDiff> racks_changed{};

  [[nodiscard]] bool identical() const noexcept;
  [[nodiscard]] std::string to_text() const;
};

// Structural difference between two rack records. `from` may be a record from
// any source; the function never assumes `to` descends from `from`.
[[nodiscard]] RACK_REGISTRY_API RackDiff diff_records(const RackRecord& from,
                                                      const RackRecord& to);

[[nodiscard]] RACK_REGISTRY_API SnapshotDiff diff_snapshots(const RackSnapshot& from,
                                                            const RackSnapshot& to);

}  // namespace rackregistry
