// Rack Registry - error taxonomy implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/errors.hpp"

#include <string>
#include <utility>

#include "rack_registry/text.hpp"

namespace rackregistry {

ErrorCategory category_of(ErrorCode code) noexcept {
  const auto value = static_cast<std::uint16_t>(code);
  if (value == 0) {
    return ErrorCategory::None;
  }
  switch (value / 100) {
    case 1:
      return ErrorCategory::Input;
    case 2:
      return ErrorCategory::Identity;
    case 3:
      return ErrorCategory::Authority;
    case 4:
      return ErrorCategory::Lifecycle;
    case 5:
      return ErrorCategory::Occupancy;
    case 6:
      return ErrorCategory::Compatibility;
    case 7:
      return ErrorCategory::Persistence;
    case 8:
      return ErrorCategory::Limits;
    case 9:
      return ErrorCategory::Writer;
    default:
      return ErrorCategory::None;
  }
}

std::string_view code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::InvalidArgument:
      return "invalid_argument";
    case ErrorCode::MalformedIdentity:
      return "malformed_identity";
    case ErrorCode::IdentityTooLong:
      return "identity_too_long";
    case ErrorCode::InvalidCharacter:
      return "invalid_character";
    case ErrorCode::InvalidEnumValue:
      return "invalid_enum_value";
    case ErrorCode::InvalidRange:
      return "invalid_range";
    case ErrorCode::EmptyValue:
      return "empty_value";
    case ErrorCode::InvalidUtf8:
      return "invalid_utf8";
    case ErrorCode::InvalidTimestamp:
      return "invalid_timestamp";
    case ErrorCode::DuplicateRackId:
      return "duplicate_rack_id";
    case ErrorCode::UnknownRackId:
      return "unknown_rack_id";
    case ErrorCode::DuplicateMemberId:
      return "duplicate_member_id";
    case ErrorCode::UnknownMemberId:
      return "unknown_member_id";
    case ErrorCode::DuplicateAssetPlacement:
      return "duplicate_asset_placement";
    case ErrorCode::DuplicateDomainReference:
      return "duplicate_domain_reference";
    case ErrorCode::DuplicateTrait:
      return "duplicate_trait";
    case ErrorCode::RequestIdConflict:
      return "request_id_conflict";
    case ErrorCode::StaleRackGeneration:
      return "stale_rack_generation";
    case ErrorCode::StaleMembershipGeneration:
      return "stale_membership_generation";
    case ErrorCode::StaleLifecycleState:
      return "stale_lifecycle_state";
    case ErrorCode::GenerationNotRetained:
      return "generation_not_retained";
    case ErrorCode::StaleWriterFenced:
      return "stale_writer_fenced";
    case ErrorCode::StoreEpochRegression:
      return "store_epoch_regression";
    case ErrorCode::SequenceRegression:
      return "sequence_regression";
    case ErrorCode::SnapshotIdentityMismatch:
      return "snapshot_identity_mismatch";
    case ErrorCode::LifecycleTransitionNotAllowed:
      return "lifecycle_transition_not_allowed";
    case ErrorCode::LifecycleMutationForbidden:
      return "lifecycle_mutation_forbidden";
    case ErrorCode::OccupancyOverlap:
      return "occupancy_overlap";
    case ErrorCode::MountOutOfBounds:
      return "mount_out_of_bounds";
    case ErrorCode::InvalidMountSpan:
      return "invalid_mount_span";
    case ErrorCode::SharedMountCapacityExceeded:
      return "shared_mount_capacity_exceeded";
    case ErrorCode::SharedMountClassMismatch:
      return "shared_mount_class_mismatch";
    case ErrorCode::RackExtentWouldEvictMembers:
      return "rack_extent_would_evict_members";
    case ErrorCode::CompatibilityUnsatisfied:
      return "compatibility_unsatisfied";
    case ErrorCode::UnknownCompatibilityProfile:
      return "unknown_compatibility_profile";
    case ErrorCode::MalformedCompatibilityProfile:
      return "malformed_compatibility_profile";
    case ErrorCode::IoFailure:
      return "io_failure";
    case ErrorCode::IntegrityCheckFailed:
      return "integrity_check_failed";
    case ErrorCode::UnsupportedFormatVersion:
      return "unsupported_format_version";
    case ErrorCode::TruncatedState:
      return "truncated_state";
    case ErrorCode::CorruptState:
      return "corrupt_state";
    case ErrorCode::StateTooLarge:
      return "state_too_large";
    case ErrorCode::NoAuthoritativeState:
      return "no_authoritative_state";
    case ErrorCode::UnsupportedCoordinateModel:
      return "unsupported_coordinate_model";
    case ErrorCode::InvalidStateEncoding:
      return "invalid_state_encoding";
    case ErrorCode::LimitExceeded:
      return "limit_exceeded";
    case ErrorCode::SnapshotInvalid:
      return "snapshot_invalid";
    case ErrorCode::WriterLockHeld:
      return "writer_lock_held";
    case ErrorCode::WriterLockInvalid:
      return "writer_lock_invalid";
    case ErrorCode::ReadOnlyStore:
      return "read_only_store";
    case ErrorCode::LockIoFailure:
      return "lock_io_failure";
    case ErrorCode::WriterIdentityMismatch:
      return "writer_identity_mismatch";
  }
  return "unknown_error";
}

