// Rack Registry - mounting coordinate model and interval semantics.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/mount.hpp"

#include <string>

#include "rack_registry/text.hpp"

namespace rackregistry {
namespace {

// Parses an unsigned decimal value with overflow detection. Leading zeros are
// accepted; an empty string or any non-digit is rejected.
bool parse_uint64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (!ascii_digit(c)) {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (static_cast<std::uint64_t>(-1) - digit) / 10u) {
      return false;
    }
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

Result<std::uint64_t> parse_count(std::string_view text, std::string_view what) {
  std::uint64_t value = 0;
  if (!parse_uint64(text, value)) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(what) + " must be a decimal integer",
                      ErrorDetail{.operation = std::string(what), .subject = std::string(text)});
  }
  return value;
}

// Parses "U10", "u10" or "10" into a 1-based unit number.
Result<RackUnitIndex> parse_unit_number(std::string_view text) {
  std::string_view digits = text;
  if (!digits.empty() && (digits.front() == 'U' || digits.front() == 'u')) {
    digits.remove_prefix(1);
  }
  const auto value = parse_count(digits, "rack unit");
  if (!value) {
    return value.error();
  }
  return RackUnitIndex::create(value.value());
}

// Parses "U10-U12" or "U10" into an inclusive unit range. Unparseable text is
// reported as InvalidRange so every consumer of the coordinate grammar sees the
// same code for the same class of defect.
Result<RackUnitRange> parse_unit_range(std::string_view text) {
  const std::size_t separator = text.find('-');
  if (separator == std::string_view::npos) {
    const auto unit = RackUnitIndex::parse(text);
    if (!unit) {
      return unit.error();
    }
    return RackUnitRange::single(unit.value());
  }
  const auto first = RackUnitIndex::parse(text.substr(0, separator));
  if (!first) {
    return first.error();
  }
  const auto last = RackUnitIndex::parse(text.substr(separator + 1));
  if (!last) {
    return last.error();
  }
  return RackUnitRange::inclusive(first.value().value(), last.value().value());
}

// Parses "[3,5)" into a half-open slot range.
Result<SlotRange> parse_slot_range(std::string_view text) {
  if (text.size() < 5 || text.front() != '[' || text.back() != ')') {
    return make_error(ErrorCode::InvalidMountSpan,
                      "slot range must be written as [begin,end)",
                      ErrorDetail{.operation = "SlotRange", .subject = std::string(text)});
  }
  const std::string_view inner = text.substr(1, text.size() - 2);
  const std::size_t separator = inner.find(',');
  if (separator == std::string_view::npos) {
    return make_error(ErrorCode::InvalidMountSpan,
                      "slot range must be written as [begin,end)",
                      ErrorDetail{.operation = "SlotRange", .subject = std::string(text)});
  }
  const auto begin = parse_count(inner.substr(0, separator), "slot range begin");
  if (!begin) {
    return begin.error();
  }
  const auto end = parse_count(inner.substr(separator + 1), "slot range end");
  if (!end) {
    return end.error();
  }
  return SlotRange::create(begin.value(), end.value());
}

// Parses either "[3,5)" or "U10-U12" into a slot range.
Result<SlotRange> parse_mount_coordinate(std::string_view text) {
  if (!text.empty() && text.front() == '[') {
    return parse_slot_range(text);
  }
  const auto units = parse_unit_range(text);
  if (!units) {
    return units.error();
  }
  return SlotRange::whole_units(units.value());
}

std::string uint64_text(std::uint64_t value) { return std::to_string(value); }

}  // namespace

// ---------------------------------------------------------------------------
// RackUnitIndex
// ---------------------------------------------------------------------------

