// Rack Registry - canonical serialization of snapshots and records.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The encoding is little-endian, length-prefixed and free of padding, so two
// equal snapshots always produce identical bytes on every platform and with
// every compiler. Every declared count is checked against its documented bound
// and against the bytes that are actually present before anything is
// allocated.

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "internal.hpp"
#include "rack_registry/limits.hpp"
#include "rack_registry/requests.hpp"
#include "rack_registry/text.hpp"
#include "rack_registry/version.hpp"

namespace rackregistry {
namespace internal {
namespace {

constexpr std::string_view kRackDigestDomain = "rack-registry.rack-record.v1";
constexpr std::string_view kSnapshotDigestDomain = "rack-registry.snapshot.v1";

// Fixed part of the file header: magic, four u32 fields, two u64 fields, three
// producer version fields, the payload length, the payload digest, the producer
// flag and the header digest. A producer identity, when present, follows the
// flag as a length-prefixed string.
constexpr std::size_t kFixedHeaderBytes = kFileMagic.size() + 4 * 4 + 8 * 2 + 4 * 3 + 8 +
                                          Sha256::kDigestBytes + 1 + Sha256::kDigestBytes;

// Smallest number of encoded bytes one element of a collection can occupy.
// Used to reject an absurd count before allocating for it.
constexpr std::size_t kMinProvenanceBytes = 1 + 1 + 1 + 1 + 8 + 8 + 1;
constexpr std::size_t kMinMemberBytes = 1 + 1 + 1 + 4 + 4 + 1 + 4 + 1 + 8 + 8 + 8 + 4 + 4 + 4;
constexpr std::size_t kMinEvidenceBytes = 8 + 8 + 8 + 1 + 4 + Sha256::kDigestBytes;
constexpr std::size_t kMinIdempotencyBytes =
    1 + Sha256::kDigestBytes + 1 + 1 + 8 + 8 + 8;

std::string corrupt(std::string what) {
  return "stored state is invalid: " + std::move(what);
}

bool fail(RackError& error, ErrorCode code, std::string message,
          std::string_view subject = {}) {
  error = make_error(code, std::move(message),
                     ErrorDetail{.operation = "decode", .subject = std::string(subject)});
  return false;
}

bool fail_from(RackError& error, const RackError& inner, std::string_view what) {
  error = make_error(ErrorCode::InvalidStateEncoding,
                     std::string(what) + ": " + inner.message,
                     ErrorDetail{.operation = "decode",
                                 .items = {std::string(code_name(inner.code))}});
  return false;
}

void write_provenance(ByteWriter& writer, const ProvenanceRecord& provenance) {
  writer.u8(static_cast<std::uint8_t>(provenance.source));
  writer.text(provenance.actor.text());
  writer.u8(provenance.source_reference.has_value() ? 1u : 0u);
  if (provenance.source_reference.has_value()) {
    writer.text(provenance.source_reference->text());
  }
  writer.u64(provenance.source_sequence);
  writer.u64(provenance.observed_at_unix_ns);
  writer.text(provenance.note.text());
}

bool read_provenance(ByteReader& reader, ProvenanceRecord& provenance, RackError& error) {
  std::uint8_t source_value = 0;
  std::string actor_text;
  std::uint8_t has_reference = 0;
  std::string reference_text;
  std::uint64_t sequence = 0;
  std::uint64_t observed_at = 0;
  std::string note_text;
  if (!reader.u8(source_value) || !reader.text(actor_text, kMaxActorBytes) ||
      !reader.u8(has_reference)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("provenance record is truncated"));
  }
  if (has_reference > 1) {
    return fail(error, ErrorCode::CorruptState, corrupt("provenance reference flag is not a boolean"));
  }
  if (has_reference == 1 && !reader.text(reference_text, kMaxSourceReferenceBytes)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("provenance reference is truncated"));
  }
  if (!reader.u64(sequence) || !reader.u64(observed_at) ||
      !reader.text(note_text, kMaxNoteBytes)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("provenance record is truncated"));
  }

  const auto source = provenance_source_from_value(source_value);
  if (!source) {
    return fail_from(error, source.error(), "provenance source");
  }
  const auto actor = ActorId::parse(actor_text);
  if (!actor) {
    return fail_from(error, actor.error(), "provenance actor");
  }
  const auto note = Note::parse_or_none(note_text);
  if (!note) {
    return fail_from(error, note.error(), "provenance note");
  }

  ProvenanceRecord decoded;
  decoded.source = source.value();
  decoded.actor = actor.value();
  if (has_reference == 1) {
    const auto reference = SourceReference::parse(reference_text);
    if (!reference) {
      return fail_from(error, reference.error(), "provenance source reference");
    }
    decoded.source_reference = reference.value();
  }
  decoded.source_sequence = sequence;
  decoded.observed_at_unix_ns = observed_at;
  decoded.note = note.value();
  provenance = std::move(decoded);
  return true;
}

void write_traits(ByteWriter& writer, const TraitSet& traits) {
  writer.u32(static_cast<std::uint32_t>(traits.size()));
  for (const Trait& trait : traits.traits()) {
    writer.text(trait.text());
  }
}

