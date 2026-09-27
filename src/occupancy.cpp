// Rack Registry - occupancy rules and invariant validation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "internal.hpp"
#include "rack_registry/requests.hpp"
#include "rack_registry/text.hpp"

namespace rackregistry {
namespace internal {

Status check_mount_bounds(const RackStructure& structure, const MountSpan& candidate,
                          std::string_view operation) {
  if (candidate.is_zero_u()) {
    return Status{};
  }
  const auto extent = rack_slot_extent(structure.unit_count);
  if (!extent) {
    return make_error(extent.error().code, extent.error().message,
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = structure.id.text(),
                                  .related = candidate.to_text()});
  }
  if (!extent.value().contains(candidate.span())) {
    return make_error(ErrorCode::MountOutOfBounds,
                      "mount span " + candidate.to_text() + " lies outside " +
                          extent.value().to_text() + " for a " +
                          std::to_string(structure.unit_count) + "-unit rack",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = structure.id.text(),
                                  .related = candidate.to_text(),
                                  .expected = extent.value().end(),
                                  .actual = candidate.span().end()});
  }
  return Status{};
}

Result<std::optional<OccupancyConflict>> find_occupancy_conflict(
    const RackStructure& structure, const std::map<RackMemberId, MemberRecord>& members,
    const MountSpan& candidate, const RackMemberId* ignore_member, std::string_view operation) {
  (void)structure;
  (void)operation;
  if (candidate.is_zero_u()) {
    return std::optional<OccupancyConflict>{};
  }

  // Members are visited in member-identity order, so the reported conflict is
  // the same on every run and on every machine.
  const auto ignored = [ignore_member](const RackMemberId& member_id) {
    return ignore_member != nullptr && member_id == *ignore_member;
  };

  // Shared-mount accounting comes first and covers the whole membership: a
  // shared coordinate can only be judged against every co-occupant, not just
  // against the first member that happens to conflict.
  if (candidate.is_shared()) {
    std::uint32_t occupants = 0;
    const RackMemberId* first_occupant = nullptr;
    for (const auto& [member_id, member] : members) {
      if (ignored(member_id) || !member.mount.co_occupies(candidate)) {
        continue;
      }
      if (first_occupant == nullptr) {
        first_occupant = &member_id;
      }
      if (member.mount.share_capacity() != candidate.share_capacity()) {
        OccupancyConflict conflict;
        conflict.member_id = member_id;
        conflict.code = ErrorCode::SharedMountClassMismatch;
        conflict.message = "shared mount " + candidate.to_text() + " declares capacity " +
                           std::to_string(candidate.share_capacity()) + " but member " +
                           member_id.text() + " declares capacity " +
                           std::to_string(member.mount.share_capacity());
        return std::optional<OccupancyConflict>{conflict};
      }
      ++occupants;
    }
    if (first_occupant != nullptr && occupants + 1u > candidate.share_capacity()) {
      OccupancyConflict conflict;
      conflict.member_id = *first_occupant;
      conflict.code = ErrorCode::SharedMountCapacityExceeded;
      conflict.message = "shared mount " + candidate.to_text() + " already holds " +
                         std::to_string(occupants) + " of " +
                         std::to_string(candidate.share_capacity()) + " permitted members";
      return std::optional<OccupancyConflict>{conflict};
    }
  }

  for (const auto& [member_id, member] : members) {
    if (ignored(member_id) || !member.mount.conflicts_with(candidate)) {
      continue;
    }
    OccupancyConflict conflict;
    conflict.member_id = member_id;
    conflict.code = ErrorCode::OccupancyOverlap;
    conflict.message = "mount span " + candidate.to_text() + " overlaps member " +
                       member_id.text() + " at " + member.mount.to_text();
    return std::optional<OccupancyConflict>{conflict};
  }
  return std::optional<OccupancyConflict>{};
}