Result<RackUnitIndex> RackUnitIndex::create(std::uint64_t value) {
  if (value < 1 || value > kMaxRackUnits) {
    return make_error(ErrorCode::InvalidRange, "rack unit must be between 1 and " +
                                                   std::to_string(kMaxRackUnits),
                      ErrorDetail{.operation = "RackUnitIndex",
                                  .expected = kMaxRackUnits,
                                  .actual = value});
  }
  return RackUnitIndex(static_cast<std::uint32_t>(value));
}

Result<RackUnitIndex> RackUnitIndex::parse(std::string_view text) {
  const auto unit = parse_unit_number(text);
  if (!unit) {
    return make_error(ErrorCode::InvalidRange, "rack unit must be a decimal integer",
                      ErrorDetail{.operation = "RackUnitIndex", .subject = std::string(text)});
  }
  return unit;
}

std::string RackUnitIndex::to_text() const {
  if (!is_valid()) {
    return "U?";
  }
  return "U" + uint64_text(value_);
}

// ---------------------------------------------------------------------------
// MountSlotIndex
// ---------------------------------------------------------------------------

Result<MountSlotIndex> MountSlotIndex::create(std::uint64_t value) {
  if (value < 1 || value >= kMaxMountSlotExclusive) {
    return make_error(ErrorCode::InvalidRange,
                      "mount slot must be between 1 and " +
                          std::to_string(kMaxMountSlotExclusive - 1),
                      ErrorDetail{.operation = "MountSlotIndex",
                                  .expected = kMaxMountSlotExclusive - 1,
                                  .actual = value});
  }
  return MountSlotIndex(static_cast<std::uint32_t>(value));
}

// ---------------------------------------------------------------------------
// RackUnitRange
// ---------------------------------------------------------------------------

Result<RackUnitRange> RackUnitRange::create(std::uint64_t first, std::uint64_t last) {
  if (first < 1 || last <= first || last > static_cast<std::uint64_t>(kMaxRackUnits) + 1u) {
    return make_error(ErrorCode::InvalidRange,
                      "rack unit range must satisfy 1 <= first < last <= " +
                          std::to_string(kMaxRackUnits + 1),
                      ErrorDetail{.operation = "RackUnitRange", .expected = first, .actual = last});
  }
  RackUnitRange range;
  range.first_ = static_cast<std::uint32_t>(first);
  range.last_ = static_cast<std::uint32_t>(last);
  return range;
}

Result<RackUnitRange> RackUnitRange::inclusive(std::uint64_t first, std::uint64_t last) {
  if (last < first) {
    return make_error(ErrorCode::InvalidRange, "inclusive rack unit range ends before it starts",
                      ErrorDetail{.operation = "RackUnitRange", .expected = first, .actual = last});
  }
  if (last == static_cast<std::uint64_t>(-1)) {
    return make_error(ErrorCode::InvalidRange, "inclusive rack unit range overflows",
                      ErrorDetail{.operation = "RackUnitRange", .actual = last});
  }
  return create(first, last + 1);
}

Result<RackUnitRange> RackUnitRange::single(RackUnitIndex unit) {
  if (!unit.is_valid()) {
    return make_error(ErrorCode::InvalidRange, "rack unit is outside the valid range",
                      ErrorDetail{.operation = "RackUnitRange", .actual = unit.value()});
  }
  return create(unit.value(), static_cast<std::uint64_t>(unit.value()) + 1u);
}

Result<RackUnitRange> RackUnitRange::parse(std::string_view text) { return parse_unit_range(text); }

std::string RackUnitRange::to_text() const {
  if (!is_valid()) {
    return "U?";
  }
  if (unit_count() == 1) {
    return "U" + uint64_text(first_);
  }
  return "U" + uint64_text(first_) + "-U" + uint64_text(inclusive_last());
}

// ---------------------------------------------------------------------------
// SlotRange
// ---------------------------------------------------------------------------