bool read_traits(ByteReader& reader, TraitSet& traits, RackError& error, std::string_view what) {
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("trait count is missing"));
  }
  if (count > kMaxTraitsPerSet) {
    return fail(error, ErrorCode::LimitExceeded,
                "stored trait count " + std::to_string(count) + " exceeds the bound");
  }
  if (count > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("trait count exceeds the bytes that remain"));
  }
  std::vector<Trait> collected;
  collected.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string text;
    if (!reader.text(text, kMaxTraitBytes)) {
      return fail(error, ErrorCode::TruncatedState, corrupt("trait text is truncated"));
    }
    const auto trait = Trait::parse(text);
    if (!trait) {
      return fail_from(error, trait.error(), what);
    }
    collected.push_back(trait.value());
  }
  const auto set = TraitSet::create(std::move(collected));
  if (!set) {
    return fail_from(error, set.error(), what);
  }
  traits = set.value();
  return true;
}

void write_domain_references(ByteWriter& writer,
                             const std::vector<PowerDomainReference>& references) {
  writer.u32(static_cast<std::uint32_t>(references.size()));
  for (const PowerDomainReference& reference : references) {
    writer.text(reference.text());
  }
}

void write_domain_references(ByteWriter& writer,
                             const std::vector<CoolingDomainReference>& references) {
  writer.u32(static_cast<std::uint32_t>(references.size()));
  for (const CoolingDomainReference& reference : references) {
    writer.text(reference.text());
  }
}

template <typename Reference>
bool read_domain_references(ByteReader& reader, std::vector<Reference>& references, RackError& error,
                            std::string_view what) {
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("domain reference count is missing"));
  }
  if (count > kMaxDomainReferencesPerRack) {
    return fail(error, ErrorCode::LimitExceeded,
                "stored domain reference count " + std::to_string(count) + " exceeds the bound");
  }
  if (count > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("domain reference count exceeds the bytes that remain"));
  }
  std::vector<Reference> collected;
  collected.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string text;
    if (!reader.text(text, kMaxOpaqueReferenceBytes)) {
      return fail(error, ErrorCode::TruncatedState, corrupt("domain reference is truncated"));
    }
    const auto reference = Reference::parse(text);
    if (!reference) {
      return fail_from(error, reference.error(), what);
    }
    collected.push_back(reference.value());
  }
  std::sort(collected.begin(), collected.end(),
            [](const Reference& a, const Reference& b) { return a < b; });
  for (std::size_t i = 1; i < collected.size(); ++i) {
    if (collected[i] == collected[i - 1]) {
      return fail(error, ErrorCode::DuplicateDomainReference,
                  corrupt("a domain reference appears more than once"));
    }
  }
  references = std::move(collected);
  return true;
}

// The shared prefix of both encodings: everything about a rack except its
// members and its history.
void write_rack_header(ByteWriter& writer, const RackRecord& record) {
  writer.text(record.structure.id.text());
  writer.u64(record.generation.value());
  writer.u64(record.revision.value());
  writer.u64(record.membership_generation.value());
  writer.u8(static_cast<std::uint8_t>(record.lifecycle));
  writer.u32(record.structure.unit_count);
  writer.text(record.structure.profile.id.text());
  write_traits(writer, record.structure.profile.provides);
  write_domain_references(writer, record.structure.power_domains);
  write_domain_references(writer, record.structure.cooling_domains);
  writer.text(record.structure.label.text());
}

// The authoritative state of one member: everything that determines what is
// where, without its provenance trail.
void write_member_state(ByteWriter& writer, const MemberRecord& member) {
  writer.text(member.member_id.text());
  writer.text(member.asset_id.text());
  writer.u8(static_cast<std::uint8_t>(member.mount.kind()));
  writer.u32(member.mount.span().begin());
  writer.u32(member.mount.span().end());
  writer.text(member.mount.shared_class().text());
  writer.u32(member.mount.share_capacity());
  writer.u8(static_cast<std::uint8_t>(member.state));
  writer.u64(member.mount_generation.value());
  writer.u64(member.asset_generation.value());
  writer.u64(member.created_at_membership_generation.value());
  write_traits(writer, member.requirements.required);
  write_traits(writer, member.requirements.forbids);
}

void write_member(ByteWriter& writer, const MemberRecord& member) {
  write_member_state(writer, member);
  writer.u32(static_cast<std::uint32_t>(member.provenance.size()));
  for (const ProvenanceRecord& provenance : member.provenance) {
    write_provenance(writer, provenance);
  }
  writer.u64(member.provenance_dropped);
}

