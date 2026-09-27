// Rack Registry - rack lifecycle state machine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "rack_registry/export.hpp"
#include "rack_registry/result.hpp"

namespace rackregistry {

// Lifecycle of a rack as a facility object. The state gates mutation; it never
// expresses electrical or cooling control, and it is not an operational health
// signal (that belongs to observability runtimes).
enum class LifecycleState : std::uint8_t {
  // The rack is known to the registry but has not been accepted into service.
  Defined = 0,
  // The rack has been accepted into service and is being prepared.
  Commissioned = 1,
  // The rack is in service.
  Active = 2,
  // The rack is temporarily withdrawn from service; members may still be
  // inserted, moved, replaced and removed, but the physical structure of the
  // rack may not be redefined.
  Maintenance = 3,
  // The rack is permanently withdrawn. It is retained as authoritative history
  // and accepts no further mutation of any kind.
  Retired = 4,
  // The rack has been physically removed from the facility. Terminal.
  Removed = 5,
};

inline constexpr std::size_t kLifecycleStateCount = 6;

[[nodiscard]] RACK_REGISTRY_API std::string_view lifecycle_state_name(LifecycleState state) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<LifecycleState> parse_lifecycle_state(std::string_view text);
[[nodiscard]] RACK_REGISTRY_API Result<LifecycleState> lifecycle_state_from_value(std::uint8_t value);

// Legal successor states, in ascending enum order. The empty result means the
// state is terminal.
[[nodiscard]] RACK_REGISTRY_API std::vector<LifecycleState> allowed_transitions(
    LifecycleState state);

[[nodiscard]] RACK_REGISTRY_API bool transition_allowed(LifecycleState from,
                                                        LifecycleState to) noexcept;

// True when the state permits inserting, moving, replacing and removing
// members.
[[nodiscard]] RACK_REGISTRY_API bool permits_membership_mutation(LifecycleState state) noexcept;

// True when the state permits redefining the physical extent, the
// compatibility profile or the display label of the rack.
[[nodiscard]] RACK_REGISTRY_API bool permits_structural_mutation(LifecycleState state) noexcept;

// True when the state permits changing power or cooling domain associations.
[[nodiscard]] RACK_REGISTRY_API bool permits_domain_mutation(LifecycleState state) noexcept;

// True for states in which mutation of any kind is refused.
[[nodiscard]] RACK_REGISTRY_API bool is_immutable(LifecycleState state) noexcept;

// True for Commissioned, Active and Maintenance.
[[nodiscard]] RACK_REGISTRY_API bool is_in_service(LifecycleState state) noexcept;

}  // namespace rackregistry
