// Rack Registry - stable machine-readable error taxonomy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/export.hpp"

namespace rackregistry {

// Error codes are a stable, machine-readable contract. Numeric values are
// grouped by fault domain and must never be reused for a different meaning.
// New codes are appended inside the matching group.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // 1xx - input rejected at a construction or validation boundary.
  InvalidArgument = 100,
  MalformedIdentity = 101,
  IdentityTooLong = 102,
  InvalidCharacter = 103,
  InvalidEnumValue = 104,
  InvalidRange = 105,
  EmptyValue = 106,
  InvalidUtf8 = 107,
  InvalidTimestamp = 108,

  // 2xx - identity uniqueness and existence.
  DuplicateRackId = 200,
  UnknownRackId = 201,
  DuplicateMemberId = 202,
  UnknownMemberId = 203,
  DuplicateAssetPlacement = 204,
  DuplicateDomainReference = 205,
  DuplicateTrait = 206,
  RequestIdConflict = 207,

  // 3xx - stale authority, generations and epochs.
  StaleRackGeneration = 300,
  StaleMembershipGeneration = 301,
  StaleLifecycleState = 302,
  GenerationNotRetained = 303,
  StaleWriterFenced = 304,
  StoreEpochRegression = 305,
  SequenceRegression = 306,
  SnapshotIdentityMismatch = 307,

  // 4xx - lifecycle gates.
  LifecycleTransitionNotAllowed = 400,
  LifecycleMutationForbidden = 401,

  // 5xx - occupancy, mounting coordinates and overlap.
  OccupancyOverlap = 500,
  MountOutOfBounds = 501,
  InvalidMountSpan = 502,
  SharedMountCapacityExceeded = 503,
  SharedMountClassMismatch = 504,
  RackExtentWouldEvictMembers = 505,

  // 6xx - compatibility.
  CompatibilityUnsatisfied = 600,
  UnknownCompatibilityProfile = 601,
  MalformedCompatibilityProfile = 602,

  // 7xx - persistence, encoding and integrity.
  IoFailure = 700,
  IntegrityCheckFailed = 701,
  UnsupportedFormatVersion = 702,
  TruncatedState = 703,
  CorruptState = 704,
  StateTooLarge = 705,
  NoAuthoritativeState = 706,
  UnsupportedCoordinateModel = 707,
  InvalidStateEncoding = 708,

  // 8xx - bounds and resource closure.
  LimitExceeded = 800,
  SnapshotInvalid = 801,

  // 9xx - writer ownership and fencing.
  WriterLockHeld = 900,
  WriterLockInvalid = 901,
  ReadOnlyStore = 902,
  LockIoFailure = 903,
  WriterIdentityMismatch = 904,
};

// Broad category of a code, so callers can branch on fault class without
// enumerating every code.
enum class ErrorCategory : std::uint8_t {
  None = 0,
  Input = 1,
  Identity = 2,
  Authority = 3,
  Lifecycle = 4,
  Occupancy = 5,
  Compatibility = 6,
  Persistence = 7,
  Limits = 8,
  Writer = 9,
};

// Machine-readable structured context for a rejection. Fields that do not
// apply are empty. `items` carries the ordered, deduplicated list relevant to
// the rejection (for example the missing compatibility traits).
struct ErrorDetail {
  std::string operation;
  std::string subject;
  std::string related;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
  std::vector<std::string> items;
};

struct RackError {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  ErrorDetail detail;
};

[[nodiscard]] RACK_REGISTRY_API ErrorCategory category_of(ErrorCode code) noexcept;

// Stable identifier for a code, for example "occupancy_overlap".
[[nodiscard]] RACK_REGISTRY_API std::string_view code_name(ErrorCode code) noexcept;

// One sentence explaining what the code means, independent of any particular
// occurrence.
[[nodiscard]] RACK_REGISTRY_API std::string_view explain(ErrorCode code) noexcept;

[[nodiscard]] RACK_REGISTRY_API std::uint16_t code_value(ErrorCode code) noexcept;

// Renders "code_name(code_value): message" plus any structured context.
[[nodiscard]] RACK_REGISTRY_API std::string describe(const RackError& error);

[[nodiscard]] RACK_REGISTRY_API RackError make_error(ErrorCode code,
                                                     std::string message,
                                                     ErrorDetail detail = {});

}  // namespace rackregistry