bool read_member(ByteReader& reader, MemberRecord& member, RackError& error) {
  std::string member_id_text;
  std::string asset_id_text;
  std::uint8_t kind_value = 0;
  std::uint32_t span_begin = 0;
  std::uint32_t span_end = 0;
  std::string shared_class_text;
  std::uint32_t share_capacity = 0;
  std::uint8_t state_value = 0;
  std::uint64_t mount_generation = 0;
  std::uint64_t asset_generation = 0;
  std::uint64_t created_generation = 0;

  if (!reader.text(member_id_text, kMaxIdentityTextBytes) ||
      !reader.text(asset_id_text, kMaxOpaqueReferenceBytes) || !reader.u8(kind_value) ||
      !reader.u32(span_begin) || !reader.u32(span_end) ||
      !reader.text(shared_class_text, kMaxIdentityTextBytes) || !reader.u32(share_capacity) ||
      !reader.u8(state_value) || !reader.u64(mount_generation) || !reader.u64(asset_generation) ||
      !reader.u64(created_generation)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("member record is truncated"));
  }

  MountKind kind = MountKind::ZeroU;
  if (kind_value > static_cast<std::uint8_t>(MountKind::ZeroU)) {
    return fail(error, ErrorCode::InvalidEnumValue,
                corrupt("mount kind value is outside its domain"));
  }
  kind = static_cast<MountKind>(kind_value);
  const auto state = membership_state_from_value(state_value);
  if (!state) {
    return fail_from(error, state.error(), "membership state");
  }
  const auto member_id = RackMemberId::parse(member_id_text);
  if (!member_id) {
    return fail_from(error, member_id.error(), "member identity");
  }
  const auto asset_id = AssetId::parse(asset_id_text);
  if (!asset_id) {
    return fail_from(error, asset_id.error(), "asset reference");
  }

  MountSpan mount;
  if (kind == MountKind::ZeroU) {
    if (span_begin != 0 || span_end != 0 || !shared_class_text.empty() || share_capacity != 0) {
      return fail(error, ErrorCode::CorruptState,
                  corrupt("a zero-U mount must not carry a slot span or a shared class"));
    }
    mount = MountSpan::zero_u();
  } else {
    const auto span = SlotRange::create(span_begin, span_end);
    if (!span) {
      return fail_from(error, span.error(), "mount span");
    }
    if (kind == MountKind::FullSpan) {
      if (!shared_class_text.empty() || share_capacity != 0) {
        return fail(error, ErrorCode::CorruptState,
                    corrupt("a full-span mount must not carry a shared class or capacity"));
      }
      const auto created = MountSpan::full(span.value());
      if (!created) {
        return fail_from(error, created.error(), "mount span");
      }
      mount = created.value();
    } else {
      const auto shared_class = SharedMountClass::parse(shared_class_text);
      if (!shared_class) {
        return fail_from(error, shared_class.error(), "shared mount class");
      }
      const auto created = MountSpan::shared(span.value(), shared_class.value(), share_capacity);
      if (!created) {
        return fail_from(error, created.error(), "shared mount span");
      }
      mount = created.value();
    }
  }

  MemberRequirements requirements;
  if (!read_traits(reader, requirements.required, error, "member required traits")) {
    return false;
  }
  if (!read_traits(reader, requirements.forbids, error, "member forbidden traits")) {
    return false;
  }

  std::uint32_t provenance_count = 0;
  if (!reader.u32(provenance_count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("member provenance count is missing"));
  }
  if (provenance_count > kMaxProvenanceRecordsPerMember) {
    return fail(error, ErrorCode::LimitExceeded,
                "stored member provenance count exceeds the bound");
  }
  if (static_cast<std::size_t>(provenance_count) * kMinProvenanceBytes > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("member provenance count exceeds the bytes that remain"));
  }
  std::vector<ProvenanceRecord> provenance;
  provenance.reserve(provenance_count);
  for (std::uint32_t i = 0; i < provenance_count; ++i) {
    ProvenanceRecord record;
    if (!read_provenance(reader, record, error)) {
      return false;
    }
    provenance.push_back(std::move(record));
  }
  std::uint64_t provenance_dropped = 0;
  if (!reader.u64(provenance_dropped)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("member provenance drop count is missing"));
  }

  const auto mount_generation_value = RackGeneration::create(mount_generation);
  if (!mount_generation_value) {
    return fail_from(error, mount_generation_value.error(), "member mount generation");
  }
  const auto asset_generation_value = RackGeneration::create(asset_generation);
  if (!asset_generation_value) {
    return fail_from(error, asset_generation_value.error(), "member asset generation");
  }
  const auto created = MembershipGeneration::create(created_generation);
  if (!created) {
    return fail_from(error, created.error(), "member membership generation");
  }

  MemberRecord decoded;
  decoded.member_id = member_id.value();
  decoded.asset_id = asset_id.value();
  decoded.mount = mount;
  decoded.state = state.value();
  decoded.requirements = std::move(requirements);
  decoded.mount_generation = mount_generation_value.value();
  decoded.asset_generation = asset_generation_value.value();
  decoded.created_at_membership_generation = created.value();
  decoded.provenance = std::move(provenance);
  decoded.provenance_dropped = provenance_dropped;
  member = std::move(decoded);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Byte writer and reader
// ---------------------------------------------------------------------------

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(value); }

void ByteWriter::u32(std::uint32_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value));
  buffer_.push_back(static_cast<std::uint8_t>(value >> 8));
  buffer_.push_back(static_cast<std::uint8_t>(value >> 16));
  buffer_.push_back(static_cast<std::uint8_t>(value >> 24));
}

void ByteWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void ByteWriter::raw(const std::uint8_t* data, std::size_t size) {
  buffer_.insert(buffer_.end(), data, data + size);
}

void ByteWriter::text(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::digest(const StateDigest& value) {
  raw(value.bytes().data(), value.bytes().size());
}

bool ByteReader::u8(std::uint8_t& out) noexcept {
  if (remaining() < 1) {
    return false;
  }
  out = data_[position_];
  position_ += 1;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
  if (remaining() < 4) {
    return false;
  }
  out = static_cast<std::uint32_t>(data_[position_]) |
        (static_cast<std::uint32_t>(data_[position_ + 1]) << 8) |
        (static_cast<std::uint32_t>(data_[position_ + 2]) << 16) |
        (static_cast<std::uint32_t>(data_[position_ + 3]) << 24);
  position_ += 4;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
  if (remaining() < 8) {
    return false;
  }
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data_[position_ + i]) << (i * 8);
  }
  out = value;
  position_ += 8;
  return true;
}

bool ByteReader::raw(std::size_t count, const std::uint8_t*& out) noexcept {
  if (remaining() < count) {
    return false;
  }
  out = data_ + position_;
  position_ += count;
  return true;
}

bool ByteReader::text(std::string& out, std::size_t max_bytes) {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (length > max_bytes) {
    return false;
  }
  const std::uint8_t* pointer = nullptr;
  if (!raw(length, pointer)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(pointer), length);
  return true;
}

bool ByteReader::digest(StateDigest& out) noexcept {
  const std::uint8_t* pointer = nullptr;
  if (!raw(Sha256::kDigestBytes, pointer)) {
    return false;
  }
  std::array<std::uint8_t, Sha256::kDigestBytes> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = pointer[i];
  }
  out = StateDigest(bytes);
  return true;
}

bool ByteReader::skip(std::size_t count) noexcept {
  if (remaining() < count) {
    return false;
  }
  position_ += count;
  return true;
}

// ---------------------------------------------------------------------------
// Rack record encoding
// ---------------------------------------------------------------------------

void write_rack_state(ByteWriter& writer, const RackRecord& record) {
  write_rack_header(writer, record);

  writer.u32(static_cast<std::uint32_t>(record.members.size()));
  for (const auto& [member_id, member] : record.members) {
    (void)member_id;
    write_member_state(writer, member);
  }
}

void write_rack_record(ByteWriter& writer, const RackRecord& record) {
  write_rack_header(writer, record);

  writer.u32(static_cast<std::uint32_t>(record.members.size()));
  for (const auto& [member_id, member] : record.members) {
    (void)member_id;
    write_member(writer, member);
  }

  writer.u32(static_cast<std::uint32_t>(record.provenance.size()));
  for (const ProvenanceRecord& provenance : record.provenance) {
    write_provenance(writer, provenance);
  }
  writer.u64(record.provenance_dropped);

  writer.u32(static_cast<std::uint32_t>(record.generation_evidence.size()));
  for (const GenerationEvidence& evidence : record.generation_evidence) {
    writer.u64(evidence.generation.value());
    writer.u64(evidence.revision.value());
    writer.u64(evidence.membership_generation.value());
    writer.u8(static_cast<std::uint8_t>(evidence.lifecycle));
    writer.u32(evidence.member_count);
    writer.digest(evidence.state_digest);
  }

  writer.u32(static_cast<std::uint32_t>(record.idempotency.size()));
  for (const IdempotencyRecord& entry : record.idempotency) {
    writer.text(entry.request_id.text());
    writer.digest(entry.content_digest);
    writer.u8(entry.operation);
    writer.text(entry.member_id.text());
    writer.u64(entry.generation.value());
    writer.u64(entry.revision.value());
    writer.u64(entry.membership_generation.value());
  }
  writer.u64(record.idempotency_dropped);
}

