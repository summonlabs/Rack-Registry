// Rack Registry - mutation command types.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/compatibility.hpp"
#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/lifecycle.hpp"
#include "rack_registry/member.hpp"
#include "rack_registry/mount.hpp"
#include "rack_registry/provenance.hpp"
#include "rack_registry/rack.hpp"

namespace rackregistry {

// Every accepted mutation produces a receipt naming the operation and the
// resulting generations. Receipts are the only source of "what generation am I
// now at", which is what a caller uses to build the precondition of its next
// command.
enum class OperationKind : std::uint8_t {
  RegisterRack = 0,
  SetRackStructure = 1,
  TransitionLifecycle = 2,
  InsertMember = 3,
  RemoveMember = 4,
  MoveMember = 5,
  ReplaceMember = 6,
  RestoreSnapshot = 7,
};

inline constexpr std::size_t kOperationKindCount = 8;

[[nodiscard]] RACK_REGISTRY_API std::string_view operation_kind_name(OperationKind kind) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<OperationKind> parse_operation_kind(std::string_view text);
[[nodiscard]] RACK_REGISTRY_API Result<OperationKind> operation_kind_from_value(std::uint8_t value);

// Precondition for a mutation that changes only the structure of a rack.
struct StructuralPrecondition {
  RackGeneration expected_generation{};
};

// Precondition for a membership mutation. Both counters are required: the rack
// generation fences against a concurrent structural or lifecycle change, and
// the membership generation fences against a concurrent membership change.
struct MembershipPrecondition {
  RackGeneration expected_generation{};
  MembershipGeneration expected_membership_generation{};
};

// Precondition for a lifecycle transition. The expected state makes the
// transition a compare-and-swap rather than a blind assignment.
struct LifecyclePrecondition {
  RackGeneration expected_generation{};
  LifecycleState expected_state = LifecycleState::Defined;
};

// Shared part of every mutation command.
struct MutationIdentity {
  ProvenanceRecord provenance{};
  std::optional<RequestId> request_id{};
};

struct RegisterRackRequest {
  RackStructure structure{};
  MutationIdentity identity{};
};

// Full replacement of a rack's structural definition. Structure changes are
// expressed as complete definitions rather than patches, so a caller cannot
// accidentally leave a field at a value it did not intend.
struct SetRackStructureRequest {
  RackId rack_id{};
  StructuralPrecondition precondition{};
  std::uint32_t unit_count = 0;
  CompatibilityProfile profile{};
  std::vector<PowerDomainReference> power_domains{};
  std::vector<CoolingDomainReference> cooling_domains{};
  DisplayLabel label{};
  MutationIdentity identity{};
};

struct TransitionLifecycleRequest {
  RackId rack_id{};
  LifecyclePrecondition precondition{};
  LifecycleState target = LifecycleState::Defined;
  MutationIdentity identity{};
};

struct InsertMemberRequest {
  RackId rack_id{};
  MembershipPrecondition precondition{};
  RackMemberId member_id{};
  AssetId asset_id{};
  MountSpan mount{};
  MembershipState state = MembershipState::Installed;
  MemberRequirements requirements{};
  MutationIdentity identity{};
};

struct RemoveMemberRequest {
  RackId rack_id{};
  MembershipPrecondition precondition{};
  RackMemberId member_id{};
  MutationIdentity identity{};
};

struct MoveMemberRequest {
  RackId rack_id{};
  MembershipPrecondition precondition{};
  RackMemberId member_id{};
  MountSpan target_mount{};
  MutationIdentity identity{};
};

// Replaces the asset referenced by an existing membership record, keeping the
// member identity and mount span. The new asset's requirements are validated
// against the rack profile, and the membership state may be advanced (for
// example from Reserved to Installed) in the same atomic step.
struct ReplaceMemberRequest {
  RackId rack_id{};
  MembershipPrecondition precondition{};
  RackMemberId member_id{};
  AssetId replacement_asset{};
  MembershipState target_state = MembershipState::Installed;
  MemberRequirements requirements{};
  MutationIdentity identity{};
};

}  // namespace rackregistry
