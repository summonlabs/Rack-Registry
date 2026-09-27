// Rack Registry - compatibility profiles and member requirements.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/limits.hpp"
#include "rack_registry/result.hpp"

namespace rackregistry {

// An ordered, duplicate-free set of traits. Iteration order is byte-wise
// ascending over the trait text, so a set has exactly one representation and
// therefore exactly one canonical encoding.
class TraitSet {
 public:
  TraitSet() = default;

  // Sorts the input and rejects duplicates with DuplicateTrait rather than
  // silently collapsing them.
  [[nodiscard]] static Result<TraitSet> create(std::vector<Trait> traits);
  // Comma-separated trait list. Empty input yields an empty set.
  [[nodiscard]] static Result<TraitSet> parse(std::string_view text);

  [[nodiscard]] bool empty() const noexcept { return traits_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return traits_.size(); }
  [[nodiscard]] const std::vector<Trait>& traits() const noexcept { return traits_; }
  [[nodiscard]] bool contains(const Trait& trait) const noexcept;
  [[nodiscard]] bool contains_any(const TraitSet& other) const noexcept;

  [[nodiscard]] std::vector<Trait> intersection(const TraitSet& other) const;
  [[nodiscard]] std::vector<Trait> difference(const TraitSet& other) const;

  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] bool operator==(const TraitSet& other) const noexcept {
    return traits_ == other.traits_;
  }
  [[nodiscard]] bool operator!=(const TraitSet& other) const noexcept {
    return !(*this == other);
  }

 private:
  std::vector<Trait> traits_;
};

// The compatibility profile a rack declares. `provides` is the authoritative
// local copy of the traits the rack offers; the identifier is a stable
// reference to the profile definition held by a compatibility catalog owned by
// another runtime, and Rack Registry never resolves it over the network.
struct CompatibilityProfile {
  CompatibilityProfileId id{};
  TraitSet provides{};

  [[nodiscard]] bool operator==(const CompatibilityProfile& other) const noexcept {
    return id == other.id && provides == other.provides;
  }
  [[nodiscard]] bool operator!=(const CompatibilityProfile& other) const noexcept {
    return !(*this == other);
  }
};

// What a member needs from the rack it is mounted in.
struct MemberRequirements {
  // Traits the member needs the rack to provide.
  TraitSet required{};
  // Traits the member cannot coexist with.
  TraitSet forbids{};

  [[nodiscard]] bool operator==(const MemberRequirements& other) const noexcept {
    return required == other.required && forbids == other.forbids;
  }
  [[nodiscard]] bool operator!=(const MemberRequirements& other) const noexcept {
    return !(*this == other);
  }
};

// Outcome of a compatibility evaluation. Both lists are in byte-wise ascending
// order, so a report has one canonical rendering.
struct CompatibilityReport {
  bool compatible = true;
  // Traits the member requires that the rack does not provide.
  std::vector<Trait> missing{};
  // Traits the member forbids that the rack does provide.
  std::vector<Trait> forbidden_present{};

  [[nodiscard]] std::string to_text() const;
};

// A member is compatible when every required trait is provided and no
// forbidden trait is provided. Both directions are reported.
[[nodiscard]] RACK_REGISTRY_API CompatibilityReport evaluate_compatibility(
    const CompatibilityProfile& profile, const MemberRequirements& requirements);

}  // namespace rackregistry