Result<SlotRange> SlotRange::create(std::uint64_t begin, std::uint64_t end) {
  if (begin < 1 || end <= begin || end > kMaxMountSlotExclusive) {
    return make_error(ErrorCode::InvalidRange,
                      "mount slot range must satisfy 1 <= begin < end <= " +
                          std::to_string(kMaxMountSlotExclusive),
                      ErrorDetail{.operation = "SlotRange", .expected = begin, .actual = end});
  }
  SlotRange range;
  range.begin_ = static_cast<std::uint32_t>(begin);
  range.end_ = static_cast<std::uint32_t>(end);
  return range;
}

Result<SlotRange> SlotRange::inclusive(std::uint64_t first, std::uint64_t last) {
  if (last < first) {
    return make_error(ErrorCode::InvalidRange, "inclusive slot range ends before it starts",
                      ErrorDetail{.operation = "SlotRange", .expected = first, .actual = last});
  }
  if (last == static_cast<std::uint64_t>(-1)) {
    return make_error(ErrorCode::InvalidRange, "inclusive slot range overflows",
                      ErrorDetail{.operation = "SlotRange", .actual = last});
  }
  return create(first, last + 1);
}

Result<SlotRange> SlotRange::single(MountSlotIndex slot) {
  if (!slot.is_valid()) {
    return make_error(ErrorCode::InvalidRange, "mount slot is outside the valid range",
                      ErrorDetail{.operation = "SlotRange", .actual = slot.value()});
  }
  return create(slot.value(), static_cast<std::uint64_t>(slot.value()) + 1u);
}

Result<SlotRange> SlotRange::half_unit(RackUnitIndex unit, HalfSlot half) {
  if (!unit.is_valid()) {
    return make_error(ErrorCode::InvalidRange, "rack unit is outside the valid range",
                      ErrorDetail{.operation = "SlotRange", .actual = unit.value()});
  }
  const std::uint64_t first = static_cast<std::uint64_t>(unit.value() - 1u) *
                                  kMountSlotsPerRackUnit +
                              1u + static_cast<std::uint64_t>(half);
  return create(first, first + 1u);
}

Result<SlotRange> SlotRange::whole_units(RackUnitRange units) {
  if (!units.is_valid()) {
    return make_error(ErrorCode::InvalidRange, "rack unit range is outside the valid range",
                      ErrorDetail{.operation = "SlotRange"});
  }
  const std::uint64_t begin =
      static_cast<std::uint64_t>(units.first() - 1u) * kMountSlotsPerRackUnit + 1u;
  const std::uint64_t end =
      static_cast<std::uint64_t>(units.last() - 1u) * kMountSlotsPerRackUnit + 1u;
  return create(begin, end);
}

Result<SlotRange> SlotRange::parse(std::string_view text) { return parse_slot_range(text); }

RackUnitRange SlotRange::touched_units() const noexcept {
  RackUnitRange units;
  if (!is_valid()) {
    return units;
  }
  const std::uint32_t first = (begin_ + 1u) / kMountSlotsPerRackUnit;
  const std::uint32_t last = end_ / kMountSlotsPerRackUnit + 1u;
  const auto created = RackUnitRange::create(first, last);
  return created.has_value() ? created.value() : RackUnitRange{};
}

std::vector<SlotRange> SlotRange::subtract(const SlotRange& other) const {
  std::vector<SlotRange> parts;
  if (!is_valid() || !other.is_valid() || !overlaps(other)) {
    if (is_valid()) {
      parts.push_back(*this);
    }
    return parts;
  }
  if (other.begin_ > begin_) {
    const auto head = SlotRange::create(begin_, other.begin_);
    if (head) {
      parts.push_back(head.value());
    }
  }
  if (other.end_ < end_) {
    const auto tail = SlotRange::create(other.end_, end_);
    if (tail) {
      parts.push_back(tail.value());
    }
  }
  return parts;
}

std::string SlotRange::to_text() const {
  return "[" + uint64_text(begin_) + "," + uint64_text(end_) + ")";
}

// ---------------------------------------------------------------------------
// MountKind
// ---------------------------------------------------------------------------

