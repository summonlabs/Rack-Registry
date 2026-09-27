// Rack Registry - version and format identity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>

#include "rack_registry/export.hpp"

namespace rackregistry {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// Version of the durable state file format understood by this build. A file
// written by a different major format version is rejected, never coerced.
inline constexpr std::uint32_t kStateFormatVersion = 1;

// Version of the canonical snapshot payload layout inside a state file. This
// is part of the file format identity: a reader refuses a payload whose
// declared layout it does not implement.
inline constexpr std::uint32_t kSnapshotLayoutVersion = 1;

// Number of mount slots that make up one rack unit in the coordinate system
// used by this format version. The value is written into every state file and
// a reader rejects a file that declares any other value, so a future format
// can change the resolution without silently reinterpreting stored spans.
inline constexpr std::uint32_t kMountSlotsPerRackUnit = 2;

[[nodiscard]] RACK_REGISTRY_API std::string_view version_string() noexcept;

}  // namespace rackregistry
