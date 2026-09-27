// Rack Registry - the authoritative registry and its mutation surface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/compatibility.hpp"
#include "rack_registry/diff.hpp"
#include "rack_registry/export.hpp"
#include "rack_registry/member.hpp"
#include "rack_registry/rack.hpp"
#include "rack_registry/requests.hpp"
#include "rack_registry/result.hpp"

namespace rackregistry {

// Receipt for one accepted mutation. `replayed` is true when the request was
// answered from the idempotency table of the rack instead of being applied:
// the state did not change, and every counter in the receipt is the counter
// the original application produced. `state_digest` is the canonical digest of
// the rack after the operation.
struct MutationReceipt {
  OperationKind operation = OperationKind::RegisterRack;
  RackId rack_id{};
  std::optional<RackMemberId> member_id{};
  RackGeneration generation{};
  RackRevision revision{};
  MembershipGeneration membership_generation{};
  bool replayed = false;
  StateDigest state_digest{};

  [[nodiscard]] std::string to_text() const;
};

// One recorded rejection, retained for inspection. Rejections never change
// authoritative state; the journal exists so an operator can see why a
// mutation was refused and what the expected authority was.
struct RejectionRecord {
  std::uint64_t sequence = 0;
  ErrorCode code = ErrorCode::Ok;
  std::string operation{};
  std::string subject{};
  std::string message{};
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;

  [[nodiscard]] std::string to_text() const;
};

// Accounting for the registry. Every counter is exact and observed under the
// registry lock.
struct RegistryStats {
  std::size_t rack_count = 0;
  std::size_t member_count = 0;
  // Racks whose lifecycle state refuses every mutation (retired and removed).
  std::size_t immutable_rack_count = 0;
  std::size_t retained_generation_records = 0;
  std::size_t generation_evidence_records = 0;
  std::size_t idempotency_records = 0;
  std::size_t provenance_records = 0;
  std::size_t rejection_records = 0;
  std::uint64_t total_rejections = 0;
};

// The authoritative in-memory registry.
//
// Concurrency model: every public method is safe to call from any thread and
// is internally synchronized. Mutations are serialized: one mutation at a time
// is planned, validated and applied, and a reader either observes the state
// before a mutation or the state after it, never a partial application. Query
// methods take a shared lock and return self-contained values, so a caller can
// never hold a reference into mutable state. The library emits no callbacks and
// therefore cannot re-enter itself.
//
// Determinism: no method reads a wall clock, generates randomness, or depends
// on locale. The caller supplies every timestamp through provenance.
class RACK_REGISTRY_API RackRegistry {
 public:
  RackRegistry();
  ~RackRegistry();

  RackRegistry(RackRegistry&&) noexcept;
  RackRegistry& operator=(RackRegistry&&) noexcept;
  RackRegistry(const RackRegistry&) = delete;
  RackRegistry& operator=(const RackRegistry&) = delete;

  // -------------------------------------------------------------------------
  // Mutations. Each returns a receipt on acceptance and a RackError on
  // rejection. A rejection leaves the registry byte-for-byte unchanged.
  // -------------------------------------------------------------------------

  // Registers a rack with an initial structural definition. Rejected with
  // DuplicateRackId when the identity is already known. The new rack starts at
  // generation 1, revision 1, membership generation 0 and lifecycle Defined.
  [[nodiscard]] Result<MutationReceipt> register_rack(const RegisterRackRequest& request);

  // Replaces the structural definition of an existing rack. Rejected when the
  // rack lifecycle state forbids structural mutation, when the expected
  // generation is stale, when the new extent would leave an existing member
  // outside the rack, or when the new profile no longer satisfies an existing
  // member's requirements.
  [[nodiscard]] Result<MutationReceipt> set_rack_structure(const SetRackStructureRequest& request);

  // Applies one legal lifecycle transition. Rejected when the transition is
  // not in the lifecycle table or when the expected state or generation is
  // stale.
  [[nodiscard]] Result<MutationReceipt> transition_lifecycle(
      const TransitionLifecycleRequest& request);