bool read_rack_record(ByteReader& reader, RackRecord& record, RackError& error) {
  std::string rack_id_text;
  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  std::uint64_t membership_generation = 0;
  std::uint8_t lifecycle_value = 0;
  std::uint32_t unit_count = 0;
  std::string profile_id_text;

  if (!reader.text(rack_id_text, kMaxIdentityTextBytes) || !reader.u64(generation) ||
      !reader.u64(revision) || !reader.u64(membership_generation) ||
      !reader.u8(lifecycle_value) || !reader.u32(unit_count) ||
      !reader.text(profile_id_text, kMaxIdentityTextBytes)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("rack record header is truncated"));
  }

  const auto rack_id = RackId::parse(rack_id_text);
  if (!rack_id) {
    return fail_from(error, rack_id.error(), "rack identity");
  }
  const auto rack_generation = RackGeneration::create(generation);
  if (!rack_generation) {
    return fail_from(error, rack_generation.error(), "rack generation");
  }
  const auto rack_revision = RackRevision::create(revision);
  if (!rack_revision) {
    return fail_from(error, rack_revision.error(), "rack revision");
  }
  const auto membership = MembershipGeneration::create(membership_generation);
  if (!membership) {
    return fail_from(error, membership.error(), "membership generation");
  }
  const auto lifecycle = lifecycle_state_from_value(lifecycle_value);
  if (!lifecycle) {
    return fail_from(error, lifecycle.error(), "lifecycle state");
  }
  const auto units = validate_unit_count(unit_count);
  if (!units) {
    return fail_from(error, units.error(), "rack unit count");
  }
  const auto profile_id = CompatibilityProfileId::parse(profile_id_text);
  if (!profile_id) {
    return fail_from(error, profile_id.error(), "compatibility profile identity");
  }

  RackRecord decoded;
  decoded.structure.id = rack_id.value();
  decoded.generation = rack_generation.value();
  decoded.revision = rack_revision.value();
  decoded.membership_generation = membership.value();
  decoded.lifecycle = lifecycle.value();
  decoded.structure.unit_count = units.value();
  decoded.structure.profile.id = profile_id.value();

  if (!read_traits(reader, decoded.structure.profile.provides, error,
                   "rack compatibility profile traits")) {
    return false;
  }
  if (!read_domain_references(reader, decoded.structure.power_domains, error,
                              "power domain reference")) {
    return false;
  }
  if (!read_domain_references(reader, decoded.structure.cooling_domains, error,
                              "cooling domain reference")) {
    return false;
  }

  std::string label_text;
  if (!reader.text(label_text, kMaxLabelBytes)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("rack label is truncated"));
  }
  const auto label = DisplayLabel::parse_or_none(label_text);
  if (!label) {
    return fail_from(error, label.error(), "rack label");
  }
  decoded.structure.label = label.value();

  std::uint32_t member_count = 0;
  if (!reader.u32(member_count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("member count is missing"));
  }
  if (member_count > kMaxMembersPerRack) {
    return fail(error, ErrorCode::LimitExceeded,
                "stored member count " + std::to_string(member_count) + " exceeds the bound");
  }
  if (static_cast<std::size_t>(member_count) * kMinMemberBytes > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("member count exceeds the bytes that remain"));
  }
  for (std::uint32_t i = 0; i < member_count; ++i) {
    MemberRecord member;
    if (!read_member(reader, member, error)) {
      return false;
    }
    const auto inserted = decoded.members.emplace(member.member_id, std::move(member));
    if (!inserted.second) {
      return fail(error, ErrorCode::DuplicateMemberId,
                  corrupt("a member identity appears more than once in one rack"));
    }
  }

  std::uint32_t provenance_count = 0;
  if (!reader.u32(provenance_count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("rack provenance count is missing"));
  }
  if (provenance_count > kMaxProvenanceRecordsPerRack) {
    return fail(error, ErrorCode::LimitExceeded, "stored rack provenance count exceeds the bound");
  }
  if (static_cast<std::size_t>(provenance_count) * kMinProvenanceBytes > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("rack provenance count exceeds the bytes that remain"));
  }
  decoded.provenance.reserve(provenance_count);
  for (std::uint32_t i = 0; i < provenance_count; ++i) {
    ProvenanceRecord provenance;
    if (!read_provenance(reader, provenance, error)) {
      return false;
    }
    decoded.provenance.push_back(std::move(provenance));
  }
  if (!reader.u64(decoded.provenance_dropped)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("rack provenance drop count is missing"));
  }

  std::uint32_t evidence_count = 0;
  if (!reader.u32(evidence_count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("generation evidence count is missing"));
  }
  if (evidence_count > kMaxRetainedGenerationsPerRack) {
    return fail(error, ErrorCode::LimitExceeded, "stored generation evidence count exceeds the bound");
  }
  if (static_cast<std::size_t>(evidence_count) * kMinEvidenceBytes > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("generation evidence count exceeds the bytes that remain"));
  }
  decoded.generation_evidence.reserve(evidence_count);
  for (std::uint32_t i = 0; i < evidence_count; ++i) {
    std::uint64_t evidence_generation = 0;
    std::uint64_t evidence_revision = 0;
    std::uint64_t evidence_membership = 0;
    std::uint8_t evidence_lifecycle = 0;
    std::uint32_t evidence_members = 0;
    StateDigest evidence_digest;
    if (!reader.u64(evidence_generation) || !reader.u64(evidence_revision) ||
        !reader.u64(evidence_membership) || !reader.u8(evidence_lifecycle) ||
        !reader.u32(evidence_members) || !reader.digest(evidence_digest)) {
      return fail(error, ErrorCode::TruncatedState, corrupt("generation evidence is truncated"));
    }
    GenerationEvidence evidence;
    const auto generation_value = RackGeneration::create(evidence_generation);
    if (!generation_value) {
      return fail_from(error, generation_value.error(), "generation evidence generation");
    }
    const auto revision_value = RackRevision::create(evidence_revision);
    if (!revision_value) {
      return fail_from(error, revision_value.error(), "generation evidence revision");
    }
    const auto membership_value = MembershipGeneration::create(evidence_membership);
    if (!membership_value) {
      return fail_from(error, membership_value.error(), "generation evidence membership");
    }
    const auto evidence_lifecycle_value = lifecycle_state_from_value(evidence_lifecycle);
    if (!evidence_lifecycle_value) {
      return fail_from(error, evidence_lifecycle_value.error(), "generation evidence lifecycle");
    }
    if (evidence_members > kMaxMembersPerRack) {
      return fail(error, ErrorCode::LimitExceeded,
                  "generation evidence member count exceeds the bound");
    }
    evidence.generation = generation_value.value();
    evidence.revision = revision_value.value();
    evidence.membership_generation = membership_value.value();
    evidence.lifecycle = evidence_lifecycle_value.value();
    evidence.member_count = evidence_members;
    evidence.state_digest = evidence_digest;
    decoded.generation_evidence.push_back(evidence);
  }

  std::uint32_t idempotency_count = 0;
  if (!reader.u32(idempotency_count)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("idempotency count is missing"));
  }
  if (idempotency_count > kMaxIdempotencyRecordsPerRack) {
    return fail(error, ErrorCode::LimitExceeded, "stored idempotency count exceeds the bound");
  }
  if (static_cast<std::size_t>(idempotency_count) * kMinIdempotencyBytes > reader.remaining()) {
    return fail(error, ErrorCode::TruncatedState,
                corrupt("idempotency count exceeds the bytes that remain"));
  }
  decoded.idempotency.reserve(idempotency_count);
  for (std::uint32_t i = 0; i < idempotency_count; ++i) {
    std::string request_id_text;
    StateDigest content_digest;
    std::uint8_t operation = 0;
    std::string member_id_text;
    std::uint64_t entry_generation = 0;
    std::uint64_t entry_revision = 0;
    std::uint64_t entry_membership = 0;
    if (!reader.text(request_id_text, kMaxRequestIdBytes) || !reader.digest(content_digest) ||
        !reader.u8(operation) || !reader.text(member_id_text, kMaxIdentityTextBytes) ||
        !reader.u64(entry_generation) || !reader.u64(entry_revision) ||
        !reader.u64(entry_membership)) {
      return fail(error, ErrorCode::TruncatedState, corrupt("idempotency record is truncated"));
    }
    const auto request_id = RequestId::parse(request_id_text);
    if (!request_id) {
      return fail_from(error, request_id.error(), "idempotency request identity");
    }
    if (operation >= kOperationKindCount) {
      return fail(error, ErrorCode::InvalidEnumValue,
                  corrupt("idempotency record names an unknown operation"));
    }
    const auto entry_generation_value = RackGeneration::create(entry_generation);
    if (!entry_generation_value) {
      return fail_from(error, entry_generation_value.error(), "idempotency generation");
    }
    const auto entry_revision_value = RackRevision::create(entry_revision);
    if (!entry_revision_value) {
      return fail_from(error, entry_revision_value.error(), "idempotency revision");
    }
    const auto entry_membership_value = MembershipGeneration::create(entry_membership);
    if (!entry_membership_value) {
      return fail_from(error, entry_membership_value.error(), "idempotency membership generation");
    }
    IdempotencyRecord entry;
    entry.request_id = request_id.value();
    entry.content_digest = content_digest;
    entry.operation = operation;
    if (!member_id_text.empty()) {
      const auto member_id = RackMemberId::parse(member_id_text);
      if (!member_id) {
        return fail_from(error, member_id.error(), "idempotency member identity");
      }
      entry.member_id = member_id.value();
    }
    entry.generation = entry_generation_value.value();
    entry.revision = entry_revision_value.value();
    entry.membership_generation = entry_membership_value.value();
    decoded.idempotency.push_back(std::move(entry));
  }
  if (!reader.u64(decoded.idempotency_dropped)) {
    return fail(error, ErrorCode::TruncatedState, corrupt("idempotency drop count is missing"));
  }

  record = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> encode_rack_record(const RackRecord& record) {
  ByteWriter writer;
  write_rack_state(writer, record);
  return std::move(writer).take();
}

