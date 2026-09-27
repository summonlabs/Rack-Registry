// Rack Registry - authoritative rack records and immutable snapshots.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rack_registry/compatibility.hpp"
#include "rack_registry/digest.hpp"
#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/lifecycle.hpp"
#include "rack_registry/member.hpp"
#include "rack_registry/provenance.hpp"

namespace rackregistry {

// The structural definition of a rack: everything that is not membership,
// lifecycle or history. Domain associations are typed references into runtimes
// owned elsewhere; Rack Registry never resolves them and never models their
// internal semantics.
struct RackStructure {
  RackId id{};
  std::uint32_t unit_count = 0;
  CompatibilityProfile profile{};
  // Canonically ordered (byte-wise ascending), duplicate-free.
  std::vector<PowerDomainReference> power_domains{};
  std::vector<CoolingDomainReference> cooling_domains{};
  DisplayLabel label{};

  // The half-open slot interval [1, unit_count * kMountSlotsPerRackUnit + 1).
  // Only valid when unit_count has been validated; create_rack_structure
  // enforces that.
  [[nodiscard]] SlotRange slot_extent() const noexcept;

  [[nodiscard]] bool operator==(const RackStructure& other) const noexcept;
  [[nodiscard]] bool operator!=(const RackStructure& other) const noexcept {
    return !(*this == other);
  }
};

// Validates a structural definition in isolation: identity, unit count,
// profile identity, domain references and label. Duplicate domain references
// are rejected rather than collapsed.
[[nodiscard]] RACK_REGISTRY_API Status validate_structure(const RackStructure& structure);

// Evidence retained for one past generation of a rack. It records the counters
// and the canonical digest of that generation so that a reader can verify the
// generation chain and detect a state file whose chain has been rewritten.
struct GenerationEvidence {
  RackGeneration generation{};
  RackRevision revision{};
  MembershipGeneration membership_generation{};
  LifecycleState lifecycle = LifecycleState::Defined;
  std::uint32_t member_count = 0;
  StateDigest state_digest{};

  [[nodiscard]] bool operator==(const GenerationEvidence& other) const noexcept {
    return generation == other.generation && revision == other.revision &&
           membership_generation == other.membership_generation &&
           lifecycle == other.lifecycle && member_count == other.member_count &&
           state_digest == other.state_digest;
  }
};

// One recorded idempotency receipt. A retried request whose identity and
// content digest match the recorded one is answered from this table instead of
// being applied a second time.
struct IdempotencyRecord {
  RequestId request_id{};
  StateDigest content_digest{};
  std::uint8_t operation = 0;
  RackMemberId member_id{};
  RackGeneration generation{};
  RackRevision revision{};
  MembershipGeneration membership_generation{};

  [[nodiscard]] bool operator==(const IdempotencyRecord& other) const noexcept {
    return request_id == other.request_id && content_digest == other.content_digest &&
           operation == other.operation && member_id == other.member_id &&
           generation == other.generation && revision == other.revision &&
           membership_generation == other.membership_generation;
  }
};

// The authoritative record of one rack. Membership is keyed by member identity,
// which gives a deterministic iteration order for free; mount-ordered
// enumeration is computed when requested.
struct RackRecord {
  RackStructure structure{};
  LifecycleState lifecycle = LifecycleState::Defined;
  RackGeneration generation = RackGeneration::initial();
  RackRevision revision = RackRevision::initial();
  MembershipGeneration membership_generation = MembershipGeneration::initial();

  // Members keyed by identity, so at most one authoritative placement exists
  // per governed member.
  std::map<RackMemberId, MemberRecord> members{};

  // Bounded provenance trail of the rack itself, oldest first.
  std::vector<ProvenanceRecord> provenance{};
  std::uint64_t provenance_dropped = 0;

  // Bounded generation evidence ring, oldest first. The last entry always
  // describes the current generation.
  std::vector<GenerationEvidence> generation_evidence{};

  // Bounded idempotency receipts, oldest first, with the eviction count kept
  // so that bounded replay coverage is visible.
  std::vector<IdempotencyRecord> idempotency{};
  std::uint64_t idempotency_dropped = 0;

  [[nodiscard]] std::size_t member_count() const noexcept { return members.size(); }
  [[nodiscard]] bool has_member(const RackMemberId& member_id) const noexcept {
    return members.find(member_id) != members.end();
  }

  // Finds the member referencing `asset_id`, if any. At most one can exist.
  [[nodiscard]] std::optional<RackMemberId> member_of_asset(const AssetId& asset_id) const;

  [[nodiscard]] bool operator==(const RackRecord& other) const noexcept;
  [[nodiscard]] bool operator!=(const RackRecord& other) const noexcept {
    return !(*this == other);
  }
};

// A rack record that a caller may read but not mutate. Views are returned by
// value so that a consumer never observes a partially applied mutation.
class RackView {
 public:
  RackView() = default;
  explicit RackView(RackRecord record) : record_(std::move(record)) {}