std::string_view explain(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "The operation completed.";
    case ErrorCode::InvalidArgument:
      return "An argument is outside the domain this operation accepts.";
    case ErrorCode::MalformedIdentity:
      return "The text is not a well formed identity of the required kind.";
    case ErrorCode::IdentityTooLong:
      return "The text is longer than the bound for its kind.";
    case ErrorCode::InvalidCharacter:
      return "The text contains a character outside the domain for its kind.";
    case ErrorCode::InvalidEnumValue:
      return "A numeric value does not name a member of its enumeration.";
    case ErrorCode::InvalidRange:
      return "A range is empty, inverted, or outside the permitted interval.";
    case ErrorCode::EmptyValue:
      return "A value that must be present is empty.";
    case ErrorCode::InvalidUtf8:
      return "The text is not valid UTF-8.";
    case ErrorCode::InvalidTimestamp:
      return "A supplied timestamp is not acceptable for this operation.";
    case ErrorCode::DuplicateRackId:
      return "A rack with this identity is already registered.";
    case ErrorCode::UnknownRackId:
      return "No rack with this identity is registered.";
    case ErrorCode::DuplicateMemberId:
      return "A member with this identity already exists in the rack.";
    case ErrorCode::UnknownMemberId:
      return "No member with this identity exists in the rack.";
    case ErrorCode::DuplicateAssetPlacement:
      return "The referenced asset is already placed elsewhere in this rack.";
    case ErrorCode::DuplicateDomainReference:
      return "The same domain reference appears more than once.";
    case ErrorCode::DuplicateTrait:
      return "The same trait appears more than once in one set.";
    case ErrorCode::RequestIdConflict:
      return "This request identity was already used with different content.";
    case ErrorCode::StaleRackGeneration:
      return "The expected rack generation is not the current one.";
    case ErrorCode::StaleMembershipGeneration:
      return "The expected membership generation is not the current one.";
    case ErrorCode::StaleLifecycleState:
      return "The expected lifecycle state is not the current one.";
    case ErrorCode::GenerationNotRetained:
      return "The requested generation is no longer retained and cannot be compared.";
    case ErrorCode::StaleWriterFenced:
      return "This writer lost durable authority before it could publish.";
    case ErrorCode::StoreEpochRegression:
      return "The store epoch would move backwards.";
    case ErrorCode::SequenceRegression:
      return "The publication sequence would move backwards.";
    case ErrorCode::SnapshotIdentityMismatch:
      return "A snapshot does not belong to the store or writer it was applied to.";
    case ErrorCode::LifecycleTransitionNotAllowed:
      return "The requested lifecycle transition is not in the transition table.";
    case ErrorCode::LifecycleMutationForbidden:
      return "The rack lifecycle state forbids this class of mutation.";
    case ErrorCode::OccupancyOverlap:
      return "The mount span overlaps an existing member that cannot share it.";
    case ErrorCode::MountOutOfBounds:
      return "The mount span lies outside the physical extent of the rack.";
    case ErrorCode::InvalidMountSpan:
      return "The mount span is not a legal mounting coordinate.";
    case ErrorCode::SharedMountCapacityExceeded:
      return "The shared mount already holds as many members as its capacity allows.";
    case ErrorCode::SharedMountClassMismatch:
      return "Co-occupants of one shared mount must declare the same class and capacity.";
    case ErrorCode::RackExtentWouldEvictMembers:
      return "The new rack extent would leave an existing member outside the rack.";
    case ErrorCode::CompatibilityUnsatisfied:
      return "Member requirements are not satisfied by the rack compatibility profile.";
    case ErrorCode::UnknownCompatibilityProfile:
      return "The referenced compatibility profile is not the rack's declared profile.";
    case ErrorCode::MalformedCompatibilityProfile:
      return "The compatibility profile is not usable.";
    case ErrorCode::IoFailure:
      return "A filesystem operation failed.";
    case ErrorCode::IntegrityCheckFailed:
      return "Stored integrity data does not match the stored payload.";
    case ErrorCode::UnsupportedFormatVersion:
      return "The stored format version is not supported by this build.";
    case ErrorCode::TruncatedState:
      return "The stored state ends before its declared structure does.";
    case ErrorCode::CorruptState:
      return "The stored state is structurally invalid.";
    case ErrorCode::StateTooLarge:
      return "The stored state declares a size above the accepted bound.";
    case ErrorCode::NoAuthoritativeState:
      return "No authoritative generation could be established.";
    case ErrorCode::UnsupportedCoordinateModel:
      return "The stored state uses a mounting coordinate model this build does not implement.";
    case ErrorCode::InvalidStateEncoding:
      return "The stored state violates a rule of its own encoding.";
    case ErrorCode::LimitExceeded:
      return "A documented resource bound would be exceeded.";
    case ErrorCode::SnapshotInvalid:
      return "The snapshot violates an invariant this library guarantees.";
    case ErrorCode::WriterLockHeld:
      return "Another live writer holds authority for this state file.";
    case ErrorCode::WriterLockInvalid:
      return "The writer lock file is unreadable or fails its integrity check.";
    case ErrorCode::ReadOnlyStore:
      return "The store was opened read-only and refuses mutation.";
    case ErrorCode::LockIoFailure:
      return "A writer lock file operation failed.";
    case ErrorCode::WriterIdentityMismatch:
      return "The recorded writer identity is not the identity that owns this store.";
  }
  return "Unrecognised error code.";
}