  // Inserts a member. Rejected on duplicate member identity, duplicate asset
  // placement in the same rack, out-of-bounds mount span, occupancy overlap,
  // shared-mount capacity or class mismatch, incompatible requirements, or a
  // stale precondition.
  [[nodiscard]] Result<MutationReceipt> insert_member(const InsertMemberRequest& request);

  // Removes a member. Rejected when the member is unknown or the precondition
  // is stale.
  [[nodiscard]] Result<MutationReceipt> remove_member(const RemoveMemberRequest& request);

  // Moves a member to another mount span, atomically. The member's own current
  // span never counts as a conflict with itself. Rejected when the target span
  // is out of bounds, conflicts with another member, or the precondition is
  // stale.
  [[nodiscard]] Result<MutationReceipt> move_member(const MoveMemberRequest& request);

  // Replaces the asset referenced by a member, keeping its identity and mount
  // span, and optionally advances its membership state. Rejected when the
  // replacement asset is already placed in the rack, when it is already the
  // member's asset, when the new requirements are incompatible, or when the
  // precondition is stale.
  [[nodiscard]] Result<MutationReceipt> replace_member(const ReplaceMemberRequest& request);

  // -------------------------------------------------------------------------
  // Queries. All are const, take a shared lock and return self-contained
  // values.
  // -------------------------------------------------------------------------

  [[nodiscard]] bool contains_rack(const RackId& rack_id) const;
  [[nodiscard]] std::size_t rack_count() const;

  [[nodiscard]] Result<RackView> rack(const RackId& rack_id) const;
  // All racks in ascending rack-identity order.
  [[nodiscard]] std::vector<RackView> racks() const;

  [[nodiscard]] Result<std::vector<MemberRecord>> members(const RackId& rack_id,
                                                          MemberOrder order) const;
  [[nodiscard]] Result<std::optional<MemberRecord>> member(const RackId& rack_id,
                                                           const RackMemberId& member_id) const;
  [[nodiscard]] Result<std::vector<OccupancyRecord>> occupancy(const RackId& rack_id) const;
  // Structural free ranges: what is not occupied. Descriptive only.
  [[nodiscard]] Result<std::vector<FreeSpan>> free_spans(const RackId& rack_id) const;
  [[nodiscard]] Result<std::vector<SharedSpanAvailability>> shared_availability(
      const RackId& rack_id) const;
  [[nodiscard]] Result<CompatibilityReport> evaluate_compatibility(
      const RackId& rack_id, const MemberRequirements& requirements) const;

  // Diffs a retained generation against the current generation.
  [[nodiscard]] Result<RackDiff> diff_generations(const RackId& rack_id,
                                                  RackGeneration from) const;
  // Diffs two retained generations. Rejected with GenerationNotRetained when
  // either generation has fallen out of the retained ring.
  [[nodiscard]] Result<RackDiff> diff_generations(const RackId& rack_id,
                                                  RackGeneration from,
                                                  RackGeneration to) const;

  // -------------------------------------------------------------------------
  // Whole-registry state.
  // -------------------------------------------------------------------------

  // An immutable snapshot. `produced_by` names the tool or component that
  // produced it and is recorded verbatim in the snapshot and in its digest, so
  // two producers that name themselves differently produce different snapshot
  // digests by construction. Pass nullopt to record no producer.
  [[nodiscard]] RackSnapshot snapshot(
      const std::optional<SourceReference>& produced_by = std::nullopt) const;
  [[nodiscard]] StateDigest state_digest() const;

  // Replaces the whole registry from a snapshot, re-validating every invariant.
  // A snapshot that violates an invariant is rejected and the registry is left
  // unchanged.
  [[nodiscard]] Status restore(const RackSnapshot& snapshot);

  // -------------------------------------------------------------------------
  // Inspection.
  // -------------------------------------------------------------------------

  // Rejections in occurrence order, oldest first, bounded by
  // kMaxRejectionJournalEntries.
  [[nodiscard]] std::vector<RejectionRecord> rejections() const;
  void clear_rejections();
  [[nodiscard]] RegistryStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rackregistry
