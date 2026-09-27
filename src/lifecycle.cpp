// Rack Registry - rack lifecycle state machine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/lifecycle.hpp"

#include <array>
#include <string>

namespace rackregistry {
namespace {

// The complete legal transition table. A transition that does not appear here
// is refused with LifecycleTransitionNotAllowed, including a transition to the
// current state.
constexpr std::array<std::array<bool, kLifecycleStateCount>, kLifecycleStateCount>
    kTransitionTable = {{
        // to:    Defined, Commissioned, Active, Maintenance, Retired, Removed
        /*Defined*/ {false, true, false, false, true, false},
        /*Commissioned*/ {false, false, true, false, true, false},
        /*Active*/ {false, false, false, true, true, false},
        /*Maintenance*/ {false, false, true, false, true, false},
        /*Retired*/ {false, false, false, false, false, true},
        /*Removed*/ {false, false, false, false, false, false},
    }};

// Capability table: membership mutation, structural mutation, domain
// mutation, for each state in enum order.
struct Capabilities {
  bool membership;
  bool structural;
  bool domains;
};

constexpr std::array<Capabilities, kLifecycleStateCount> kCapabilities = {{
    /*Defined*/ {true, true, true},
    /*Commissioned*/ {true, true, true},
    /*Active*/ {true, true, true},
    /*Maintenance*/ {true, false, true},
    /*Retired*/ {false, false, false},
    /*Removed*/ {false, false, false},
}};

[[nodiscard]] constexpr std::size_t index_of(LifecycleState state) noexcept {
  return static_cast<std::size_t>(state);
}

}  // namespace

std::string_view lifecycle_state_name(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Defined:
      return "defined";
    case LifecycleState::Commissioned:
      return "commissioned";
    case LifecycleState::Active:
      return "active";
    case LifecycleState::Maintenance:
      return "maintenance";
    case LifecycleState::Retired:
      return "retired";
    case LifecycleState::Removed:
      return "removed";
  }
  return "unknown";
}

Result<LifecycleState> parse_lifecycle_state(std::string_view text) {
  for (std::size_t i = 0; i < kLifecycleStateCount; ++i) {
    const auto state = static_cast<LifecycleState>(i);
    if (lifecycle_state_name(state) == text) {
      return state;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown lifecycle state",
                    ErrorDetail{.operation = "LifecycleState", .subject = std::string(text)});
}

Result<LifecycleState> lifecycle_state_from_value(std::uint8_t value) {
  if (value >= kLifecycleStateCount) {
    return make_error(ErrorCode::InvalidEnumValue, "lifecycle state value is outside its domain",
                      ErrorDetail{.operation = "LifecycleState",
                                  .expected = kLifecycleStateCount - 1,
                                  .actual = value});
  }
  return static_cast<LifecycleState>(value);
}

std::vector<LifecycleState> allowed_transitions(LifecycleState state) {
  std::vector<LifecycleState> result;
  const std::size_t from = index_of(state);
  if (from >= kLifecycleStateCount) {
    return result;
  }
  for (std::size_t to = 0; to < kLifecycleStateCount; ++to) {
    if (kTransitionTable[from][to]) {
      result.push_back(static_cast<LifecycleState>(to));
    }
  }
  return result;
}

bool transition_allowed(LifecycleState from, LifecycleState to) noexcept {
  const std::size_t source = index_of(from);
  const std::size_t target = index_of(to);
  if (source >= kLifecycleStateCount || target >= kLifecycleStateCount) {
    return false;
  }
  return kTransitionTable[source][target];
}

bool permits_membership_mutation(LifecycleState state) noexcept {
  const std::size_t index = index_of(state);
  return index < kLifecycleStateCount && kCapabilities[index].membership;
}

bool permits_structural_mutation(LifecycleState state) noexcept {
  const std::size_t index = index_of(state);
  return index < kLifecycleStateCount && kCapabilities[index].structural;
}

bool permits_domain_mutation(LifecycleState state) noexcept {
  const std::size_t index = index_of(state);
  return index < kLifecycleStateCount && kCapabilities[index].domains;
}

bool is_immutable(LifecycleState state) noexcept {
  return !permits_membership_mutation(state) && !permits_structural_mutation(state) &&
         !permits_domain_mutation(state);
}

bool is_in_service(LifecycleState state) noexcept {
  return state == LifecycleState::Commissioned || state == LifecycleState::Active ||
         state == LifecycleState::Maintenance;
}

}  // namespace rackregistry