std::uint16_t code_value(ErrorCode code) noexcept {
  return static_cast<std::uint16_t>(code);
}

std::string describe(const RackError& error) {
  std::string out;
  out.append(code_name(error.code));
  out.push_back('(');
  out.append(std::to_string(code_value(error.code)));
  out.push_back(')');
  if (!error.message.empty()) {
    out.append(": ");
    out.append(error.message);
  }
  const ErrorDetail& detail = error.detail;
  const bool has_detail = !detail.operation.empty() || !detail.subject.empty() ||
                          !detail.related.empty() || detail.expected != 0 || detail.actual != 0 ||
                          !detail.items.empty();
  if (has_detail) {
    out.append(" [");
    bool first = true;
    const auto separate = [&out, &first]() {
      if (!first) {
        out.append(", ");
      }
      first = false;
    };
    if (!detail.operation.empty()) {
      separate();
      out.append("operation=");
      out.append(detail.operation);
    }
    if (!detail.subject.empty()) {
      separate();
      out.append("subject=");
      out.append(detail.subject);
    }
    if (!detail.related.empty()) {
      separate();
      out.append("related=");
      out.append(detail.related);
    }
    if (detail.expected != 0) {
      separate();
      out.append("expected=");
      out.append(std::to_string(detail.expected));
    }
    if (detail.actual != 0) {
      separate();
      out.append("actual=");
      out.append(std::to_string(detail.actual));
    }
    if (!detail.items.empty()) {
      separate();
      out.append("items=");
      out.append(join(detail.items, '|'));
    }
    out.push_back(']');
  }
  return out;
}

RackError make_error(ErrorCode code, std::string message, ErrorDetail detail) {
  RackError error;
  error.code = code;
  error.message = std::move(message);
  error.detail = std::move(detail);
  return error;
}

}  // namespace rackregistry
