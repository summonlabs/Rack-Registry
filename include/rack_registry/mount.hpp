// Rack Registry - mounting coordinate model and interval semantics.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/limits.hpp"
#include "rack_registry/result.hpp"
#include "rack_registry/version.hpp"

namespace rackregistry {

// ---------------------------------------------------------------------------
// Coordinate model
// ---------------------------------------------------------------------------
//
// Vertical position inside a rack is expressed in *mount slots*. One rack unit
// spans exactly kMountSlotsPerRackUnit (2) mount slots, so the coordinate space
// resolves half-unit mountings without resorting to floating point.
//
//   rack unit U  <->  mount slots { 2U-1, 2U }
//
// Slot numbers are 1-based. The lower half of unit U is slot 2U-1 and the
// upper half is slot 2U.
//
// EVERY interval in this library is half open: the range [begin, end) contains
// begin and excludes end. Two ranges are disjoint when one begins at or after
// the other's end, so adjacent ranges (for example [1,3) and [3,5)) never
// overlap. Unit ranges follow the same rule. Text forms differ deliberately:
// slot ranges render as "[begin,end)" and unit ranges render inclusively as
// "U10-U12", because that is how rack units are spoken about on a data center
// floor. Both parsers are strict and both round-trip.

// One 1-based rack unit number, valid from 1 to kMaxRackUnits.
class RackUnitIndex {
 public:
  constexpr RackUnitIndex() noexcept = default;
  constexpr explicit RackUnitIndex(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] static Result<RackUnitIndex> create(std::uint64_t value);
  [[nodiscard]] static Result<RackUnitIndex> parse(std::string_view text);

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return value_ >= 1 && value_ <= kMaxRackUnits;
  }
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr bool operator==(const RackUnitIndex& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const RackUnitIndex& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const RackUnitIndex& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const RackUnitIndex& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const RackUnitIndex& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const RackUnitIndex& other) const noexcept {
    return value_ >= other.value_;
  }

 private:
  std::uint32_t value_ = 0;
};

// One 1-based mount slot number, valid from 1 to kMaxMountSlotExclusive - 1.
class MountSlotIndex {
 public:
  constexpr MountSlotIndex() noexcept = default;
  constexpr explicit MountSlotIndex(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] static Result<MountSlotIndex> create(std::uint64_t value);

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return value_ >= 1 && value_ < kMaxMountSlotExclusive;
  }
  [[nodiscard]] constexpr std::uint32_t rack_unit() const noexcept {
    return (value_ + 1u) / kMountSlotsPerRackUnit;
  }

  [[nodiscard]] constexpr bool operator==(const MountSlotIndex& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const MountSlotIndex& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const MountSlotIndex& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const MountSlotIndex& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const MountSlotIndex& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const MountSlotIndex& other) const noexcept {
    return value_ >= other.value_;
  }

 private:
  std::uint32_t value_ = 0;
};

// Which half of a rack unit a half-unit mounting occupies.
enum class HalfSlot : std::uint8_t { Lower = 0, Upper = 1 };

// A non-empty half-open interval of rack units: [first, last). `first` is at
// least 1 and `last` is at most kMaxRackUnits + 1.
class RackUnitRange {
 public:
  constexpr RackUnitRange() noexcept = default;

  [[nodiscard]] static Result<RackUnitRange> create(std::uint64_t first, std::uint64_t last);
  // `first` and `last` are inclusive unit numbers, the way rack units are
  // spoken about. An end below the start is rejected, never swapped.
  [[nodiscard]] static Result<RackUnitRange> inclusive(std::uint64_t first, std::uint64_t last);
  [[nodiscard]] static Result<RackUnitRange> single(RackUnitIndex unit);
  [[nodiscard]] static Result<RackUnitRange> parse(std::string_view text);

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return first_ >= 1 && last_ > first_ && last_ <= kMaxRackUnits + 1;
  }
  [[nodiscard]] constexpr std::uint32_t first() const noexcept { return first_; }
  [[nodiscard]] constexpr std::uint32_t last() const noexcept { return last_; }
  [[nodiscard]] constexpr std::uint32_t unit_count() const noexcept {
    return last_ > first_ ? last_ - first_ : 0u;
  }
  [[nodiscard]] constexpr std::uint32_t inclusive_last() const noexcept {
    return last_ > 0 ? last_ - 1u : 0u;
  }

  [[nodiscard]] bool contains(RackUnitIndex unit) const noexcept {
    return unit.value() >= first_ && unit.value() < last_;
  }
  [[nodiscard]] bool overlaps(const RackUnitRange& other) const noexcept {
    return first_ < other.last_ && other.first_ < last_;
  }

  // Human-facing inclusive form, for example "U10-U12".
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr bool operator==(const RackUnitRange& other) const noexcept {
    return first_ == other.first_ && last_ == other.last_;
  }
  [[nodiscard]] constexpr bool operator!=(const RackUnitRange& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] constexpr bool operator<(const RackUnitRange& other) const noexcept {
    return first_ != other.first_ ? first_ < other.first_ : last_ < other.last_;
  }

 private:
  std::uint32_t first_ = 0;
  std::uint32_t last_ = 0;
};