  [[nodiscard]] const RackStructure& structure() const noexcept { return record_.structure; }
  [[nodiscard]] const RackId& id() const noexcept { return record_.structure.id; }
  [[nodiscard]] std::uint32_t unit_count() const noexcept { return record_.structure.unit_count; }
  [[nodiscard]] const CompatibilityProfile& profile() const noexcept {
    return record_.structure.profile;
  }
  [[nodiscard]] const std::vector<PowerDomainReference>& power_domains() const noexcept {
    return record_.structure.power_domains;
  }
  [[nodiscard]] const std::vector<CoolingDomainReference>& cooling_domains() const noexcept {
    return record_.structure.cooling_domains;
  }
  [[nodiscard]] const DisplayLabel& label() const noexcept { return record_.structure.label; }

  [[nodiscard]] LifecycleState lifecycle() const noexcept { return record_.lifecycle; }
  [[nodiscard]] RackGeneration generation() const noexcept { return record_.generation; }
  [[nodiscard]] RackRevision revision() const noexcept { return record_.revision; }
  [[nodiscard]] MembershipGeneration membership_generation() const noexcept {
    return record_.membership_generation;
  }
  [[nodiscard]] std::size_t member_count() const noexcept { return record_.members.size(); }
  [[nodiscard]] const std::vector<ProvenanceRecord>& provenance() const noexcept {
    return record_.provenance;
  }
  [[nodiscard]] std::uint64_t provenance_dropped() const noexcept {
    return record_.provenance_dropped;
  }
  [[nodiscard]] const std::vector<GenerationEvidence>& generation_evidence() const noexcept {
    return record_.generation_evidence;
  }
  [[nodiscard]] std::uint64_t idempotency_dropped() const noexcept {
    return record_.idempotency_dropped;
  }

  // Members in the requested deterministic order.
  [[nodiscard]] std::vector<MemberRecord> members(MemberOrder order) const;
  [[nodiscard]] std::optional<MemberRecord> member(const RackMemberId& member_id) const;

  // Rack units touched by non-zero-U members, ascending and disjoint.
  [[nodiscard]] std::vector<FreeSpan> free_spans() const;
  [[nodiscard]] std::vector<OccupancyRecord> occupancy() const;
  [[nodiscard]] std::vector<SharedSpanAvailability> shared_availability() const;

  // Canonical digest of this rack's authoritative state.
  [[nodiscard]] StateDigest state_digest() const;

  // The underlying record. Exposed for snapshotting and persistence; the
  // returned reference is valid for the lifetime of the view.
  [[nodiscard]] const RackRecord& record() const noexcept { return record_; }

 private:
  RackRecord record_{};
};

// An immutable, self-contained snapshot of every rack a registry holds.
// Snapshots are the serialization unit and the unit of comparison for
// generation diffing across a publication boundary.
class RackSnapshot {
 public:
  RackSnapshot() = default;

  // Racks in ascending rack-identity order.
  [[nodiscard]] const std::vector<RackRecord>& racks() const noexcept { return racks_; }
  [[nodiscard]] std::size_t rack_count() const noexcept { return racks_.size(); }
  [[nodiscard]] std::size_t member_count() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return racks_.empty(); }

  [[nodiscard]] const RackRecord* find(const RackId& rack_id) const noexcept;

  [[nodiscard]] StoreEpoch store_epoch() const noexcept { return store_epoch_; }
  [[nodiscard]] StoreSequence store_sequence() const noexcept { return store_sequence_; }
  // Identity of the tool or component that produced the snapshot, when one was
  // supplied. It is part of the snapshot digest.
  [[nodiscard]] const std::optional<SourceReference>& produced_by() const noexcept {
    return produced_by_;
  }

  // The canonical digest of the whole snapshot: it covers every rack record,
  // the ordering, the store epoch and the publication sequence.
  [[nodiscard]] StateDigest state_digest() const;

  // Validates every invariant this library guarantees, including ones that a
  // hand-built or decoded snapshot could violate. Returns the first violation
  // in canonical order.
  [[nodiscard]] Status validate() const;

  // Canonical byte encoding. Two snapshots that are equal encode to identical
  // bytes.
  [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;

  // A copy of this snapshot stamped with a store position. Durable publication
  // uses it to record which writer epoch and publication sequence the
  // generation belongs to. It changes no rack state.
  [[nodiscard]] RackSnapshot with_store_position(StoreEpoch epoch,
                                                 StoreSequence sequence) const;

  // Interprets untrusted bytes. Every declared length, count, enum domain and
  // identity is validated before any large allocation, and the result is
  // re-validated against the registry invariants before it is returned.
  [[nodiscard]] static Result<RackSnapshot> from_bytes(const std::uint8_t* data, std::size_t size);

 private:
  friend class RackRegistry;

  std::vector<RackRecord> racks_{};
  StoreEpoch store_epoch_ = StoreEpoch::initial();
  StoreSequence store_sequence_ = StoreSequence::initial();
  std::optional<SourceReference> produced_by_{};
};

// Ordering and equality for checks and tests. Rack equality is structural over
// the fields that carry meaning, not over incidental padding.
[[nodiscard]] RACK_REGISTRY_API bool rack_records_equal(const RackRecord& left,
                                                        const RackRecord& right) noexcept;
[[nodiscard]] RACK_REGISTRY_API bool member_records_equal(const MemberRecord& left,
                                                          const MemberRecord& right) noexcept;
[[nodiscard]] RACK_REGISTRY_API bool structures_equal(const RackStructure& left,
                                                      const RackStructure& right) noexcept;

}  // namespace rackregistry
