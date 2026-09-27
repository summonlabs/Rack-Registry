// Rack Registry - authoritative registry and mutation engine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/registry.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "internal.hpp"
#include "rack_registry/limits.hpp"
#include "rack_registry/text.hpp"

namespace rackregistry {
namespace {

constexpr std::string_view kRequestDigestDomain = "rack-registry.request.v1";

// One rack plus the bounded ring of its previous generations, which is what
// generation diffing reads.
struct RackEntry {
  RackRecord record{};
  std::vector<RackRecord> history{};
};

struct ReplayLookup {
  bool found = false;
  bool conflict = false;
  MutationReceipt receipt{};
};

std::string rack_id_text(const RackId& rack_id) {
  return rack_id.empty() ? std::string("<none>") : rack_id.text();
}

void append_provenance(std::vector<ProvenanceRecord>& trail, std::uint64_t& dropped,
                       const ProvenanceRecord& provenance, std::size_t bound) {
  trail.push_back(provenance);
  while (trail.size() > bound) {
    trail.erase(trail.begin());
    ++dropped;
  }
}

void append_evidence(RackRecord& record) {
  GenerationEvidence evidence;
  evidence.generation = record.generation;
  evidence.revision = record.revision;
  evidence.membership_generation = record.membership_generation;
  evidence.lifecycle = record.lifecycle;
  evidence.member_count = static_cast<std::uint32_t>(record.members.size());
  evidence.state_digest = internal::rack_record_digest(record);
  record.generation_evidence.push_back(evidence);
  while (record.generation_evidence.size() > kMaxRetainedGenerationsPerRack) {
    record.generation_evidence.erase(record.generation_evidence.begin());
  }
}

Result<MutationReceipt> make_receipt(const RackRecord& record, OperationKind operation,
                                     const std::optional<RackMemberId>& member_id, bool replayed) {
  MutationReceipt receipt;
  receipt.operation = operation;
  receipt.rack_id = record.structure.id;
  receipt.member_id = member_id;
  receipt.generation = record.generation;
  receipt.revision = record.revision;
  receipt.membership_generation = record.membership_generation;
  receipt.replayed = replayed;
  receipt.state_digest = internal::rack_record_digest(record);
  return receipt;
}

// Content digest of a request's *intent*. Preconditions are deliberately
// excluded: a caller that retries the same intent with a refreshed expected
// generation must still be recognised as a retry rather than as a conflicting
// reuse of the request identity.
StateDigest request_content_digest(OperationKind operation, const RackId& rack_id,
                                   const internal::ByteWriter& body) {
  internal::ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(operation));
  writer.text(rack_id.text());
  const std::vector<std::uint8_t>& payload = body.buffer();
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.raw(payload.data(), payload.size());
  return StateDigest::domain(kRequestDigestDomain, std::move(writer).take());
}

ReplayLookup lookup_replay(const RackRecord& record, const std::optional<RequestId>& request_id,
                           const StateDigest& content) {
  ReplayLookup lookup;
  if (!request_id.has_value()) {
    return lookup;
  }
  for (const IdempotencyRecord& entry : record.idempotency) {
    if (!(entry.request_id == *request_id)) {
      continue;
    }
    lookup.found = true;
    if (!(entry.content_digest == content)) {
      lookup.conflict = true;
      return lookup;
    }
    lookup.receipt.operation = static_cast<OperationKind>(entry.operation);
    lookup.receipt.rack_id = record.structure.id;
    if (!entry.member_id.empty()) {
      lookup.receipt.member_id = entry.member_id;
    }
    lookup.receipt.generation = entry.generation;
    lookup.receipt.revision = entry.revision;
    lookup.receipt.membership_generation = entry.membership_generation;
    lookup.receipt.replayed = true;
    lookup.receipt.state_digest = internal::rack_record_digest(record);
    return lookup;
  }
  return lookup;
}

void remember_request(RackRecord& record, const std::optional<RequestId>& request_id,
                      const StateDigest& content, OperationKind operation,
                      const MutationReceipt& receipt) {
  if (!request_id.has_value()) {
    return;
  }
  IdempotencyRecord entry;
  entry.request_id = *request_id;
  entry.content_digest = content;
  entry.operation = static_cast<std::uint8_t>(operation);
  if (receipt.member_id.has_value()) {
    entry.member_id = *receipt.member_id;
  }
  entry.generation = receipt.generation;
  entry.revision = receipt.revision;
  entry.membership_generation = receipt.membership_generation;
  record.idempotency.push_back(std::move(entry));
  while (record.idempotency.size() > kMaxIdempotencyRecordsPerRack) {
    record.idempotency.erase(record.idempotency.begin());
    ++record.idempotency_dropped;
  }
}

void push_history(RackEntry& entry) {
  entry.history.push_back(entry.record);
  while (entry.history.size() > kMaxRetainedGenerationsPerRack) {
    entry.history.erase(entry.history.begin());
  }
}

const RackRecord* find_retained(const RackEntry& entry, RackGeneration generation) {
  if (entry.record.generation == generation) {
    return &entry.record;
  }
  for (const RackRecord& record : entry.history) {
    if (record.generation == generation) {
      return &record;
    }
  }
  return nullptr;
}

void record_rejection(std::vector<RejectionRecord>& journal, std::uint64_t& sequence,
                      std::uint64_t& total, const RackError& error,
                      const std::string& operation) {
  ++total;
  RejectionRecord record;
  record.sequence = ++sequence;
  record.code = error.code;
  record.operation = operation;
  record.subject = error.detail.subject;
  record.message = error.message;
  record.expected = error.detail.expected;
  record.actual = error.detail.actual;
  journal.push_back(std::move(record));
  while (journal.size() > kMaxRejectionJournalEntries) {
    journal.erase(journal.begin());
  }
}

Result<const RackEntry*> find_entry(const std::map<RackId, RackEntry>& racks,
                                    const RackId& rack_id, std::string_view operation) {
  const auto found = racks.find(rack_id);
  if (found == racks.end()) {
    return make_error(ErrorCode::UnknownRackId,
                      "rack " + rack_id_text(rack_id) + " is not registered",
                      ErrorDetail{.operation = std::string(operation),
                                  .subject = rack_id_text(rack_id)});
  }
  return &found->second;
}

}  // namespace