// A non-empty half-open interval of mount slots: [begin, end).
class SlotRange {
 public:
  constexpr SlotRange() noexcept = default;

  [[nodiscard]] static Result<SlotRange> create(std::uint64_t begin, std::uint64_t end);
  // Inclusive slot numbers; `last` below `first` is rejected, never swapped.
  [[nodiscard]] static Result<SlotRange> inclusive(std::uint64_t first, std::uint64_t last);
  [[nodiscard]] static Result<SlotRange> single(MountSlotIndex slot);
  [[nodiscard]] static Result<SlotRange> half_unit(RackUnitIndex unit, HalfSlot half);
  [[nodiscard]] static Result<SlotRange> whole_units(RackUnitRange units);
  [[nodiscard]] static Result<SlotRange> parse(std::string_view text);

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return begin_ >= 1 && end_ > begin_ && end_ <= kMaxMountSlotExclusive;
  }
  [[nodiscard]] constexpr std::uint32_t begin() const noexcept { return begin_; }
  [[nodiscard]] constexpr std::uint32_t end() const noexcept { return end_; }
  [[nodiscard]] constexpr std::uint32_t slot_count() const noexcept {
    return end_ > begin_ ? end_ - begin_ : 0u;
  }
  [[nodiscard]] constexpr std::uint32_t inclusive_last() const noexcept {
    return end_ > 0 ? end_ - 1u : 0u;
  }

  // The rack units this range touches, half open. Always non-empty for a valid
  // range, because a valid range covers at least one slot.
  [[nodiscard]] RackUnitRange touched_units() const noexcept;

  [[nodiscard]] constexpr bool contains_slot(std::uint32_t slot) const noexcept {
    return slot >= begin_ && slot < end_;
  }
  [[nodiscard]] constexpr bool contains(const SlotRange& other) const noexcept {
    return begin_ <= other.begin_ && other.end_ <= end_;
  }
  // Half-open semantics: [1,3) and [3,5) do not overlap.
  [[nodiscard]] constexpr bool overlaps(const SlotRange& other) const noexcept {
    return begin_ < other.end_ && other.begin_ < end_;
  }
  // True when both ranges describe exactly the same slots.
  [[nodiscard]] constexpr bool is_identical_to(const SlotRange& other) const noexcept {
    return begin_ == other.begin_ && end_ == other.end_;
  }

  // Returns this range with `other` removed. The result may be empty (both
  // parts absent), a single range, or two disjoint ranges.
  [[nodiscard]] std::vector<SlotRange> subtract(const SlotRange& other) const;

  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr bool operator==(const SlotRange& other) const noexcept {
    return begin_ == other.begin_ && end_ == other.end_;
  }
  [[nodiscard]] constexpr bool operator!=(const SlotRange& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] constexpr bool operator<(const SlotRange& other) const noexcept {
    return begin_ != other.begin_ ? begin_ < other.begin_ : end_ < other.end_;
  }

 private:
  std::uint32_t begin_ = 0;
  std::uint32_t end_ = 0;
};

// How a member is mounted in a rack.
enum class MountKind : std::uint8_t {
  // Exclusive occupation of an interval of mount slots.
  FullSpan = 0,
  // Occupation of an interval of mount slots that may legally be shared with
  // other members declaring the same shared-mount class over exactly the same
  // interval, up to the declared capacity.
  SharedSpan = 1,
  // Mounted outside the unit space (side or rear rail). Zero-U members occupy
  // no slot and never conflict with any other member.
  ZeroU = 2,
};