std::string_view mount_kind_name(MountKind kind) noexcept {
  switch (kind) {
    case MountKind::FullSpan:
      return "full";
    case MountKind::SharedSpan:
      return "shared";
    case MountKind::ZeroU:
      return "zero-u";
  }
  return "unknown";
}

Result<MountKind> parse_mount_kind(std::string_view text) {
  if (text == "full") {
    return MountKind::FullSpan;
  }
  if (text == "shared") {
    return MountKind::SharedSpan;
  }
  if (text == "zero-u") {
    return MountKind::ZeroU;
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown mount kind",
                    ErrorDetail{.operation = "MountKind", .subject = std::string(text)});
}

// ---------------------------------------------------------------------------
// MountSpan
// ---------------------------------------------------------------------------

Result<MountSpan> MountSpan::full(SlotRange span) {
  if (!span.is_valid()) {
    return make_error(ErrorCode::InvalidMountSpan, "full-span mount requires a non-empty slot range",
                      ErrorDetail{.operation = "MountSpan"});
  }
  MountSpan result;
  result.kind_ = MountKind::FullSpan;
  result.span_ = span;
  return result;
}

Result<MountSpan> MountSpan::full_units(RackUnitRange units) {
  const auto span = SlotRange::whole_units(units);
  if (!span) {
    return span.error();
  }
  return full(span.value());
}

Result<MountSpan> MountSpan::shared(SlotRange span, SharedMountClass shared_class,
                                    std::uint32_t share_capacity) {
  if (!span.is_valid()) {
    return make_error(ErrorCode::InvalidMountSpan,
                      "shared-span mount requires a non-empty slot range",
                      ErrorDetail{.operation = "MountSpan"});
  }
  if (shared_class.empty()) {
    return make_error(ErrorCode::InvalidMountSpan,
                      "shared-span mount requires a shared-mount class",
                      ErrorDetail{.operation = "MountSpan"});
  }
  if (share_capacity < 1) {
    return make_error(ErrorCode::InvalidMountSpan,
                      "shared-span mount requires a capacity of at least 1",
                      ErrorDetail{.operation = "MountSpan"});
  }
  MountSpan result;
  result.kind_ = MountKind::SharedSpan;
  result.span_ = span;
  result.shared_class_ = std::move(shared_class);
  result.share_capacity_ = share_capacity;
  return result;
}

Result<MountSpan> MountSpan::shared_units(RackUnitRange units, SharedMountClass shared_class,
                                          std::uint32_t share_capacity) {
  const auto span = SlotRange::whole_units(units);
  if (!span) {
    return span.error();
  }
  return shared(span.value(), std::move(shared_class), share_capacity);
}

