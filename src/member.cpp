// Rack Registry - membership record implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/member.hpp"

#include <string>

namespace rackregistry {

std::string_view membership_state_name(MembershipState state) noexcept {
  switch (state) {
    case MembershipState::Installed:
      return "installed";
    case MembershipState::Reserved:
      return "reserved";
  }
  return "unknown";
}

Result<MembershipState> parse_membership_state(std::string_view text) {
  for (std::size_t i = 0; i < kMembershipStateCount; ++i) {
    const auto state = static_cast<MembershipState>(i);
    if (membership_state_name(state) == text) {
      return state;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown membership state",
                    ErrorDetail{.operation = "MembershipState", .subject = std::string(text)});
}

Result<MembershipState> membership_state_from_value(std::uint8_t value) {
  if (value >= kMembershipStateCount) {
    return make_error(ErrorCode::InvalidEnumValue, "membership state value is outside its domain",
                      ErrorDetail{.operation = "MembershipState",
                                  .expected = kMembershipStateCount - 1,
                                  .actual = value});
  }
  return static_cast<MembershipState>(value);
}

std::string_view member_order_name(MemberOrder order) noexcept {
  switch (order) {
    case MemberOrder::MountOrder:
      return "mount";
    case MemberOrder::IdentityOrder:
      return "identity";
  }
  return "unknown";
}

Result<MemberOrder> parse_member_order(std::string_view text) {
  for (std::size_t i = 0; i < 2; ++i) {
    const auto order = static_cast<MemberOrder>(i);
    if (member_order_name(order) == text) {
      return order;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown member ordering",
                    ErrorDetail{.operation = "MemberOrder", .subject = std::string(text)});
}

bool MemberRecord::operator==(const MemberRecord& other) const noexcept {
  if (!(member_id == other.member_id) || !(asset_id == other.asset_id) ||
      !mount.equals(other.mount) || state != other.state ||
      !(requirements == other.requirements) ||
      !(mount_generation == other.mount_generation) ||
      !(asset_generation == other.asset_generation) ||
      !(created_at_membership_generation == other.created_at_membership_generation) ||
      provenance_dropped != other.provenance_dropped) {
    return false;
  }
  if (provenance.size() != other.provenance.size()) {
    return false;
  }
  for (std::size_t i = 0; i < provenance.size(); ++i) {
    if (!(provenance[i] == other.provenance[i])) {
      return false;
    }
  }
  return true;
}

std::string FreeSpan::to_text() const {
  std::string out = span.to_text();
  if (units.has_value()) {
    out.append(" ");
    out.append(units->to_text());
  }
  return out;
}

std::string SharedSpanAvailability::to_text() const {
  std::string out = span.to_text();
  out.append(" ");
  out.append(shared_class.text());
  out.append(" occupied=");
  out.append(std::to_string(occupied));
  out.append("/");
  out.append(std::to_string(capacity));
  return out;
}

}  // namespace rackregistry
