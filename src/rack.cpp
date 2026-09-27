// Rack Registry - rack structures, records, views and snapshots.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/rack.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "internal.hpp"
#include "rack_registry/text.hpp"

namespace rackregistry {
namespace {

constexpr std::string_view kSnapshotDigestDomain = "rack-registry.snapshot.v1";

// Sort key for mount-ordered enumeration: exclusive and shared mounts first in
// ascending slot order, then zero-U mounts, and identity as the final
// tie-break so the order is total.
bool mount_order_less(const MemberRecord& left, const MemberRecord& right) {
  const bool left_zero = left.mount.is_zero_u();
  const bool right_zero = right.mount.is_zero_u();
  if (left_zero != right_zero) {
    return !left_zero;
  }
  if (!left_zero) {
    if (!(left.mount.span() == right.mount.span())) {
      return left.mount.span() < right.mount.span();
    }
  }
  return left.member_id < right.member_id;
}

}  // namespace

// ---------------------------------------------------------------------------
// RackStructure
// ---------------------------------------------------------------------------

SlotRange RackStructure::slot_extent() const noexcept {
  if (unit_count < 1 || unit_count > kMaxRackUnits) {
    return SlotRange{};
  }
  const auto extent = rack_slot_extent(unit_count);
  return extent.has_value() ? extent.value() : SlotRange{};
}

bool RackStructure::operator==(const RackStructure& other) const noexcept {
  return structures_equal(*this, other);
}

bool structures_equal(const RackStructure& left, const RackStructure& right) noexcept {
  if (!(left.id == right.id) || left.unit_count != right.unit_count ||
      !(left.profile == right.profile) || !(left.label == right.label)) {
    return false;
  }
  if (left.power_domains.size() != right.power_domains.size() ||
      left.cooling_domains.size() != right.cooling_domains.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.power_domains.size(); ++i) {
    if (!(left.power_domains[i] == right.power_domains[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < left.cooling_domains.size(); ++i) {
    if (!(left.cooling_domains[i] == right.cooling_domains[i])) {
      return false;
    }
  }
  return true;
}

Status validate_structure(const RackStructure& structure) {
  if (structure.id.empty()) {
    return make_error(ErrorCode::EmptyValue, "rack identity must not be empty",
                      ErrorDetail{.operation = "validate_structure"});
  }
  const auto units = validate_unit_count(structure.unit_count);
  if (!units) {
    return make_error(units.error().code, units.error().message,
                      ErrorDetail{.operation = "validate_structure",
                                  .subject = structure.id.text(),
                                  .expected = kMaxRackUnits,
                                  .actual = structure.unit_count});
  }
  if (structure.profile.id.empty()) {
    return make_error(ErrorCode::EmptyValue, "rack compatibility profile identity must not be empty",
                      ErrorDetail{.operation = "validate_structure",
                                  .subject = structure.id.text()});
  }
  const auto profile_traits =
      TraitSet::create(structure.profile.provides.traits());
  if (!profile_traits) {
    return make_error(profile_traits.error().code, profile_traits.error().message,
                      ErrorDetail{.operation = "validate_structure",
                                  .subject = structure.id.text()});
  }
  if (structure.power_domains.size() > kMaxDomainReferencesPerRack) {
    return make_error(ErrorCode::LimitExceeded,
                      "power domain associations exceed the documented bound",
                      ErrorDetail{.operation = "validate_structure",
                                  .subject = structure.id.text(),
                                  .expected = kMaxDomainReferencesPerRack,
                                  .actual = structure.power_domains.size()});
  }
  if (structure.cooling_domains.size() > kMaxDomainReferencesPerRack) {
    return make_error(ErrorCode::LimitExceeded,
                      "cooling domain associations exceed the documented bound",
                      ErrorDetail{.operation = "validate_structure",
                                  .subject = structure.id.text(),
                                  .expected = kMaxDomainReferencesPerRack,
                                  .actual = structure.cooling_domains.size()});
  }

  auto check_ordered_unique = [&structure](const auto& references,
                                           std::string_view what) -> Status {
    for (std::size_t i = 0; i < references.size(); ++i) {
      if (references[i].empty()) {
        return make_error(ErrorCode::EmptyValue,
                          std::string(what) + " reference must not be empty",
                          ErrorDetail{.operation = "validate_structure",
                                      .subject = structure.id.text()});
      }
      if (i > 0 && !(references[i - 1] < references[i])) {
        if (references[i] == references[i - 1]) {
          return make_error(ErrorCode::DuplicateDomainReference,
                            std::string(what) + " reference " + references[i].text() +
                                " appears more than once",
                            ErrorDetail{.operation = "validate_structure",
                                        .subject = structure.id.text(),
                                        .related = references[i].text()});
        }
        return make_error(ErrorCode::InvalidStateEncoding,
                          std::string(what) + " references are not in canonical order",
                          ErrorDetail{.operation = "validate_structure",
                                      .subject = structure.id.text()});
      }
    }
    return Status{};
  };

  const Status power = check_ordered_unique(structure.power_domains, "power domain");
  if (!power) {
    return power;
  }
  const Status cooling = check_ordered_unique(structure.cooling_domains, "cooling domain");
  if (!cooling) {
    return cooling;
  }
  return Status{};
}

// ---------------------------------------------------------------------------
// RackRecord
// ---------------------------------------------------------------------------

std::optional<RackMemberId> RackRecord::member_of_asset(const AssetId& asset_id) const {
  for (const auto& [member_id, member] : members) {
    if (member.asset_id == asset_id) {
      return member_id;
    }
  }
  return std::nullopt;
}

bool RackRecord::operator==(const RackRecord& other) const noexcept {
  return rack_records_equal(*this, other);
}

bool member_records_equal(const MemberRecord& left, const MemberRecord& right) noexcept {
  return left == right;
}

bool rack_records_equal(const RackRecord& left, const RackRecord& right) noexcept {
  if (!structures_equal(left.structure, right.structure) || left.lifecycle != right.lifecycle ||
      !(left.generation == right.generation) || !(left.revision == right.revision) ||
      !(left.membership_generation == right.membership_generation) ||
      left.provenance_dropped != right.provenance_dropped ||
      left.idempotency_dropped != right.idempotency_dropped) {
    return false;
  }
  if (left.members.size() != right.members.size()) {
    return false;
  }
  auto left_member = left.members.begin();
  auto right_member = right.members.begin();
  for (; left_member != left.members.end(); ++left_member, ++right_member) {
    if (!(left_member->first == right_member->first) ||
        !(left_member->second == right_member->second)) {
      return false;
    }
  }
  if (left.provenance.size() != right.provenance.size() ||
      left.generation_evidence.size() != right.generation_evidence.size() ||
      left.idempotency.size() != right.idempotency.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.provenance.size(); ++i) {
    if (!(left.provenance[i] == right.provenance[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < left.generation_evidence.size(); ++i) {
    if (!(left.generation_evidence[i] == right.generation_evidence[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < left.idempotency.size(); ++i) {
    if (!(left.idempotency[i] == right.idempotency[i])) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// RackView
// ---------------------------------------------------------------------------

std::vector<MemberRecord> RackView::members(MemberOrder order) const {
  std::vector<MemberRecord> result;
  result.reserve(record_.members.size());
  for (const auto& [member_id, member] : record_.members) {
    (void)member_id;
    result.push_back(member);
  }
  if (order == MemberOrder::MountOrder) {
    std::sort(result.begin(), result.end(), mount_order_less);
  }
  return result;
}

std::optional<MemberRecord> RackView::member(const RackMemberId& member_id) const {
  const auto found = record_.members.find(member_id);
  if (found == record_.members.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<OccupancyRecord> RackView::occupancy() const {
  std::vector<OccupancyRecord> result;
  result.reserve(record_.members.size());
  for (const MemberRecord& member : members(MemberOrder::MountOrder)) {
    OccupancyRecord entry;
    entry.member_id = member.member_id;
    entry.asset_id = member.asset_id;
    entry.mount = member.mount;
    entry.state = member.state;
    entry.units = member.mount.touched_units();
    result.push_back(std::move(entry));
  }
  return result;
}

std::vector<FreeSpan> RackView::free_spans() const {
  std::vector<FreeSpan> result;
  const SlotRange extent = record_.structure.slot_extent();
  if (!extent.is_valid()) {
    return result;
  }

  std::vector<SlotRange> occupied;
  occupied.reserve(record_.members.size());
  for (const auto& [member_id, member] : record_.members) {
    (void)member_id;
    if (!member.mount.is_zero_u()) {
      occupied.push_back(member.mount.span());
    }
  }
  std::sort(occupied.begin(), occupied.end());

  std::vector<SlotRange> free_ranges;
  std::uint32_t cursor = extent.begin();
  for (const SlotRange& span : occupied) {
    if (span.begin() > cursor) {
      const auto gap = SlotRange::create(cursor, span.begin());
      if (gap) {
        free_ranges.push_back(gap.value());
      }
    }
    if (span.end() > cursor) {
      cursor = span.end();
    }
  }
  if (cursor < extent.end()) {
    const auto tail = SlotRange::create(cursor, extent.end());
    if (tail) {
      free_ranges.push_back(tail.value());
    }
  }

  result.reserve(free_ranges.size());
  for (const SlotRange& span : free_ranges) {
    FreeSpan entry;
    entry.span = span;
    const RackUnitRange units = span.touched_units();
    if (units.is_valid()) {
      entry.units = units;
    }
    result.push_back(std::move(entry));
  }
  return result;
}

std::vector<SharedSpanAvailability> RackView::shared_availability() const {
  std::vector<SharedSpanAvailability> result;
  for (const auto& [member_id, member] : record_.members) {
    (void)member_id;
    if (!member.mount.is_shared()) {
      continue;
    }
    const auto existing = std::find_if(
        result.begin(), result.end(), [&member](const SharedSpanAvailability& entry) {
          return entry.span.is_identical_to(member.mount.span()) &&
                 entry.shared_class == member.mount.shared_class();
        });
    if (existing == result.end()) {
      SharedSpanAvailability entry;
      entry.span = member.mount.span();
      entry.shared_class = member.mount.shared_class();
      entry.capacity = member.mount.share_capacity();
      entry.occupied = 1;
      result.push_back(std::move(entry));
    } else {
      ++existing->occupied;
    }
  }
  std::sort(result.begin(), result.end(),
            [](const SharedSpanAvailability& a, const SharedSpanAvailability& b) {
              if (!(a.span == b.span)) {
                return a.span < b.span;
              }
              return a.shared_class < b.shared_class;
            });
  return result;
}

StateDigest RackView::state_digest() const { return internal::rack_record_digest(record_); }

// ---------------------------------------------------------------------------
// RackSnapshot
// ---------------------------------------------------------------------------

std::size_t RackSnapshot::member_count() const noexcept {
  std::size_t total = 0;
  for (const RackRecord& record : racks_) {
    total += record.members.size();
  }
  return total;
}

const RackRecord* RackSnapshot::find(const RackId& rack_id) const noexcept {
  const auto found = std::lower_bound(racks_.begin(), racks_.end(), rack_id,
                                      [](const RackRecord& record, const RackId& id) {
                                        return record.structure.id < id;
                                      });
  if (found == racks_.end() || !(found->structure.id == rack_id)) {
    return nullptr;
  }
  return &*found;
}

StateDigest RackSnapshot::state_digest() const {
  internal::ByteWriter writer;
  writer.u64(store_epoch_.value());
  writer.u64(store_sequence_.value());
  writer.u8(produced_by_.has_value() ? 1u : 0u);
  if (produced_by_.has_value()) {
    writer.text(produced_by_->text());
  }
  writer.u32(static_cast<std::uint32_t>(racks_.size()));
  for (const RackRecord& record : racks_) {
    const StateDigest digest = internal::rack_record_digest(record);
    writer.digest(digest);
  }
  return StateDigest::domain(kSnapshotDigestDomain, std::move(writer).take());
}

Status RackSnapshot::validate() const {
  if (racks_.size() > kMaxRacks) {
    return make_error(ErrorCode::LimitExceeded, "snapshot holds more racks than the bound permits",
                      ErrorDetail{.operation = "RackSnapshot::validate",
                                  .expected = kMaxRacks,
                                  .actual = racks_.size()});
  }
  for (std::size_t i = 0; i < racks_.size(); ++i) {
    if (racks_[i].structure.id.empty()) {
      return make_error(ErrorCode::EmptyValue, "snapshot holds a rack without an identity",
                        ErrorDetail{.operation = "RackSnapshot::validate"});
    }
    if (i > 0 && !(racks_[i - 1].structure.id < racks_[i].structure.id)) {
      return make_error(ErrorCode::InvalidStateEncoding,
                        "snapshot racks are not in ascending identity order",
                        ErrorDetail{.operation = "RackSnapshot::validate",
                                    .subject = racks_[i].structure.id.text()});
    }
    const Status status = internal::validate_rack_record(racks_[i], "RackSnapshot::validate");
    if (!status) {
      return status;
    }
    const RackRecord& record = racks_[i];
    if (!record.generation_evidence.empty()) {
      const StateDigest current = internal::rack_record_digest(record);
      if (!(record.generation_evidence.back().state_digest == current)) {
        return make_error(ErrorCode::IntegrityCheckFailed,
                          "retained generation evidence does not match the state it describes",
                          ErrorDetail{.operation = "RackSnapshot::validate",
                                      .subject = record.structure.id.text(),
                                      .related = record.generation_evidence.back()
                                                     .state_digest.to_hex(),
                                      .items = {current.to_hex()}});
      }
    }
  }
  return Status{};
}

}  // namespace rackregistry