struct RackRegistry::Impl {
  mutable std::shared_mutex mutex{};
  std::map<RackId, RackEntry> racks{};
  std::vector<RejectionRecord> rejections{};
  std::uint64_t rejection_sequence = 0;
  std::uint64_t total_rejections = 0;
};

RackRegistry::RackRegistry() : impl_(std::make_unique<Impl>()) {}
RackRegistry::~RackRegistry() = default;

// A moved-from registry is left holding a fresh empty implementation rather
// than a null pointer, so a moved-from object stays usable instead of turning
// every later call into undefined behaviour.
RackRegistry::RackRegistry(RackRegistry&& other) noexcept : impl_(std::move(other.impl_)) {
  if (impl_ == nullptr) {
    impl_ = std::make_unique<Impl>();
  }
  other.impl_ = std::make_unique<Impl>();
}

RackRegistry& RackRegistry::operator=(RackRegistry&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    if (impl_ == nullptr) {
      impl_ = std::make_unique<Impl>();
    }
    other.impl_ = std::make_unique<Impl>();
  }
  return *this;
}

// ---------------------------------------------------------------------------
// Receipt and rejection rendering
// ---------------------------------------------------------------------------

std::string MutationReceipt::to_text() const {
  std::string out(operation_kind_name(operation));
  out.append(" rack=");
  out.append(rack_id_text(rack_id));
  if (member_id.has_value()) {
    out.append(" member=");
    out.append(member_id->text());
  }
  out.append(" generation=");
  out.append(std::to_string(generation.value()));
  out.append(" revision=");
  out.append(std::to_string(revision.value()));
  out.append(" membership_generation=");
  out.append(std::to_string(membership_generation.value()));
  if (replayed) {
    out.append(" replayed");
  }
  out.append(" digest=");
  out.append(state_digest.to_hex());
  return out;
}

std::string RejectionRecord::to_text() const {
  std::string out = std::to_string(sequence);
  out.append(" ");
  out.append(code_name(code));
  out.append(" ");
  out.append(operation);
  if (!subject.empty()) {
    out.append(" subject=");
    out.append(subject);
  }
  out.append(": ");
  out.append(message);
  return out;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

Result<MutationReceipt> RackRegistry::register_rack(const RegisterRackRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "register_rack");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  if (request.structure.id.empty()) {
    return reject(make_error(ErrorCode::EmptyValue, "a rack must be registered with an identity",
                             ErrorDetail{.operation = "register_rack"}));
  }

  RackRecord shell;
  shell.structure = request.structure;
  shell.generation = RackGeneration::initial();
  shell.revision = RackRevision::initial();
  shell.membership_generation = MembershipGeneration::initial();
  shell.lifecycle = LifecycleState::Defined;
  internal::ByteWriter body;
  internal::write_rack_state(body, shell);
  const StateDigest content =
      request_content_digest(OperationKind::RegisterRack, request.structure.id, body);

  const auto existing = impl_->racks.find(request.structure.id);
  if (existing != impl_->racks.end()) {
    const ReplayLookup replay =
        lookup_replay(existing->second.record, request.identity.request_id, content);
    if (replay.conflict) {
      return reject(make_error(ErrorCode::RequestIdConflict,
                               "request identity " + request.identity.request_id->text() +
                                   " was already used with different content",
                               ErrorDetail{.operation = "register_rack",
                                           .subject = request.structure.id.text(),
                                           .related = request.identity.request_id->text()}));
    }
    if (replay.found) {
      return replay.receipt;
    }
    return reject(make_error(ErrorCode::DuplicateRackId,
                             "rack " + request.structure.id.text() + " is already registered",
                             ErrorDetail{.operation = "register_rack",
                                         .subject = request.structure.id.text()}));
  }

  if (impl_->racks.size() >= kMaxRacks) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "registry already holds the maximum number of racks",
                             ErrorDetail{.operation = "register_rack",
                                         .expected = kMaxRacks,
                                         .actual = impl_->racks.size()}));
  }

  const Status structure = validate_structure(request.structure);
  if (!structure) {
    return reject(make_error(structure.error().code, structure.error().message,
                             ErrorDetail{.operation = "register_rack",
                                         .subject = request.structure.id.text()}));
  }

  RackEntry entry;
  entry.record.structure = request.structure;
  entry.record.lifecycle = LifecycleState::Defined;
  entry.record.generation = RackGeneration::initial();
  entry.record.revision = RackRevision::initial();
  entry.record.membership_generation = MembershipGeneration::initial();
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt = make_receipt(entry.record, OperationKind::RegisterRack, std::nullopt, false);
  remember_request(entry.record, request.identity.request_id, content, OperationKind::RegisterRack,
                   receipt.value());
  impl_->racks.emplace(request.structure.id, std::move(entry));
  return receipt;
}