StateDigest rack_record_digest(const RackRecord& record) {
  return StateDigest::domain(kRackDigestDomain, encode_rack_record(record));
}

// ---------------------------------------------------------------------------
// Snapshot payload
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encode_snapshot_payload(const RackSnapshot& snapshot) {
  ByteWriter writer;
  writer.u32(kSnapshotLayoutVersion);
  writer.u32(static_cast<std::uint32_t>(snapshot.racks().size()));
  for (const RackRecord& record : snapshot.racks()) {
    write_rack_record(writer, record);
  }
  return std::move(writer).take();
}

Result<std::vector<RackRecord>> decode_snapshot_payload(const std::uint8_t* data, std::size_t size) {
  ByteReader reader(data, size);
  std::uint32_t layout = 0;
  std::uint32_t rack_count = 0;
  if (!reader.u32(layout) || !reader.u32(rack_count)) {
    return make_error(ErrorCode::TruncatedState, "snapshot payload header is truncated",
                      ErrorDetail{.operation = "decode_snapshot_payload"});
  }
  if (layout != kSnapshotLayoutVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "snapshot payload layout " + std::to_string(layout) +
                          " is not supported by this build",
                      ErrorDetail{.operation = "decode_snapshot_payload",
                                  .expected = kSnapshotLayoutVersion,
                                  .actual = layout});
  }
  if (rack_count > kMaxRacks) {
    return make_error(ErrorCode::LimitExceeded,
                      "stored rack count " + std::to_string(rack_count) + " exceeds the bound",
                      ErrorDetail{.operation = "decode_snapshot_payload",
                                  .expected = kMaxRacks,
                                  .actual = rack_count});
  }
  if (rack_count > reader.remaining()) {
    return make_error(ErrorCode::TruncatedState,
                      "stored rack count exceeds the bytes that remain",
                      ErrorDetail{.operation = "decode_snapshot_payload",
                                  .actual = rack_count});
  }

  std::vector<RackRecord> racks;
  racks.reserve(rack_count);
  for (std::uint32_t i = 0; i < rack_count; ++i) {
    RackRecord record;
    RackError error;
    if (!read_rack_record(reader, record, error)) {
      return error;
    }
    racks.push_back(std::move(record));
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::CorruptState,
                      "snapshot payload contains trailing bytes after the last rack",
                      ErrorDetail{.operation = "decode_snapshot_payload",
                                  .actual = reader.remaining()});
  }

  std::sort(racks.begin(), racks.end(), [](const RackRecord& a, const RackRecord& b) {
    return a.structure.id < b.structure.id;
  });
  for (std::size_t i = 1; i < racks.size(); ++i) {
    if (racks[i].structure.id == racks[i - 1].structure.id) {
      return make_error(ErrorCode::DuplicateRackId,
                        "rack " + racks[i].structure.id.text() + " appears more than once",
                        ErrorDetail{.operation = "decode_snapshot_payload",
                                    .subject = racks[i].structure.id.text()});
    }
  }
  return racks;
}

}  // namespace internal