namespace {

// Validates a group of members whose mount spans are exactly identical.
Status validate_identical_group(const RackStructure& structure,
                                const std::vector<const MemberRecord*>& group) {
  if (group.size() <= 1) {
    return Status{};
  }
  const MemberRecord& first = *group.front();
  if (first.mount.kind() != MountKind::SharedSpan) {
    return make_error(ErrorCode::OccupancyOverlap,
                      "members " + first.member_id.text() + " and " + group[1]->member_id.text() +
                          " both occupy " + first.mount.to_text() +
                          " but the mount is not a shared mount",
                      ErrorDetail{.operation = "validate_occupancy",
                                  .subject = structure.id.text(),
                                  .related = group[1]->member_id.text(),
                                  .items = {first.mount.to_text()}});
  }
  for (const MemberRecord* member : group) {
    if (member->mount.kind() != MountKind::SharedSpan) {
      return make_error(ErrorCode::OccupancyOverlap,
                        "member " + member->member_id.text() + " occupies shared mount " +
                            first.mount.to_text() + " as an exclusive mount",
                        ErrorDetail{.operation = "validate_occupancy",
                                    .subject = structure.id.text(),
                                    .related = member->member_id.text()});
    }
    if (!(member->mount.shared_class() == first.mount.shared_class()) ||
        member->mount.share_capacity() != first.mount.share_capacity()) {
      return make_error(ErrorCode::SharedMountClassMismatch,
                        "co-occupants of " + first.mount.to_text() +
                            " must declare the same shared-mount class and capacity",
                        ErrorDetail{.operation = "validate_occupancy",
                                    .subject = structure.id.text(),
                                    .related = member->member_id.text(),
                                    .items = {first.mount.shared_class().text(),
                                              member->mount.shared_class().text()}});
    }
  }
  if (group.size() > first.mount.share_capacity()) {
    return make_error(ErrorCode::SharedMountCapacityExceeded,
                      "shared mount " + first.mount.to_text() + " holds " +
                          std::to_string(group.size()) + " members but permits " +
                          std::to_string(first.mount.share_capacity()),
                      ErrorDetail{.operation = "validate_occupancy",
                                  .subject = structure.id.text(),
                                  .expected = first.mount.share_capacity(),
                                  .actual = group.size()});
  }
  return Status{};
}

}  // namespace

Status validate_occupancy(const RackStructure& structure,
                          const std::map<RackMemberId, MemberRecord>& members,
                          std::string_view operation) {
  if (members.size() > kMaxMembersPerRack) {
    return make_error(ErrorCode::LimitExceeded,
                      "rack holds more members than the documented bound",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = structure.id.text(),
                                  .expected = kMaxMembersPerRack,
                                  .actual = members.size()});
  }

  std::vector<const MemberRecord*> placed;
  placed.reserve(members.size());
  for (const auto& [member_id, member] : members) {
    (void)member_id;
    const Status bounds = check_mount_bounds(structure, member.mount, operation);
    if (!bounds) {
      return make_error(bounds.error().code, bounds.error().message,
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = structure.id.text(),
                                    .related = member.member_id.text()});
    }
    if (!member.mount.is_zero_u()) {
      placed.push_back(&member);
    }
  }

  std::sort(placed.begin(), placed.end(), [](const MemberRecord* a, const MemberRecord* b) {
    if (a->mount.span() != b->mount.span()) {
      return a->mount.span() < b->mount.span();
    }
    return a->member_id < b->member_id;
  });

  std::size_t index = 0;
  while (index < placed.size()) {
    std::size_t end_of_group = index;
    std::vector<const MemberRecord*> group;
    while (end_of_group < placed.size() &&
           placed[end_of_group]->mount.span().is_identical_to(placed[index]->mount.span())) {
      group.push_back(placed[end_of_group]);
      ++end_of_group;
    }
    const Status group_status = validate_identical_group(structure, group);
    if (!group_status) {
      return group_status;
    }
    if (end_of_group < placed.size() &&
        placed[end_of_group]->mount.span().begin() < placed[index]->mount.span().end()) {
      return make_error(ErrorCode::OccupancyOverlap,
                        "mount spans " + placed[index]->mount.to_text() + " and " +
                            placed[end_of_group]->mount.to_text() + " partially overlap",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = structure.id.text(),
                                    .related = placed[end_of_group]->member_id.text(),
                                    .items = {placed[index]->member_id.text()}});
    }
    index = end_of_group;
  }
  return Status{};
}

