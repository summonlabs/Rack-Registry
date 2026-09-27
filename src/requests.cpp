// Rack Registry - mutation command enumeration.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "rack_registry/requests.hpp"

namespace rackregistry {

std::string_view operation_kind_name(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::RegisterRack:
      return "register_rack";
    case OperationKind::SetRackStructure:
      return "set_rack_structure";
    case OperationKind::TransitionLifecycle:
      return "transition_lifecycle";
    case OperationKind::InsertMember:
      return "insert_member";
    case OperationKind::RemoveMember:
      return "remove_member";
    case OperationKind::MoveMember:
      return "move_member";
    case OperationKind::ReplaceMember:
      return "replace_member";
    case OperationKind::RestoreSnapshot:
      return "restore_snapshot";
  }
  return "unknown";
}

Result<OperationKind> parse_operation_kind(std::string_view text) {
  for (std::size_t i = 0; i < kOperationKindCount; ++i) {
    const auto kind = static_cast<OperationKind>(i);
    if (operation_kind_name(kind) == text) {
      return kind;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown operation kind",
                    ErrorDetail{.operation = "OperationKind", .subject = std::string(text)});
}

Result<OperationKind> operation_kind_from_value(std::uint8_t value) {
  if (value >= kOperationKindCount) {
    return make_error(ErrorCode::InvalidEnumValue, "operation kind value is outside its domain",
                      ErrorDetail{.operation = "OperationKind",
                                  .expected = kOperationKindCount - 1,
                                  .actual = value});
  }
  return static_cast<OperationKind>(value);
}

}  // namespace rackregistry