Result<MutationReceipt> RackRegistry::set_rack_structure(const SetRackStructureRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "set_rack_structure");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  const auto found = impl_->racks.find(request.rack_id);
  if (found == impl_->racks.end()) {
    return reject(make_error(ErrorCode::UnknownRackId,
                             "rack " + rack_id_text(request.rack_id) + " is not registered",
                             ErrorDetail{.operation = "set_rack_structure",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  RackRecord candidate_record;
  candidate_record.structure.id = request.rack_id;
  candidate_record.structure.unit_count = request.unit_count;
  candidate_record.structure.profile = request.profile;
  candidate_record.structure.power_domains = request.power_domains;
  candidate_record.structure.cooling_domains = request.cooling_domains;
  candidate_record.structure.label = request.label;
  candidate_record.generation = RackGeneration::initial();
  candidate_record.revision = RackRevision::initial();
  internal::ByteWriter body;
  internal::write_rack_state(body, candidate_record);
  const StateDigest content =
      request_content_digest(OperationKind::SetRackStructure, request.rack_id, body);

  const ReplayLookup replay =
      lookup_replay(found->second.record, request.identity.request_id, content);
  if (replay.conflict) {
    return reject(make_error(ErrorCode::RequestIdConflict,
                             "request identity " + request.identity.request_id->text() +
                                 " was already used with different content",
                             ErrorDetail{.operation = "set_rack_structure",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (replay.found) {
    return replay.receipt;
  }
  if (!(found->second.record.generation == request.precondition.expected_generation)) {
    return reject(make_error(
        ErrorCode::StaleRackGeneration,
        "expected rack generation " +
            std::to_string(request.precondition.expected_generation.value()) + " but rack is at " +
            std::to_string(found->second.record.generation.value()),
        ErrorDetail{.operation = "set_rack_structure",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_generation.value(),
                    .actual = found->second.record.generation.value()}));
  }
  if (!permits_structural_mutation(found->second.record.lifecycle)) {
    return reject(make_error(ErrorCode::LifecycleMutationForbidden,
                             "rack lifecycle state " +
                                 std::string(lifecycle_state_name(found->second.record.lifecycle)) +
                                 " forbids redefining the rack structure",
                             ErrorDetail{.operation = "set_rack_structure",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = std::string(lifecycle_state_name(
                                             found->second.record.lifecycle))}));
  }

  RackStructure candidate = found->second.record.structure;
  candidate.unit_count = request.unit_count;
  candidate.profile = request.profile;
  candidate.power_domains = request.power_domains;
  candidate.cooling_domains = request.cooling_domains;
  candidate.label = request.label;
  const Status structure = validate_structure(candidate);
  if (!structure) {
    return reject(make_error(structure.error().code, structure.error().message,
                             ErrorDetail{.operation = "set_rack_structure",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  // Every existing member must still fit inside the new extent and must still
  // be compatible with the new profile. The check revalidates the complete
  // membership under the candidate structure, so a structural change can never
  // silently evict or invalidate a member.
  const auto new_extent = rack_slot_extent(candidate.unit_count);
  if (!new_extent) {
    return reject(make_error(new_extent.error().code, new_extent.error().message,
                             ErrorDetail{.operation = "set_rack_structure",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  for (const auto& [member_id, member] : found->second.record.members) {
    if (!member.mount.is_zero_u() && !new_extent.value().contains(member.mount.span())) {
      return reject(make_error(ErrorCode::RackExtentWouldEvictMembers,
                               "reducing the rack to " + std::to_string(candidate.unit_count) +
                                   " units would leave member " + member_id.text() + " at " +
                                   member.mount.to_text() + " outside the rack",
                               ErrorDetail{.operation = "set_rack_structure",
                                           .subject = rack_id_text(request.rack_id),
                                           .related = member_id.text(),
                                           .expected = new_extent.value().end(),
                                           .actual = member.mount.span().end()}));
    }
    const CompatibilityReport report =
        rackregistry::evaluate_compatibility(candidate.profile, member.requirements);
    if (!report.compatible) {
      return reject(make_error(ErrorCode::CompatibilityUnsatisfied,
                               "profile " + candidate.profile.id.text() +
                                   " no longer satisfies member " + member_id.text() + ": " +
                                   report.to_text(),
                               ErrorDetail{.operation = "set_rack_structure",
                                           .subject = rack_id_text(request.rack_id),
                                           .related = member_id.text()}));
    }
  }
  if (found->second.record.generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack generation counter is exhausted and cannot be advanced",
                             ErrorDetail{.operation = "set_rack_structure",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  RackEntry& entry = found->second;
  push_history(entry);
  entry.record.structure = std::move(candidate);
  entry.record.generation = entry.record.generation.next();
  entry.record.revision = entry.record.revision.next();
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt =
      make_receipt(entry.record, OperationKind::SetRackStructure, std::nullopt, false);
  remember_request(entry.record, request.identity.request_id, content,
                   OperationKind::SetRackStructure, receipt.value());
  return receipt;
}

Result<MutationReceipt> RackRegistry::transition_lifecycle(
    const TransitionLifecycleRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "transition_lifecycle");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  const auto found = impl_->racks.find(request.rack_id);
  if (found == impl_->racks.end()) {
    return reject(make_error(ErrorCode::UnknownRackId,
                             "rack " + rack_id_text(request.rack_id) + " is not registered",
                             ErrorDetail{.operation = "transition_lifecycle",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  internal::ByteWriter body;
  body.u8(static_cast<std::uint8_t>(request.target));
  const StateDigest content =
      request_content_digest(OperationKind::TransitionLifecycle, request.rack_id, body);

  const ReplayLookup replay =
      lookup_replay(found->second.record, request.identity.request_id, content);
  if (replay.conflict) {
    return reject(make_error(ErrorCode::RequestIdConflict,
                             "request identity " + request.identity.request_id->text() +
                                 " was already used with different content",
                             ErrorDetail{.operation = "transition_lifecycle",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (replay.found) {
    return replay.receipt;
  }
  if (!(found->second.record.generation == request.precondition.expected_generation)) {
    return reject(make_error(
        ErrorCode::StaleRackGeneration,
        "expected rack generation " +
            std::to_string(request.precondition.expected_generation.value()) + " but rack is at " +
            std::to_string(found->second.record.generation.value()),
        ErrorDetail{.operation = "transition_lifecycle",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_generation.value(),
                    .actual = found->second.record.generation.value()}));
  }
  if (found->second.record.lifecycle != request.precondition.expected_state) {
    return reject(make_error(
        ErrorCode::StaleLifecycleState,
        "expected lifecycle state " +
            std::string(lifecycle_state_name(request.precondition.expected_state)) +
            " but rack is " + std::string(lifecycle_state_name(found->second.record.lifecycle)),
        ErrorDetail{.operation = "transition_lifecycle",
                    .subject = rack_id_text(request.rack_id),
                    .expected = static_cast<std::uint64_t>(request.precondition.expected_state),
                    .actual = static_cast<std::uint64_t>(found->second.record.lifecycle)}));
  }
  if (!transition_allowed(found->second.record.lifecycle, request.target)) {
    std::vector<std::string> allowed;
    for (const LifecycleState state : allowed_transitions(found->second.record.lifecycle)) {
      allowed.emplace_back(lifecycle_state_name(state));
    }
    return reject(make_error(ErrorCode::LifecycleTransitionNotAllowed,
                             "cannot move rack from " +
                                 std::string(lifecycle_state_name(found->second.record.lifecycle)) +
                                 " to " + std::string(lifecycle_state_name(request.target)),
                             ErrorDetail{.operation = "transition_lifecycle",
                                         .subject = rack_id_text(request.rack_id),
                                         .related =
                                             std::string(lifecycle_state_name(request.target)),
                                         .items = allowed}));
  }
  if (found->second.record.generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack generation counter is exhausted and cannot be advanced",
                             ErrorDetail{.operation = "transition_lifecycle",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  RackEntry& entry = found->second;
  push_history(entry);
  entry.record.lifecycle = request.target;
  entry.record.generation = entry.record.generation.next();
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt =
      make_receipt(entry.record, OperationKind::TransitionLifecycle, std::nullopt, false);
  remember_request(entry.record, request.identity.request_id, content,
                   OperationKind::TransitionLifecycle, receipt.value());
  return receipt;
}

Result<MutationReceipt> RackRegistry::insert_member(const InsertMemberRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "insert_member");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  const auto found = impl_->racks.find(request.rack_id);
  if (found == impl_->racks.end()) {
    return reject(make_error(ErrorCode::UnknownRackId,
                             "rack " + rack_id_text(request.rack_id) + " is not registered",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  internal::ByteWriter body;
  body.text(request.member_id.text());
  body.text(request.asset_id.text());
  body.text(request.mount.to_text());
  body.u8(static_cast<std::uint8_t>(request.state));
  body.text(request.requirements.required.to_text());
  body.text(request.requirements.forbids.to_text());
  const StateDigest content =
      request_content_digest(OperationKind::InsertMember, request.rack_id, body);

  const ReplayLookup replay =
      lookup_replay(found->second.record, request.identity.request_id, content);
  if (replay.conflict) {
    return reject(make_error(ErrorCode::RequestIdConflict,
                             "request identity " + request.identity.request_id->text() +
                                 " was already used with different content",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (replay.found) {
    return replay.receipt;
  }
  if (!(found->second.record.generation == request.precondition.expected_generation)) {
    return reject(make_error(
        ErrorCode::StaleRackGeneration,
        "expected rack generation " +
            std::to_string(request.precondition.expected_generation.value()) + " but rack is at " +
            std::to_string(found->second.record.generation.value()),
        ErrorDetail{.operation = "insert_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_generation.value(),
                    .actual = found->second.record.generation.value()}));
  }
  if (!(found->second.record.membership_generation ==
        request.precondition.expected_membership_generation)) {
    return reject(make_error(
        ErrorCode::StaleMembershipGeneration,
        "expected membership generation " +
            std::to_string(request.precondition.expected_membership_generation.value()) +
            " but rack membership is at " +
            std::to_string(found->second.record.membership_generation.value()),
        ErrorDetail{.operation = "insert_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_membership_generation.value(),
                    .actual = found->second.record.membership_generation.value()}));
  }
  if (!permits_membership_mutation(found->second.record.lifecycle)) {
    return reject(make_error(ErrorCode::LifecycleMutationForbidden,
                             "rack lifecycle state " +
                                 std::string(lifecycle_state_name(found->second.record.lifecycle)) +
                                 " forbids membership mutation",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = std::string(lifecycle_state_name(
                                             found->second.record.lifecycle))}));
  }
  if (request.member_id.empty()) {
    return reject(make_error(ErrorCode::EmptyValue, "a member must be inserted with an identity",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (request.asset_id.empty()) {
    return reject(make_error(ErrorCode::EmptyValue, "a member must reference an asset",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (found->second.record.has_member(request.member_id)) {
    return reject(make_error(ErrorCode::DuplicateMemberId,
                             "member " + request.member_id.text() + " already exists in rack " +
                                 rack_id_text(request.rack_id),
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (const auto occupied = found->second.record.member_of_asset(request.asset_id);
      occupied.has_value()) {
    return reject(make_error(ErrorCode::DuplicateAssetPlacement,
                             "asset " + request.asset_id.text() +
                                 " is already placed as member " + occupied->text() + " in rack " +
                                 rack_id_text(request.rack_id),
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = occupied->text()}));
  }
  if (found->second.record.members.size() >= kMaxMembersPerRack) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack already holds the maximum number of members",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .expected = kMaxMembersPerRack,
                                         .actual = found->second.record.members.size()}));
  }
  if (found->second.record.generation.is_max() ||
      found->second.record.membership_generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack generation counter is exhausted and cannot be advanced",
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  const Status bounds =
      internal::check_mount_bounds(found->second.record.structure, request.mount, "insert_member");
  if (!bounds) {
    return reject(bounds.error());
  }
  const CompatibilityReport report =
      rackregistry::evaluate_compatibility(found->second.record.structure.profile, request.requirements);
  if (!report.compatible) {
    std::vector<std::string> items;
    for (const Trait& trait : report.missing) {
      items.push_back("missing:" + trait.text());
    }
    for (const Trait& trait : report.forbidden_present) {
      items.push_back("forbidden:" + trait.text());
    }
    return reject(make_error(ErrorCode::CompatibilityUnsatisfied,
                             "member " + request.member_id.text() + " is incompatible with profile " +
                                 found->second.record.structure.profile.id.text() + ": " +
                                 report.to_text(),
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text(),
                                         .items = items}));
  }
  const auto conflict = internal::find_occupancy_conflict(
      found->second.record.structure, found->second.record.members, request.mount, nullptr,
      "insert_member");
  if (!conflict) {
    return reject(conflict.error());
  }
  if (conflict.value().has_value()) {
    return reject(make_error(conflict.value()->code, conflict.value()->message,
                             ErrorDetail{.operation = "insert_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = conflict.value()->member_id.text()}));
  }

  RackEntry& entry = found->second;
  push_history(entry);
  entry.record.generation = entry.record.generation.next();
  entry.record.membership_generation = entry.record.membership_generation.next();

  MemberRecord member;
  member.member_id = request.member_id;
  member.asset_id = request.asset_id;
  member.mount = request.mount;
  member.state = request.state;
  member.requirements = request.requirements;
  member.mount_generation = entry.record.generation;
  member.asset_generation = entry.record.generation;
  member.created_at_membership_generation = entry.record.membership_generation;
  append_provenance(member.provenance, member.provenance_dropped, request.identity.provenance,
                    kMaxProvenanceRecordsPerMember);
  entry.record.members.emplace(request.member_id, std::move(member));
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt =
      make_receipt(entry.record, OperationKind::InsertMember, request.member_id, false);
  remember_request(entry.record, request.identity.request_id, content, OperationKind::InsertMember,
                   receipt.value());
  return receipt;
}

Result<MutationReceipt> RackRegistry::remove_member(const RemoveMemberRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "remove_member");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  const auto found = impl_->racks.find(request.rack_id);
  if (found == impl_->racks.end()) {
    return reject(make_error(ErrorCode::UnknownRackId,
                             "rack " + rack_id_text(request.rack_id) + " is not registered",
                             ErrorDetail{.operation = "remove_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  internal::ByteWriter body;
  body.text(request.member_id.text());
  const StateDigest content =
      request_content_digest(OperationKind::RemoveMember, request.rack_id, body);

  const ReplayLookup replay =
      lookup_replay(found->second.record, request.identity.request_id, content);
  if (replay.conflict) {
    return reject(make_error(ErrorCode::RequestIdConflict,
                             "request identity " + request.identity.request_id->text() +
                                 " was already used with different content",
                             ErrorDetail{.operation = "remove_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (replay.found) {
    return replay.receipt;
  }
  if (!(found->second.record.generation == request.precondition.expected_generation)) {
    return reject(make_error(
        ErrorCode::StaleRackGeneration,
        "expected rack generation " +
            std::to_string(request.precondition.expected_generation.value()) + " but rack is at " +
            std::to_string(found->second.record.generation.value()),
        ErrorDetail{.operation = "remove_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_generation.value(),
                    .actual = found->second.record.generation.value()}));
  }
  if (!(found->second.record.membership_generation ==
        request.precondition.expected_membership_generation)) {
    return reject(make_error(
        ErrorCode::StaleMembershipGeneration,
        "expected membership generation " +
            std::to_string(request.precondition.expected_membership_generation.value()) +
            " but rack membership is at " +
            std::to_string(found->second.record.membership_generation.value()),
        ErrorDetail{.operation = "remove_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_membership_generation.value(),
                    .actual = found->second.record.membership_generation.value()}));
  }
  if (!permits_membership_mutation(found->second.record.lifecycle)) {
    return reject(make_error(ErrorCode::LifecycleMutationForbidden,
                             "rack lifecycle state " +
                                 std::string(lifecycle_state_name(found->second.record.lifecycle)) +
                                 " forbids membership mutation",
                             ErrorDetail{.operation = "remove_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = std::string(lifecycle_state_name(
                                             found->second.record.lifecycle))}));
  }
  if (!found->second.record.has_member(request.member_id)) {
    return reject(make_error(ErrorCode::UnknownMemberId,
                             "member " + request.member_id.text() + " does not exist in rack " +
                                 rack_id_text(request.rack_id),
                             ErrorDetail{.operation = "remove_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (found->second.record.generation.is_max() ||
      found->second.record.membership_generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack generation counter is exhausted and cannot be advanced",
                             ErrorDetail{.operation = "remove_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  RackEntry& entry = found->second;
  push_history(entry);
  entry.record.generation = entry.record.generation.next();
  entry.record.membership_generation = entry.record.membership_generation.next();
  entry.record.members.erase(request.member_id);
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt =
      make_receipt(entry.record, OperationKind::RemoveMember, request.member_id, false);
  remember_request(entry.record, request.identity.request_id, content, OperationKind::RemoveMember,
                   receipt.value());
  return receipt;
}

Result<MutationReceipt> RackRegistry::move_member(const MoveMemberRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "move_member");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  const auto found = impl_->racks.find(request.rack_id);
  if (found == impl_->racks.end()) {
    return reject(make_error(ErrorCode::UnknownRackId,
                             "rack " + rack_id_text(request.rack_id) + " is not registered",
                             ErrorDetail{.operation = "move_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  internal::ByteWriter body;
  body.text(request.member_id.text());
  body.text(request.target_mount.to_text());
  const StateDigest content =
      request_content_digest(OperationKind::MoveMember, request.rack_id, body);

  const ReplayLookup replay =
      lookup_replay(found->second.record, request.identity.request_id, content);
  if (replay.conflict) {
    return reject(make_error(ErrorCode::RequestIdConflict,
                             "request identity " + request.identity.request_id->text() +
                                 " was already used with different content",
                             ErrorDetail{.operation = "move_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (replay.found) {
    return replay.receipt;
  }
  if (!(found->second.record.generation == request.precondition.expected_generation)) {
    return reject(make_error(
        ErrorCode::StaleRackGeneration,
        "expected rack generation " +
            std::to_string(request.precondition.expected_generation.value()) + " but rack is at " +
            std::to_string(found->second.record.generation.value()),
        ErrorDetail{.operation = "move_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_generation.value(),
                    .actual = found->second.record.generation.value()}));
  }
  if (!(found->second.record.membership_generation ==
        request.precondition.expected_membership_generation)) {
    return reject(make_error(
        ErrorCode::StaleMembershipGeneration,
        "expected membership generation " +
            std::to_string(request.precondition.expected_membership_generation.value()) +
            " but rack membership is at " +
            std::to_string(found->second.record.membership_generation.value()),
        ErrorDetail{.operation = "move_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_membership_generation.value(),
                    .actual = found->second.record.membership_generation.value()}));
  }
  if (!permits_membership_mutation(found->second.record.lifecycle)) {
    return reject(make_error(ErrorCode::LifecycleMutationForbidden,
                             "rack lifecycle state " +
                                 std::string(lifecycle_state_name(found->second.record.lifecycle)) +
                                 " forbids membership mutation",
                             ErrorDetail{.operation = "move_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = std::string(lifecycle_state_name(
                                             found->second.record.lifecycle))}));
  }
  if (!found->second.record.has_member(request.member_id)) {
    return reject(make_error(ErrorCode::UnknownMemberId,
                             "member " + request.member_id.text() + " does not exist in rack " +
                                 rack_id_text(request.rack_id),
                             ErrorDetail{.operation = "move_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (found->second.record.generation.is_max() ||
      found->second.record.membership_generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack generation counter is exhausted and cannot be advanced",
                             ErrorDetail{.operation = "move_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  const Status bounds = internal::check_mount_bounds(found->second.record.structure,
                                                     request.target_mount, "move_member");
  if (!bounds) {
    return reject(bounds.error());
  }
  const auto conflict = internal::find_occupancy_conflict(
      found->second.record.structure, found->second.record.members, request.target_mount,
      &request.member_id, "move_member");
  if (!conflict) {
    return reject(conflict.error());
  }
  if (conflict.value().has_value()) {
    return reject(make_error(conflict.value()->code, conflict.value()->message,
                             ErrorDetail{.operation = "move_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = conflict.value()->member_id.text()}));
  }

  RackEntry& entry = found->second;
  push_history(entry);
  entry.record.generation = entry.record.generation.next();
  entry.record.membership_generation = entry.record.membership_generation.next();
  MemberRecord& target = entry.record.members.at(request.member_id);
  target.mount = request.target_mount;
  target.mount_generation = entry.record.generation;
  append_provenance(target.provenance, target.provenance_dropped, request.identity.provenance,
                    kMaxProvenanceRecordsPerMember);
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt =
      make_receipt(entry.record, OperationKind::MoveMember, request.member_id, false);
  remember_request(entry.record, request.identity.request_id, content, OperationKind::MoveMember,
                   receipt.value());
  return receipt;
}

Result<MutationReceipt> RackRegistry::replace_member(const ReplaceMemberRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const auto reject = [this](const RackError& error) -> Result<MutationReceipt> {
    record_rejection(impl_->rejections, impl_->rejection_sequence, impl_->total_rejections, error,
                     "replace_member");
    return error;
  };

  const Status provenance = validate_provenance(request.identity.provenance);
  if (!provenance) {
    return reject(provenance.error());
  }
  const auto found = impl_->racks.find(request.rack_id);
  if (found == impl_->racks.end()) {
    return reject(make_error(ErrorCode::UnknownRackId,
                             "rack " + rack_id_text(request.rack_id) + " is not registered",
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  internal::ByteWriter body;
  body.text(request.member_id.text());
  body.text(request.replacement_asset.text());
  body.u8(static_cast<std::uint8_t>(request.target_state));
  body.text(request.requirements.required.to_text());
  body.text(request.requirements.forbids.to_text());
  const StateDigest content =
      request_content_digest(OperationKind::ReplaceMember, request.rack_id, body);

  const ReplayLookup replay =
      lookup_replay(found->second.record, request.identity.request_id, content);
  if (replay.conflict) {
    return reject(make_error(ErrorCode::RequestIdConflict,
                             "request identity " + request.identity.request_id->text() +
                                 " was already used with different content",
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }
  if (replay.found) {
    return replay.receipt;
  }
  if (!(found->second.record.generation == request.precondition.expected_generation)) {
    return reject(make_error(
        ErrorCode::StaleRackGeneration,
        "expected rack generation " +
            std::to_string(request.precondition.expected_generation.value()) + " but rack is at " +
            std::to_string(found->second.record.generation.value()),
        ErrorDetail{.operation = "replace_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_generation.value(),
                    .actual = found->second.record.generation.value()}));
  }
  if (!(found->second.record.membership_generation ==
        request.precondition.expected_membership_generation)) {
    return reject(make_error(
        ErrorCode::StaleMembershipGeneration,
        "expected membership generation " +
            std::to_string(request.precondition.expected_membership_generation.value()) +
            " but rack membership is at " +
            std::to_string(found->second.record.membership_generation.value()),
        ErrorDetail{.operation = "replace_member",
                    .subject = rack_id_text(request.rack_id),
                    .expected = request.precondition.expected_membership_generation.value(),
                    .actual = found->second.record.membership_generation.value()}));
  }
  if (!permits_membership_mutation(found->second.record.lifecycle)) {
    return reject(make_error(ErrorCode::LifecycleMutationForbidden,
                             "rack lifecycle state " +
                                 std::string(lifecycle_state_name(found->second.record.lifecycle)) +
                                 " forbids membership mutation",
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = std::string(lifecycle_state_name(
                                             found->second.record.lifecycle))}));
  }
  const auto member = found->second.record.members.find(request.member_id);
  if (member == found->second.record.members.end()) {
    return reject(make_error(ErrorCode::UnknownMemberId,
                             "member " + request.member_id.text() + " does not exist in rack " +
                                 rack_id_text(request.rack_id),
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (request.replacement_asset.empty()) {
    return reject(make_error(ErrorCode::EmptyValue, "a replacement must reference an asset",
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (member->second.asset_id == request.replacement_asset) {
    return reject(make_error(ErrorCode::DuplicateAssetPlacement,
                             "asset " + request.replacement_asset.text() +
                                 " is already the asset of member " + request.member_id.text(),
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text()}));
  }
  if (const auto occupied = found->second.record.member_of_asset(request.replacement_asset);
      occupied.has_value()) {
    return reject(make_error(ErrorCode::DuplicateAssetPlacement,
                             "asset " + request.replacement_asset.text() +
                                 " is already placed as member " + occupied->text() + " in rack " +
                                 rack_id_text(request.rack_id),
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = occupied->text()}));
  }
  if (found->second.record.generation.is_max() ||
      found->second.record.membership_generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "rack generation counter is exhausted and cannot be advanced",
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id)}));
  }

  const CompatibilityReport report =
      rackregistry::evaluate_compatibility(found->second.record.structure.profile, request.requirements);
  if (!report.compatible) {
    std::vector<std::string> items;
    for (const Trait& trait : report.missing) {
      items.push_back("missing:" + trait.text());
    }
    for (const Trait& trait : report.forbidden_present) {
      items.push_back("forbidden:" + trait.text());
    }
    return reject(make_error(ErrorCode::CompatibilityUnsatisfied,
                             "replacement asset " + request.replacement_asset.text() +
                                 " is incompatible with profile " +
                                 found->second.record.structure.profile.id.text() + ": " +
                                 report.to_text(),
                             ErrorDetail{.operation = "replace_member",
                                         .subject = rack_id_text(request.rack_id),
                                         .related = request.member_id.text(),
                                         .items = items}));
  }

  RackEntry& entry = found->second;
  push_history(entry);
  entry.record.generation = entry.record.generation.next();
  entry.record.membership_generation = entry.record.membership_generation.next();
  MemberRecord& target = entry.record.members.at(request.member_id);
  target.asset_id = request.replacement_asset;
  target.asset_generation = entry.record.generation;
  target.state = request.target_state;
  target.requirements = request.requirements;
  append_provenance(target.provenance, target.provenance_dropped, request.identity.provenance,
                    kMaxProvenanceRecordsPerMember);
  append_provenance(entry.record.provenance, entry.record.provenance_dropped,
                    request.identity.provenance, kMaxProvenanceRecordsPerRack);
  append_evidence(entry.record);

  const auto receipt =
      make_receipt(entry.record, OperationKind::ReplaceMember, request.member_id, false);
  remember_request(entry.record, request.identity.request_id, content,
                   OperationKind::ReplaceMember, receipt.value());
  return receipt;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool RackRegistry::contains_rack(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->racks.find(rack_id) != impl_->racks.end();
}

std::size_t RackRegistry::rack_count() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->racks.size();
}

Result<RackView> RackRegistry::rack(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->racks.find(rack_id);
  if (found == impl_->racks.end()) {
    return make_error(ErrorCode::UnknownRackId,
                      "rack " + rack_id_text(rack_id) + " is not registered",
                      ErrorDetail{.operation = "rack", .subject = rack_id_text(rack_id)});
  }
  return RackView(found->second.record);
}

std::vector<RackView> RackRegistry::racks() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<RackView> views;
  views.reserve(impl_->racks.size());
  for (const auto& [rack_id, entry] : impl_->racks) {
    (void)rack_id;
    views.emplace_back(entry.record);
  }
  return views;
}

Result<std::vector<MemberRecord>> RackRegistry::members(const RackId& rack_id,
                                                       MemberOrder order) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "members");
  if (!entry) {
    return entry.error();
  }
  return RackView(entry.value()->record).members(order);
}

Result<std::optional<MemberRecord>> RackRegistry::member(const RackId& rack_id,
                                                         const RackMemberId& member_id) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "member");
  if (!entry) {
    return entry.error();
  }
  const auto found = entry.value()->record.members.find(member_id);
  if (found == entry.value()->record.members.end()) {
    return std::optional<MemberRecord>{};
  }
  return std::optional<MemberRecord>{found->second};
}

Result<std::vector<OccupancyRecord>> RackRegistry::occupancy(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "occupancy");
  if (!entry) {
    return entry.error();
  }
  return RackView(entry.value()->record).occupancy();
}

Result<std::vector<FreeSpan>> RackRegistry::free_spans(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "free_spans");
  if (!entry) {
    return entry.error();
  }
  return RackView(entry.value()->record).free_spans();
}

Result<std::vector<SharedSpanAvailability>> RackRegistry::shared_availability(
    const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "shared_availability");
  if (!entry) {
    return entry.error();
  }
  return RackView(entry.value()->record).shared_availability();
}

Result<CompatibilityReport> RackRegistry::evaluate_compatibility(
    const RackId& rack_id, const MemberRequirements& requirements) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "evaluate_compatibility");
  if (!entry) {
    return entry.error();
  }
  return rackregistry::evaluate_compatibility(entry.value()->record.structure.profile, requirements);
}

Result<RackDiff> RackRegistry::diff_generations(const RackId& rack_id, RackGeneration from) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "diff_generations");
  if (!entry) {
    return entry.error();
  }
  const RackEntry& rack = *entry.value();
  const RackRecord* previous = find_retained(rack, from);
  if (previous == nullptr) {
    return make_error(ErrorCode::GenerationNotRetained,
                      "generation " + std::to_string(from.value()) + " of rack " +
                          rack_id_text(rack_id) + " is no longer retained for comparison",
                      ErrorDetail{.operation = "diff_generations",
                                  .subject = rack_id_text(rack_id),
                                  .expected = rack.record.generation.value(),
                                  .actual = from.value()});
  }
  return diff_records(*previous, rack.record);
}

Result<RackDiff> RackRegistry::diff_generations(const RackId& rack_id, RackGeneration from,
                                                RackGeneration to) const {
  std::shared_lock lock(impl_->mutex);
  const auto entry = find_entry(impl_->racks, rack_id, "diff_generations");
  if (!entry) {
    return entry.error();
  }
  const RackEntry& rack = *entry.value();
  const RackRecord* previous = find_retained(rack, from);
  if (previous == nullptr) {
    return make_error(ErrorCode::GenerationNotRetained,
                      "generation " + std::to_string(from.value()) + " of rack " +
                          rack_id_text(rack_id) + " is no longer retained for comparison",
                      ErrorDetail{.operation = "diff_generations",
                                  .subject = rack_id_text(rack_id),
                                  .expected = rack.record.generation.value(),
                                  .actual = from.value()});
  }
  const RackRecord* current = find_retained(rack, to);
  if (current == nullptr) {
    return make_error(ErrorCode::GenerationNotRetained,
                      "generation " + std::to_string(to.value()) + " of rack " +
                          rack_id_text(rack_id) + " is no longer retained for comparison",
                      ErrorDetail{.operation = "diff_generations",
                                  .subject = rack_id_text(rack_id),
                                  .expected = rack.record.generation.value(),
                                  .actual = to.value()});
  }
  return diff_records(*previous, *current);
}

// ---------------------------------------------------------------------------
// Whole-registry state
// ---------------------------------------------------------------------------

RackSnapshot RackRegistry::snapshot(const std::optional<SourceReference>& produced_by) const {
  std::shared_lock lock(impl_->mutex);
  RackSnapshot result;
  result.racks_.reserve(impl_->racks.size());
  for (const auto& [rack_id, entry] : impl_->racks) {
    (void)rack_id;
    result.racks_.push_back(entry.record);
  }
  result.produced_by_ = produced_by;
  return result;
}

StateDigest RackRegistry::state_digest() const { return snapshot().state_digest(); }

Status RackRegistry::restore(const RackSnapshot& snapshot) {
  const Status valid = snapshot.validate();
  if (!valid) {
    return make_error(ErrorCode::SnapshotInvalid,
                      "snapshot cannot be restored: " + valid.error().message,
                      valid.error().detail);
  }
  std::unique_lock lock(impl_->mutex);
  std::map<RackId, RackEntry> replacement;
  for (const RackRecord& record : snapshot.racks()) {
    RackEntry entry;
    entry.record = record;
    replacement.emplace(record.structure.id, std::move(entry));
  }
  impl_->racks = std::move(replacement);
  return Status{};
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

std::vector<RejectionRecord> RackRegistry::rejections() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->rejections;
}

void RackRegistry::clear_rejections() {
  std::unique_lock lock(impl_->mutex);
  impl_->rejections.clear();
}

RegistryStats RackRegistry::stats() const {
  std::shared_lock lock(impl_->mutex);
  RegistryStats stats;
  stats.rack_count = impl_->racks.size();
  for (const auto& [rack_id, entry] : impl_->racks) {
    (void)rack_id;
    stats.member_count += entry.record.members.size();
    if (is_immutable(entry.record.lifecycle)) {
      ++stats.immutable_rack_count;
    }
    stats.retained_generation_records += entry.history.size();
    stats.generation_evidence_records += entry.record.generation_evidence.size();
    stats.idempotency_records += entry.record.idempotency.size();
    stats.provenance_records += entry.record.provenance.size();
    for (const auto& [member_id, member] : entry.record.members) {
      (void)member_id;
      stats.provenance_records += member.provenance.size();
    }
  }
  stats.rejection_records = impl_->rejections.size();
  stats.total_rejections = impl_->total_rejections;
  return stats;
}

}  // namespace rackregistry