Status validate_member_consistency(const RackStructure& structure, const MemberRecord& member,
                                   std::string_view operation) {
  if (member.member_id.empty()) {
    return make_error(ErrorCode::EmptyValue, "member identity must not be empty",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = structure.id.text()});
  }
  if (member.asset_id.empty()) {
    return make_error(ErrorCode::EmptyValue, "member asset reference must not be empty",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = structure.id.text(),
                                  .related = member.member_id.text()});
  }
  if (member.provenance.size() > kMaxProvenanceRecordsPerMember) {
    return make_error(ErrorCode::LimitExceeded,
                      "member provenance trail exceeds the documented bound",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = member.member_id.text(),
                                  .expected = kMaxProvenanceRecordsPerMember,
                                  .actual = member.provenance.size()});
  }
  if (member.mount_generation.value() == 0 || member.asset_generation.value() == 0) {
    return make_error(ErrorCode::InvalidRange,
                      "a member must record the rack generation it was placed at",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = member.member_id.text()});
  }
  for (const ProvenanceRecord& provenance : member.provenance) {
    const Status valid = validate_provenance(provenance);
    if (!valid) {
      return make_error(valid.error().code, valid.error().message,
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = member.member_id.text()});
    }
  }
  const CompatibilityReport report = evaluate_compatibility(structure.profile, member.requirements);
  if (!report.compatible) {
    return make_error(ErrorCode::CompatibilityUnsatisfied,
                      "member " + member.member_id.text() + " is incompatible with profile " +
                          structure.profile.id.text() + ": " + report.to_text(),
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = member.member_id.text(),
                                  .related = structure.profile.id.text(),
                                  .items = [&report]() {
                                    std::vector<std::string> items;
                                    for (const Trait& trait : report.missing) {
                                      items.push_back("missing:" + trait.text());
                                    }
                                    for (const Trait& trait : report.forbidden_present) {
                                      items.push_back("forbidden:" + trait.text());
                                    }
                                    return items;
                                  }()});
  }
  return Status{};
}