// ---------------------------------------------------------------------------
// RackSnapshot encoding and decoding
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> RackSnapshot::to_bytes() const {
  const std::vector<std::uint8_t> payload = internal::encode_snapshot_payload(*this);
  const StateDigest payload_digest = StateDigest::of(payload);

  internal::ByteWriter header;
  header.raw(internal::kFileMagic.data(), internal::kFileMagic.size());
  header.u32(kStateFormatVersion);
  header.u32(internal::kStateEndianTag);
  header.u32(kMountSlotsPerRackUnit);
  header.u32(internal::kPayloadKindSnapshot);
  header.u64(store_epoch_.value());
  header.u64(store_sequence_.value());
  header.u32(kVersionMajor);
  header.u32(kVersionMinor);
  header.u32(kVersionPatch);
  header.u64(payload.size());
  header.digest(payload_digest);
  header.u8(produced_by_.has_value() ? 1u : 0u);
  if (produced_by_.has_value()) {
    header.text(produced_by_->text());
  }
  const StateDigest header_digest = StateDigest::of(header.buffer());
  header.digest(header_digest);

  std::vector<std::uint8_t> image = std::move(header).take();
  image.insert(image.end(), payload.begin(), payload.end());
  image.insert(image.end(), internal::kTrailerMagic.begin(), internal::kTrailerMagic.end());
  return image;
}

RackSnapshot RackSnapshot::with_store_position(StoreEpoch epoch, StoreSequence sequence) const {
  RackSnapshot copy = *this;
  copy.store_epoch_ = epoch;
  copy.store_sequence_ = sequence;
  return copy;
}

