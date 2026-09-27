// Rack Registry - compatibility sets, profiles and evaluation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/compatibility.hpp"

#include <algorithm>
#include <string>

#include "rack_registry/text.hpp"

namespace rackregistry {

Result<TraitSet> TraitSet::create(std::vector<Trait> traits) {
  if (traits.size() > kMaxTraitsPerSet) {
    return make_error(ErrorCode::LimitExceeded,
                      "trait set exceeds " + std::to_string(kMaxTraitsPerSet) + " entries",
                      ErrorDetail{.operation = "TraitSet",
                                  .expected = kMaxTraitsPerSet,
                                  .actual = traits.size()});
  }
  std::vector<std::string> texts;
  texts.reserve(traits.size());
  for (const Trait& trait : traits) {
    if (trait.empty()) {
      return make_error(ErrorCode::EmptyValue, "trait must not be empty",
                        ErrorDetail{.operation = "TraitSet"});
    }
    texts.push_back(trait.text());
  }
  std::sort(texts.begin(), texts.end(),
            [](const std::string& a, const std::string& b) { return byte_less(a, b); });
  for (std::size_t i = 1; i < texts.size(); ++i) {
    if (texts[i] == texts[i - 1]) {
      return make_error(ErrorCode::DuplicateTrait, "trait appears more than once: " + texts[i],
                        ErrorDetail{.operation = "TraitSet",
                                    .subject = texts[i],
                                    .items = {texts[i]}});
    }
  }

  TraitSet set;
  set.traits_.reserve(traits.size());
  for (const std::string& text : texts) {
    const auto trait = Trait::parse(text);
    if (!trait) {
      return trait.error();
    }
    set.traits_.push_back(trait.value());
  }
  return set;
}

Result<TraitSet> TraitSet::parse(std::string_view text) {
  if (text.empty()) {
    return TraitSet{};
  }
  std::vector<Trait> traits;
  for (const std::string& part : split(text, ',')) {
    const auto trait = Trait::parse(part);
    if (!trait) {
      return trait.error();
    }
    traits.push_back(trait.value());
  }
  return create(std::move(traits));
}

bool TraitSet::contains(const Trait& trait) const noexcept {
  return std::binary_search(traits_.begin(), traits_.end(), trait,
                            [](const Trait& a, const Trait& b) { return a < b; });
}

bool TraitSet::contains_any(const TraitSet& other) const noexcept {
  for (const Trait& trait : other.traits_) {
    if (contains(trait)) {
      return true;
    }
  }
  return false;
}

std::vector<Trait> TraitSet::intersection(const TraitSet& other) const {
  std::vector<Trait> result;
  for (const Trait& trait : traits_) {
    if (other.contains(trait)) {
      result.push_back(trait);
    }
  }
  return result;
}

std::vector<Trait> TraitSet::difference(const TraitSet& other) const {
  std::vector<Trait> result;
  for (const Trait& trait : traits_) {
    if (!other.contains(trait)) {
      result.push_back(trait);
    }
  }
  return result;
}

std::string TraitSet::to_text() const {
  std::vector<std::string> texts;
  texts.reserve(traits_.size());
  for (const Trait& trait : traits_) {
    texts.push_back(trait.text());
  }
  return join(texts, ',');
}

CompatibilityReport evaluate_compatibility(const CompatibilityProfile& profile,
                                           const MemberRequirements& requirements) {
  CompatibilityReport report;
  report.missing = requirements.required.difference(profile.provides);
  report.forbidden_present = requirements.forbids.intersection(profile.provides);
  report.compatible = report.missing.empty() && report.forbidden_present.empty();
  return report;
}

std::string CompatibilityReport::to_text() const {
  if (compatible) {
    return "compatible";
  }
  std::string out = "incompatible";
  if (!missing.empty()) {
    out.append(" missing=");
    std::vector<std::string> texts;
    texts.reserve(missing.size());
    for (const Trait& trait : missing) {
      texts.push_back(trait.text());
    }
    out.append(join(texts, '|'));
  }
  if (!forbidden_present.empty()) {
    out.append(" forbidden_present=");
    std::vector<std::string> texts;
    texts.reserve(forbidden_present.size());
    for (const Trait& trait : forbidden_present) {
      texts.push_back(trait.text());
    }
    out.append(join(texts, '|'));
  }
  return out;
}

}  // namespace rackregistry