Status validate_rack_record(const RackRecord& record, std::string_view operation) {
  const Status structure_status = validate_structure(record.structure);
  if (!structure_status) {
    return make_error(structure_status.error().code, structure_status.error().message,
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text()});
  }
  if (record.generation.value() == 0) {
    return make_error(ErrorCode::InvalidRange, "rack generation must be at least 1",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = 1,
                                  .actual = record.generation.value()});
  }
  if (record.revision.value() == 0) {
    return make_error(ErrorCode::InvalidRange, "rack revision must be at least 1",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = 1,
                                  .actual = record.revision.value()});
  }

  std::vector<std::string> asset_refs;
  asset_refs.reserve(record.members.size());
  for (const auto& [member_id, member] : record.members) {
    if (member_id.empty() || !(member.member_id == member_id)) {
      return make_error(ErrorCode::CorruptState,
                        "member map key does not match the member identity it holds",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text(),
                                    .related = member.member_id.text()});
    }
    const Status member_status = validate_member_consistency(record.structure, member, operation);
    if (!member_status) {
      return member_status;
    }
    // Generation bookkeeping must be internally consistent: a member cannot
    // have been placed or replaced at a generation the rack has not reached,
    // and it cannot claim to have been created after the current membership.
    if (member.mount_generation.value() > record.generation.value() ||
        member.asset_generation.value() > record.generation.value() ||
        member.created_at_membership_generation.value() > record.membership_generation.value()) {
      return make_error(ErrorCode::SnapshotIdentityMismatch,
                        "member " + member_id.text() +
                            " records a generation the rack has not reached",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text(),
                                    .related = member_id.text(),
                                    .expected = record.generation.value(),
                                    .actual = member.mount_generation.value()});
    }
    asset_refs.push_back(member.asset_id.text());
  }
  std::sort(asset_refs.begin(), asset_refs.end(),
            [](const std::string& a, const std::string& b) { return byte_less(a, b); });
  for (std::size_t i = 1; i < asset_refs.size(); ++i) {
    if (asset_refs[i] == asset_refs[i - 1]) {
      return make_error(ErrorCode::DuplicateAssetPlacement,
                        "asset " + asset_refs[i] + " is placed more than once in rack " +
                            record.structure.id.text(),
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text(),
                                    .related = asset_refs[i]});
    }
  }

  const Status occupancy = validate_occupancy(record.structure, record.members, operation);
  if (!occupancy) {
    return occupancy;
  }

  if (record.provenance.size() > kMaxProvenanceRecordsPerRack) {
    return make_error(ErrorCode::LimitExceeded,
                      "rack provenance trail exceeds the documented bound",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = kMaxProvenanceRecordsPerRack,
                                  .actual = record.provenance.size()});
  }
  for (const ProvenanceRecord& provenance : record.provenance) {
    const Status valid = validate_provenance(provenance);
    if (!valid) {
      return valid;
    }
  }

  if (record.generation_evidence.size() > kMaxRetainedGenerationsPerRack) {
    return make_error(ErrorCode::LimitExceeded,
                      "generation evidence ring exceeds the documented bound",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = kMaxRetainedGenerationsPerRack,
                                  .actual = record.generation_evidence.size()});
  }
  for (std::size_t i = 0; i < record.generation_evidence.size(); ++i) {
    const GenerationEvidence& evidence = record.generation_evidence[i];
    if (evidence.generation.value() == 0) {
      return make_error(ErrorCode::InvalidRange, "generation evidence names generation 0",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text()});
    }
    if (i > 0 && !(record.generation_evidence[i - 1].generation < evidence.generation)) {
      return make_error(ErrorCode::SequenceRegression,
                        "generation evidence is not in ascending generation order",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text(),
                                    .expected = record.generation_evidence[i - 1].generation.value(),
                                    .actual = evidence.generation.value()});
    }
  }
  if (!record.generation_evidence.empty() &&
      !(record.generation_evidence.back().generation == record.generation)) {
    return make_error(ErrorCode::SnapshotIdentityMismatch,
                      "the retained generation evidence does not describe the current generation",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = record.generation.value(),
                                  .actual = record.generation_evidence.back().generation.value()});
  }
  if (record.generation_evidence.size() > record.generation.value()) {
    return make_error(ErrorCode::SnapshotIdentityMismatch,
                      "more generation evidence retained than the rack has had generations",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = record.generation.value(),
                                  .actual = record.generation_evidence.size()});
  }

  if (record.idempotency.size() > kMaxIdempotencyRecordsPerRack) {
    return make_error(ErrorCode::LimitExceeded,
                      "idempotency table exceeds the documented bound",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = record.structure.id.text(),
                                  .expected = kMaxIdempotencyRecordsPerRack,
                                  .actual = record.idempotency.size()});
  }
  for (const IdempotencyRecord& entry : record.idempotency) {
    if (entry.request_id.empty()) {
      return make_error(ErrorCode::EmptyValue, "idempotency record has an empty request identity",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text()});
    }
    if (entry.operation >= kOperationKindCount) {
      return make_error(ErrorCode::InvalidEnumValue,
                        "idempotency record names an unknown operation",
                        ErrorDetail{.operation = std::string(operation),
                                    .subject = record.structure.id.text(),
                                    .actual = entry.operation});
    }
  }

  return Status{};
}

}  // namespace internal
}  // namespace rackregistry