Result<RackSnapshot> RackSnapshot::from_bytes(const std::uint8_t* data, std::size_t size) {
  // Size checks come first so that an empty buffer, which may legitimately
  // carry a null data pointer, is reported as a truncated state rather than as
  // a programming error.
  if (size < internal::kFixedHeaderBytes + internal::kTrailerMagic.size()) {
    return make_error(ErrorCode::TruncatedState,
                      "state image is smaller than the smallest valid file",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes",
                                  .expected = internal::kFixedHeaderBytes +
                                              internal::kTrailerMagic.size(),
                                  .actual = size});
  }
  if (size > kMaxStateFileBytes) {
    return make_error(ErrorCode::StateTooLarge, "state image exceeds the accepted bound",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes",
                                  .expected = kMaxStateFileBytes,
                                  .actual = size});
  }
  if (data == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "state image pointer is null",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes", .actual = size});
  }
  for (std::size_t i = 0; i < internal::kFileMagic.size(); ++i) {
    if (data[i] != internal::kFileMagic[i]) {
      return make_error(ErrorCode::CorruptState, "state image does not begin with the expected magic",
                        ErrorDetail{.operation = "RackSnapshot::from_bytes"});
    }
  }

  internal::ByteReader reader(data, size);
  const std::uint8_t* magic = nullptr;
  std::uint32_t format_version = 0;
  std::uint32_t endian_tag = 0;
  std::uint32_t slots_per_unit = 0;
  std::uint32_t payload_kind = 0;
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  std::uint32_t producer_major = 0;
  std::uint32_t producer_minor = 0;
  std::uint32_t producer_patch = 0;
  std::uint64_t payload_length = 0;
  StateDigest payload_digest;
  std::uint8_t has_producer = 0;
  StateDigest header_digest;

  if (!reader.raw(internal::kFileMagic.size(), magic) || !reader.u32(format_version) ||
      !reader.u32(endian_tag) || !reader.u32(slots_per_unit) || !reader.u32(payload_kind) ||
      !reader.u64(epoch) || !reader.u64(sequence) || !reader.u32(producer_major) ||
      !reader.u32(producer_minor) || !reader.u32(producer_patch) || !reader.u64(payload_length) ||
      !reader.digest(payload_digest) || !reader.u8(has_producer)) {
    return make_error(ErrorCode::TruncatedState, "state image header is truncated",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes"});
  }
  (void)magic;
  (void)producer_major;
  (void)producer_minor;
  (void)producer_patch;

  if (format_version != kStateFormatVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "state format version " + std::to_string(format_version) +
                          " is not supported by this build",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes",
                                  .expected = kStateFormatVersion,
                                  .actual = format_version});
  }
  if (endian_tag != internal::kStateEndianTag) {
    return make_error(ErrorCode::CorruptState,
                      "state image declares an unexpected byte-order tag",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes", .actual = endian_tag});
  }
  if (slots_per_unit != kMountSlotsPerRackUnit) {
    return make_error(ErrorCode::UnsupportedCoordinateModel,
                      "state image uses a mounting coordinate model with " +
                          std::to_string(slots_per_unit) + " slots per rack unit",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes",
                                  .expected = kMountSlotsPerRackUnit,
                                  .actual = slots_per_unit});
  }
  if (payload_kind != internal::kPayloadKindSnapshot) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "state image payload kind is not supported by this build",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes",
                                  .expected = internal::kPayloadKindSnapshot,
                                  .actual = payload_kind});
  }

  std::optional<SourceReference> producer;
  if (has_producer > 1) {
    return make_error(ErrorCode::CorruptState, "state image producer flag is not a boolean",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes"});
  }
  if (has_producer == 1) {
    std::string producer_text;
    if (!reader.text(producer_text, kMaxSourceReferenceBytes)) {
      return make_error(ErrorCode::TruncatedState, "state image producer identity is truncated",
                        ErrorDetail{.operation = "RackSnapshot::from_bytes"});
    }
    const auto parsed = SourceReference::parse(producer_text);
    if (!parsed) {
      return make_error(ErrorCode::InvalidStateEncoding,
                        "state image producer identity is invalid: " + parsed.error().message,
                        ErrorDetail{.operation = "RackSnapshot::from_bytes"});
    }
    producer = parsed.value();
  }

  if (!reader.digest(header_digest)) {
    return make_error(ErrorCode::TruncatedState, "state image header digest is truncated",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes"});
  }

  const std::size_t header_length = reader.position();
  // The stored header digest is the last field of the header, so it is excluded
  // from the bytes it covers.
  const StateDigest computed_header_digest =
      StateDigest::of(data, header_length - Sha256::kDigestBytes);
  if (computed_header_digest != header_digest) {
    return make_error(ErrorCode::IntegrityCheckFailed,
                      "state image header integrity check failed",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes"});
  }

  const std::uint64_t available = static_cast<std::uint64_t>(size - header_length);
  const std::uint64_t expected_payload = available >= internal::kTrailerMagic.size()
                                             ? available - internal::kTrailerMagic.size()
                                             : 0;
  if (payload_length != expected_payload) {
    return make_error(ErrorCode::TruncatedState,
                      "state image payload length does not match the bytes that follow the header",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes",
                                  .expected = expected_payload,
                                  .actual = payload_length});
  }

  const std::uint8_t* payload_bytes = data + header_length;
  const StateDigest computed_payload_digest =
      StateDigest::of(payload_bytes, static_cast<std::size_t>(payload_length));
  if (computed_payload_digest != payload_digest) {
    return make_error(ErrorCode::IntegrityCheckFailed, "state payload integrity check failed",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes"});
  }

  for (std::size_t i = 0; i < internal::kTrailerMagic.size(); ++i) {
    if (data[size - internal::kTrailerMagic.size() + i] != internal::kTrailerMagic[i]) {
      return make_error(ErrorCode::CorruptState, "state image is missing its trailer",
                        ErrorDetail{.operation = "RackSnapshot::from_bytes"});
    }
  }

  auto racks = internal::decode_snapshot_payload(payload_bytes,
                                                 static_cast<std::size_t>(payload_length));
  if (!racks) {
    return racks.error();
  }
  const auto epoch_value = StoreEpoch::create(epoch);
  if (!epoch_value) {
    return make_error(ErrorCode::InvalidStateEncoding,
                      "state image store epoch is not a valid epoch",
                      ErrorDetail{.operation = "RackSnapshot::from_bytes", .actual = epoch});
  }

  RackSnapshot snapshot;
  snapshot.racks_ = std::move(racks).value();
  snapshot.store_epoch_ = epoch_value.value();
  snapshot.store_sequence_ = StoreSequence::create(sequence).value();
  snapshot.produced_by_ = producer;

  const Status valid = snapshot.validate();
  if (!valid) {
    return make_error(ErrorCode::SnapshotInvalid,
                      "decoded state violates an invariant: " + valid.error().message,
                      valid.error().detail);
  }
  return snapshot;
}

}  // namespace rackregistry