[[nodiscard]] RACK_REGISTRY_API std::string_view mount_kind_name(MountKind kind) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<MountKind> parse_mount_kind(std::string_view text);

// Purely structural identity of a mount coordinate: the kind and the slot
// interval, without the shared-mount parameters. Two mount spans with the same
// coordinate occupy the same physical place.
struct MountCoordinate {
  MountKind kind = MountKind::FullSpan;
  SlotRange span{};

  [[nodiscard]] bool operator==(const MountCoordinate& other) const noexcept {
    return kind == other.kind && span == other.span;
  }
  [[nodiscard]] bool operator!=(const MountCoordinate& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] bool operator<(const MountCoordinate& other) const noexcept {
    if (kind != other.kind) {
      return static_cast<std::uint8_t>(kind) < static_cast<std::uint8_t>(other.kind);
    }
    return span < other.span;
  }
};

// A validated mount span. Construction always goes through a factory, so an
// invalid span cannot exist.
class MountSpan {
 public:
  constexpr MountSpan() noexcept = default;

  [[nodiscard]] static Result<MountSpan> full(SlotRange span);
  [[nodiscard]] static Result<MountSpan> full_units(RackUnitRange units);
  [[nodiscard]] static Result<MountSpan> shared(SlotRange span,
                                                SharedMountClass shared_class,
                                                std::uint32_t share_capacity);
  [[nodiscard]] static Result<MountSpan> shared_units(RackUnitRange units,
                                                      SharedMountClass shared_class,
                                                      std::uint32_t share_capacity);
  [[nodiscard]] static MountSpan zero_u() noexcept { return MountSpan{}; }

  // Canonical text form: "full:[1,3)", "shared:[1,3)/smc:psu-bay/2", "zero-u".
  [[nodiscard]] static Result<MountSpan> parse(std::string_view text);
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr MountKind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr SlotRange span() const noexcept { return span_; }
  [[nodiscard]] constexpr const SharedMountClass& shared_class() const noexcept {
    return shared_class_;
  }
  [[nodiscard]] constexpr std::uint32_t share_capacity() const noexcept {
    return share_capacity_;
  }
  [[nodiscard]] constexpr bool is_zero_u() const noexcept { return kind_ == MountKind::ZeroU; }
  [[nodiscard]] constexpr bool is_shared() const noexcept { return kind_ == MountKind::SharedSpan; }

  [[nodiscard]] MountCoordinate coordinate() const noexcept { return MountCoordinate{kind_, span_}; }

  // Rack units this span touches; nullopt for a zero-U mount.
  [[nodiscard]] std::optional<RackUnitRange> touched_units() const noexcept;

  // Structural equality over every field, encoded explicitly rather than
  // relying on the incidental layout of the structure.
  [[nodiscard]] bool equals(const MountSpan& other) const noexcept;

  // Overlap classification of two spans, defined by the occupation rules:
  //
  //   * a zero-U span never conflicts with anything, including another zero-U
  //     span, because it occupies no slot;
  //   * two non-zero spans conflict when their slot intervals intersect,
  //     unless both are shared spans of the same shared-mount class over
  //     exactly the same interval;
  //   * a shared span whose class or capacity differs from another span over
  //     the same interval conflicts.
  [[nodiscard]] bool conflicts_with(const MountSpan& other) const noexcept;

  // True when both spans are shared, declare the same class, and cover
  // exactly the same slot interval. Co-occupants must also declare the same
  // capacity; comparing that is the caller's job so the rejection can carry a
  // precise reason.
  [[nodiscard]] bool co_occupies(const MountSpan& other) const noexcept;

 private:
  MountKind kind_ = MountKind::ZeroU;
  SlotRange span_{};
  SharedMountClass shared_class_{};
  std::uint32_t share_capacity_ = 0;
};

// The largest half-open slot interval a rack with `unit_count` rack units
// provides.
[[nodiscard]] RACK_REGISTRY_API Result<SlotRange> rack_slot_extent(std::uint32_t unit_count);

// Validates a rack unit count.
[[nodiscard]] RACK_REGISTRY_API Result<std::uint32_t> validate_unit_count(std::uint64_t unit_count);

}  // namespace rackregistry
