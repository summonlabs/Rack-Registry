// Rack Registry - generation and snapshot diffing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/diff.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "rack_registry/text.hpp"

namespace rackregistry {
namespace {

template <typename Reference>
std::vector<Reference> added_of(const std::vector<Reference>& from,
                                const std::vector<Reference>& to) {
  std::vector<Reference> result;
  for (const Reference& candidate : to) {
    const bool present = std::find(from.begin(), from.end(), candidate) != from.end();
    if (!present) {
      result.push_back(candidate);
    }
  }
  return result;
}

template <typename Reference>
std::vector<Reference> removed_of(const std::vector<Reference>& from,
                                  const std::vector<Reference>& to) {
  return added_of(to, from);
}

void append_asset(std::string& out, const char* label, const std::optional<AssetId>& value) {
  out.append(" ");
  out.append(label);
  out.push_back('=');
  out.append(value.has_value() ? value->text() : std::string("<none>"));
}

void append_mount(std::string& out, const char* label, const std::optional<MountSpan>& value) {
  out.append(" ");
  out.append(label);
  out.push_back('=');
  out.append(value.has_value() ? value->to_text() : std::string("<none>"));
}

}  // namespace

std::string member_change_flag_text(MemberChangeFlag value) {
  if (value == MemberChangeFlag::None) {
    return "none";
  }
  std::vector<std::string> parts;
  if (has_flag(value, MemberChangeFlag::Added)) {
    parts.emplace_back("added");
  }
  if (has_flag(value, MemberChangeFlag::Removed)) {
    parts.emplace_back("removed");
  }
  if (has_flag(value, MemberChangeFlag::Moved)) {
    parts.emplace_back("moved");
  }
  if (has_flag(value, MemberChangeFlag::AssetReplaced)) {
    parts.emplace_back("asset_replaced");
  }
  if (has_flag(value, MemberChangeFlag::StateChanged)) {
    parts.emplace_back("state_changed");
  }
  if (has_flag(value, MemberChangeFlag::RequirementsChanged)) {
    parts.emplace_back("requirements_changed");
  }
  return join(parts, '|');
}

std::uint16_t member_change_flag_value(MemberChangeFlag value) {
  return static_cast<std::uint16_t>(value);
}

std::string MemberChange::to_text() const {
  std::string out = member_id.text();
  out.append(" ");
  out.append(member_change_flag_text(flags));
  if (has_flag(flags, MemberChangeFlag::AssetReplaced)) {
    append_asset(out, "from_asset", previous_asset);
    append_asset(out, "to_asset", current_asset);
  }
  if (has_flag(flags, MemberChangeFlag::Moved)) {
    append_mount(out, "from_mount", previous_mount);
    append_mount(out, "to_mount", current_mount);
  }
  if (has_flag(flags, MemberChangeFlag::StateChanged)) {
    out.append(" from_state=");
    out.append(previous_state.has_value() ? membership_state_name(*previous_state) : "<none>");
    out.append(" to_state=");
    out.append(current_state.has_value() ? membership_state_name(*current_state) : "<none>");
  }
  if (has_flag(flags, MemberChangeFlag::RequirementsChanged)) {
    out.append(" from_requires=");
    out.append(previous_requirements.has_value() ? previous_requirements->required.to_text()
                                                 : std::string("<none>"));
    out.append(" to_requires=");
    out.append(current_requirements.has_value() ? current_requirements->required.to_text()
                                                : std::string("<none>"));
  }
  if (has_flag(flags, MemberChangeFlag::Added) ||
      has_flag(flags, MemberChangeFlag::Removed)) {
    append_asset(out, "asset", current_asset.has_value() ? current_asset : previous_asset);
    append_mount(out, "mount", current_mount.has_value() ? current_mount : previous_mount);
  }
  return out;
}

bool RackDiff::empty() const noexcept {
  return !lifecycle_changed && !extent_changed &&
         !profile_changed && !label_changed && traits_added.empty() && traits_removed.empty() &&
         power_domains_added.empty() && power_domains_removed.empty() &&
         cooling_domains_added.empty() && cooling_domains_removed.empty() &&
         member_changes.empty();
}

std::string RackDiff::to_text() const {
  std::string out = rack_id.text();
  out.append(" generation ");
  out.append(std::to_string(from_generation.value()));
  out.append(" -> ");
  out.append(std::to_string(to_generation.value()));
  out.append(" revision ");
  out.append(std::to_string(from_revision.value()));
  out.append(" -> ");
  out.append(std::to_string(to_revision.value()));
  out.append(" membership ");
  out.append(std::to_string(from_membership_generation.value()));
  out.append(" -> ");
  out.append(std::to_string(to_membership_generation.value()));
  if (empty()) {
    out.append(" no_changes");
    return out;
  }
  if (lifecycle_changed) {
    out.append(" lifecycle=");
    out.append(lifecycle_state_name(from_lifecycle));
    out.append("->");
    out.append(lifecycle_state_name(to_lifecycle));
  }
  if (extent_changed) {
    out.append(" units=");
    out.append(std::to_string(from_unit_count));
    out.append("->");
    out.append(std::to_string(to_unit_count));
  }
  if (profile_changed) {
    out.append(" profile=");
    out.append(from_profile.text());
    out.append("->");
    out.append(to_profile.text());
  }
  for (const Trait& trait : traits_added) {
    out.append(" +trait=");
    out.append(trait.text());
  }
  for (const Trait& trait : traits_removed) {
    out.append(" -trait=");
    out.append(trait.text());
  }
  for (const PowerDomainReference& reference : power_domains_added) {
    out.append(" +power=");
    out.append(reference.text());
  }
  for (const PowerDomainReference& reference : power_domains_removed) {
    out.append(" -power=");
    out.append(reference.text());
  }
  for (const CoolingDomainReference& reference : cooling_domains_added) {
    out.append(" +cooling=");
    out.append(reference.text());
  }
  for (const CoolingDomainReference& reference : cooling_domains_removed) {
    out.append(" -cooling=");
    out.append(reference.text());
  }
  if (label_changed) {
    out.append(" label=");
    out.append(from_label.text());
    out.append("->");
    out.append(to_label.text());
  }
  for (const MemberChange& change : member_changes) {
    out.append(" member[");
    out.append(change.to_text());
    out.append("]");
  }
  return out;
}

RackDiff diff_records(const RackRecord& from, const RackRecord& to) {
  RackDiff diff;
  diff.rack_id = to.structure.id.empty() ? from.structure.id : to.structure.id;
  diff.from_generation = from.generation;
  diff.to_generation = to.generation;
  diff.from_revision = from.revision;
  diff.to_revision = to.revision;
  diff.from_membership_generation = from.membership_generation;
  diff.to_membership_generation = to.membership_generation;
  diff.from_lifecycle = from.lifecycle;
  diff.to_lifecycle = to.lifecycle;
  diff.lifecycle_changed = from.lifecycle != to.lifecycle;

  diff.from_unit_count = from.structure.unit_count;
  diff.to_unit_count = to.structure.unit_count;
  diff.extent_changed = from.structure.unit_count != to.structure.unit_count;

  diff.from_profile = from.structure.profile.id;
  diff.to_profile = to.structure.profile.id;
  diff.profile_changed = !(from.structure.profile.id == to.structure.profile.id);

  const std::vector<Trait> from_traits = from.structure.profile.provides.traits();
  const std::vector<Trait> to_traits = to.structure.profile.provides.traits();
  diff.traits_added = added_of(from_traits, to_traits);
  diff.traits_removed = removed_of(from_traits, to_traits);

  diff.power_domains_added = added_of(from.structure.power_domains, to.structure.power_domains);
  diff.power_domains_removed =
      removed_of(from.structure.power_domains, to.structure.power_domains);
  diff.cooling_domains_added =
      added_of(from.structure.cooling_domains, to.structure.cooling_domains);
  diff.cooling_domains_removed =
      removed_of(from.structure.cooling_domains, to.structure.cooling_domains);

  diff.from_label = from.structure.label;
  diff.to_label = to.structure.label;
  diff.label_changed = !(from.structure.label == to.structure.label);

  // Members are compared by identity; the merged walk yields ascending
  // member-identity order because both maps are ordered that way.
  auto left = from.members.begin();
  auto right = to.members.begin();
  while (left != from.members.end() || right != to.members.end()) {
    if (right == to.members.end() || (left != from.members.end() && left->first < right->first)) {
      MemberChange change;
      change.member_id = left->first;
      change.flags = MemberChangeFlag::Removed;
      change.previous_asset = left->second.asset_id;
      change.previous_mount = left->second.mount;
      change.previous_state = left->second.state;
      change.previous_requirements = left->second.requirements;
      diff.member_changes.push_back(std::move(change));
      ++left;
      continue;
    }
    if (left == from.members.end() || right->first < left->first) {
      MemberChange change;
      change.member_id = right->first;
      change.flags = MemberChangeFlag::Added;
      change.current_asset = right->second.asset_id;
      change.current_mount = right->second.mount;
      change.current_state = right->second.state;
      change.current_requirements = right->second.requirements;
      diff.member_changes.push_back(std::move(change));
      ++right;
      continue;
    }

    const MemberRecord& before = left->second;
    const MemberRecord& after = right->second;
    MemberChangeFlag flags = MemberChangeFlag::None;
    MemberChange change;
    change.member_id = left->first;
    if (!(before.asset_id == after.asset_id)) {
      flags = flags | MemberChangeFlag::AssetReplaced;
      change.previous_asset = before.asset_id;
      change.current_asset = after.asset_id;
    }
    if (!before.mount.equals(after.mount)) {
      flags = flags | MemberChangeFlag::Moved;
      change.previous_mount = before.mount;
      change.current_mount = after.mount;
    }
    if (before.state != after.state) {
      flags = flags | MemberChangeFlag::StateChanged;
      change.previous_state = before.state;
      change.current_state = after.state;
    }
    if (!(before.requirements == after.requirements)) {
      flags = flags | MemberChangeFlag::RequirementsChanged;
      change.previous_requirements = before.requirements;
      change.current_requirements = after.requirements;
    }
    if (flags != MemberChangeFlag::None) {
      change.flags = flags;
      diff.member_changes.push_back(std::move(change));
    }
    ++left;
    ++right;
  }
  return diff;
}

bool SnapshotDiff::identical() const noexcept {
  return from_digest == to_digest && racks_added.empty() && racks_removed.empty() &&
         racks_changed.empty();
}

std::string SnapshotDiff::to_text() const {
  std::string out = "snapshot ";
  out.append(from_digest.to_hex());
  out.append(" -> ");
  out.append(to_digest.to_hex());
  out.append(" sequence ");
  out.append(std::to_string(from_sequence.value()));
  out.append(" -> ");
  out.append(std::to_string(to_sequence.value()));
  if (identical()) {
    out.append(" identical");
    return out;
  }
  for (const RackId& rack : racks_added) {
    out.append(" +rack=");
    out.append(rack.text());
  }
  for (const RackId& rack : racks_removed) {
    out.append(" -rack=");
    out.append(rack.text());
  }
  for (const RackDiff& diff : racks_changed) {
    out.append(" [");
    out.append(diff.to_text());
    out.append("]");
  }
  return out;
}

SnapshotDiff diff_snapshots(const RackSnapshot& from, const RackSnapshot& to) {
  SnapshotDiff diff;
  diff.from_digest = from.state_digest();
  diff.to_digest = to.state_digest();
  diff.from_epoch = from.store_epoch();
  diff.to_epoch = to.store_epoch();
  diff.from_sequence = from.store_sequence();
  diff.to_sequence = to.store_sequence();

  for (const RackRecord& record : to.racks()) {
    if (from.find(record.structure.id) == nullptr) {
      diff.racks_added.push_back(record.structure.id);
    }
  }
  for (const RackRecord& record : from.racks()) {
    if (to.find(record.structure.id) == nullptr) {
      diff.racks_removed.push_back(record.structure.id);
    }
  }
  for (const RackRecord& record : to.racks()) {
    const RackRecord* previous = from.find(record.structure.id);
    if (previous == nullptr) {
      continue;
    }
    RackDiff rack_diff = diff_records(*previous, record);
    if (!rack_diff.empty()) {
      diff.racks_changed.push_back(std::move(rack_diff));
    }
  }
  return diff;
}

}  // namespace rackregistry