Result<MountSpan> MountSpan::parse(std::string_view text) {
  if (text == "zero-u") {
    return MountSpan::zero_u();
  }
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return make_error(ErrorCode::InvalidMountSpan,
                      "mount span must begin with 'full:', 'shared:' or be 'zero-u'",
                      ErrorDetail{.operation = "MountSpan", .subject = std::string(text)});
  }
  const std::string_view kind_text = text.substr(0, separator);
  const std::string_view rest = text.substr(separator + 1);
  const auto kind = parse_mount_kind(kind_text);
  if (!kind) {
    return kind.error();
  }

  if (kind.value() == MountKind::FullSpan) {
    const auto span = parse_mount_coordinate(rest);
    if (!span) {
      return span.error();
    }
    return full(span.value());
  }

  if (kind.value() == MountKind::SharedSpan) {
    const std::size_t first_slash = rest.find('/');
    if (first_slash == std::string_view::npos) {
      return make_error(ErrorCode::InvalidMountSpan,
                        "shared mount span must be written as shared:<coordinate>/<class>/<capacity>",
                        ErrorDetail{.operation = "MountSpan", .subject = std::string(text)});
    }
    const std::size_t second_slash = rest.find('/', first_slash + 1);
    if (second_slash == std::string_view::npos) {
      return make_error(ErrorCode::InvalidMountSpan,
                        "shared mount span must be written as shared:<coordinate>/<class>/<capacity>",
                        ErrorDetail{.operation = "MountSpan", .subject = std::string(text)});
    }
    const auto span = parse_mount_coordinate(rest.substr(0, first_slash));
    if (!span) {
      return span.error();
    }
    const auto shared_class =
        SharedMountClass::parse(rest.substr(first_slash + 1, second_slash - first_slash - 1));
    if (!shared_class) {
      return shared_class.error();
    }
    const auto capacity = parse_count(rest.substr(second_slash + 1), "shared mount capacity");
    if (!capacity) {
      return capacity.error();
    }
    if (capacity.value() < 1 || capacity.value() > 1024) {
      return make_error(ErrorCode::InvalidMountSpan,
                        "shared mount capacity must be between 1 and 1024",
                        ErrorDetail{.operation = "MountSpan", .actual = capacity.value()});
    }
    return shared(span.value(), shared_class.value(),
                  static_cast<std::uint32_t>(capacity.value()));
  }

  return make_error(ErrorCode::InvalidMountSpan,
                    "a zero-U mount span takes no coordinate and must be written as 'zero-u'",
                    ErrorDetail{.operation = "MountSpan", .subject = std::string(text)});
}

std::string MountSpan::to_text() const {
  if (kind_ == MountKind::ZeroU) {
    return "zero-u";
  }
  std::string out(mount_kind_name(kind_));
  out.push_back(':');
  out.append(span_.to_text());
  if (kind_ == MountKind::SharedSpan) {
    out.push_back('/');
    out.append(shared_class_.text());
    out.push_back('/');
    out.append(uint64_text(share_capacity_));
  }
  return out;
}

std::optional<RackUnitRange> MountSpan::touched_units() const noexcept {
  if (kind_ == MountKind::ZeroU || !span_.is_valid()) {
    return std::nullopt;
  }
  const RackUnitRange units = span_.touched_units();
  if (!units.is_valid()) {
    return std::nullopt;
  }
  return units;
}

bool MountSpan::equals(const MountSpan& other) const noexcept {
  return kind_ == other.kind_ && span_ == other.span_ && shared_class_ == other.shared_class_ &&
         share_capacity_ == other.share_capacity_;
}

bool MountSpan::co_occupies(const MountSpan& other) const noexcept {
  return kind_ == MountKind::SharedSpan && other.kind_ == MountKind::SharedSpan &&
         shared_class_ == other.shared_class_ && span_.is_identical_to(other.span_);
}

bool MountSpan::conflicts_with(const MountSpan& other) const noexcept {
  if (kind_ == MountKind::ZeroU || other.kind_ == MountKind::ZeroU) {
    return false;
  }
  if (!span_.overlaps(other.span_)) {
    return false;
  }
  return !co_occupies(other);
}

// ---------------------------------------------------------------------------
// Rack extent
// ---------------------------------------------------------------------------

Result<std::uint32_t> validate_unit_count(std::uint64_t unit_count) {
  if (unit_count < 1 || unit_count > kMaxRackUnits) {
    return make_error(ErrorCode::InvalidRange,
                      "rack unit count must be between 1 and " + std::to_string(kMaxRackUnits),
                      ErrorDetail{.operation = "unit_count",
                                  .expected = kMaxRackUnits,
                                  .actual = unit_count});
  }
  return static_cast<std::uint32_t>(unit_count);
}

Result<SlotRange> rack_slot_extent(std::uint32_t unit_count) {
  const auto validated = validate_unit_count(unit_count);
  if (!validated) {
    return validated.error();
  }
  return SlotRange::create(1, static_cast<std::uint64_t>(unit_count) * kMountSlotsPerRackUnit + 1u);
}

}  // namespace rackregistry
