// Rack Registry - strongly typed identities, generations and epochs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "rack_registry/export.hpp"
#include "rack_registry/limits.hpp"
#include "rack_registry/result.hpp"
#include "rack_registry/text.hpp"

namespace rackregistry {

namespace detail {

[[nodiscard]] constexpr bool ident_first(char c) noexcept { return ascii_alphanumeric(c); }
[[nodiscard]] constexpr bool ident_rest(char c) noexcept {
  return ascii_alphanumeric(c) || c == '.' || c == '_' || c == '-' || c == ':';
}
[[nodiscard]] constexpr bool opaque_rest(char c) noexcept {
  return ascii_alphanumeric(c) || c == '.' || c == '_' || c == '-' || c == ':' || c == '/' ||
         c == '@' || c == '+';
}
[[nodiscard]] constexpr bool trait_first(char c) noexcept { return ascii_lower_alphanumeric(c); }
[[nodiscard]] constexpr bool trait_rest(char c) noexcept {
  return ascii_lower_alphanumeric(c) || c == '.' || c == '_' || c == '-';
}

// Shared implementation of "a canonical text value with a policy". The derived
// type is final, so no further type can inherit a constructor from it.
template <typename Derived, typename Policy>
class CanonicalText {
 public:
  static Result<Derived> parse(std::string_view text) {
    if (text.empty()) {
      return make_error(ErrorCode::EmptyValue,
                        std::string(Policy::kind) + " must not be empty",
                        ErrorDetail{.operation = std::string(Policy::kind)});
    }
    if (text.size() > Policy::max_bytes) {
      return make_error(ErrorCode::IdentityTooLong,
                        std::string(Policy::kind) + " exceeds " +
                            std::to_string(Policy::max_bytes) + " bytes",
                        ErrorDetail{.operation = std::string(Policy::kind),
                                    .expected = Policy::max_bytes,
                                    .actual = text.size()});
    }

    std::string_view body = text;
    if constexpr (!Policy::prefix.empty()) {
      if (body.size() < Policy::prefix.size() ||
          body.substr(0, Policy::prefix.size()) != Policy::prefix) {
        return make_error(ErrorCode::MalformedIdentity,
                          std::string(Policy::kind) + " must begin with '" +
                              std::string(Policy::prefix) + "'",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
      body.remove_prefix(Policy::prefix.size());
    }
    if (body.empty()) {
      return make_error(ErrorCode::EmptyValue,
                        std::string(Policy::kind) + " has an empty body",
                        ErrorDetail{.operation = std::string(Policy::kind),
                                    .subject = std::string(text)});
    }
    if (body.size() > Policy::max_body_bytes) {
      return make_error(ErrorCode::IdentityTooLong,
                        std::string(Policy::kind) + " body exceeds " +
                            std::to_string(Policy::max_body_bytes) + " bytes",
                        ErrorDetail{.operation = std::string(Policy::kind),
                                    .expected = Policy::max_body_bytes,
                                    .actual = body.size()});
    }

    if constexpr (Policy::free_text) {
      if (!is_valid_utf8(body)) {
        return make_error(ErrorCode::InvalidUtf8,
                          std::string(Policy::kind) + " is not valid UTF-8",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
      if (!has_no_control_characters(body)) {
        return make_error(ErrorCode::InvalidCharacter,
                          std::string(Policy::kind) + " contains a control character",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
    } else {
      if (!Policy::first_char_ok(body.front())) {
        return make_error(ErrorCode::InvalidCharacter,
                          std::string(Policy::kind) + " starts with a character outside its domain",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
      for (const char c : body.substr(1)) {
        if (!Policy::rest_char_ok(c)) {
          return make_error(ErrorCode::InvalidCharacter,
                            std::string(Policy::kind) + " contains a character outside its domain",
                            ErrorDetail{.operation = std::string(Policy::kind),
                                        .subject = std::string(text)});
        }
      }
    }

    return Derived{std::string(text)};
  }

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  [[nodiscard]] bool operator==(const CanonicalText& other) const noexcept {
    return text_ == other.text_;
  }
  [[nodiscard]] bool operator!=(const CanonicalText& other) const noexcept {
    return text_ != other.text_;
  }
  [[nodiscard]] bool operator<(const CanonicalText& other) const noexcept {
    return byte_less(text_, other.text_);
  }
  [[nodiscard]] bool operator<=(const CanonicalText& other) const noexcept {
    return !(other < *this);
  }
  [[nodiscard]] bool operator>(const CanonicalText& other) const noexcept { return other < *this; }
  [[nodiscard]] bool operator>=(const CanonicalText& other) const noexcept { return !(*this < other); }

 protected:
  CanonicalText() = default;
  explicit CanonicalText(std::string text) : text_(std::move(text)) {}
  std::string text_;
};

struct RackIdPolicy {
  static constexpr std::string_view kind = "RackId";
  static constexpr std::string_view prefix = "rack:";
  static constexpr std::size_t max_bytes = kMaxIdentityTextBytes;
  static constexpr std::size_t max_body_bytes = 96;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = ident_rest;
};

struct RackMemberIdPolicy {
  static constexpr std::string_view kind = "RackMemberId";
  static constexpr std::string_view prefix = "rm:";
  static constexpr std::size_t max_bytes = kMaxIdentityTextBytes;
  static constexpr std::size_t max_body_bytes = 96;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = ident_rest;
};

struct CompatibilityProfileIdPolicy {
  static constexpr std::string_view kind = "CompatibilityProfileId";
  static constexpr std::string_view prefix = "cp:";
  static constexpr std::size_t max_bytes = kMaxIdentityTextBytes;
  static constexpr std::size_t max_body_bytes = 96;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = ident_rest;
};

struct SharedMountClassPolicy {
  static constexpr std::string_view kind = "SharedMountClass";
  static constexpr std::string_view prefix = "smc:";
  static constexpr std::size_t max_bytes = kMaxIdentityTextBytes;
  static constexpr std::size_t max_body_bytes = 96;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = ident_rest;
};

struct WriterIdPolicy {
  static constexpr std::string_view kind = "WriterId";
  static constexpr std::string_view prefix = "wr:";
  static constexpr std::size_t max_bytes = kMaxIdentityTextBytes;
  static constexpr std::size_t max_body_bytes = 64;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = ident_rest;
};

struct RequestIdPolicy {
  static constexpr std::string_view kind = "RequestId";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxRequestIdBytes;
  static constexpr std::size_t max_body_bytes = kMaxRequestIdBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = ident_rest;
};

struct AssetIdPolicy {
  static constexpr std::string_view kind = "AssetId";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxOpaqueReferenceBytes;
  static constexpr std::size_t max_body_bytes = kMaxOpaqueReferenceBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

struct PowerDomainReferencePolicy {
  static constexpr std::string_view kind = "PowerDomainReference";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxOpaqueReferenceBytes;
  static constexpr std::size_t max_body_bytes = kMaxOpaqueReferenceBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

struct CoolingDomainReferencePolicy {
  static constexpr std::string_view kind = "CoolingDomainReference";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxOpaqueReferenceBytes;
  static constexpr std::size_t max_body_bytes = kMaxOpaqueReferenceBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

struct ActorIdPolicy {
  static constexpr std::string_view kind = "ActorId";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxActorBytes;
  static constexpr std::size_t max_body_bytes = kMaxActorBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

struct SourceReferencePolicy {
  static constexpr std::string_view kind = "SourceReference";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxSourceReferenceBytes;
  static constexpr std::size_t max_body_bytes = kMaxSourceReferenceBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

struct TraitPolicy {
  static constexpr std::string_view kind = "Trait";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxTraitBytes;
  static constexpr std::size_t max_body_bytes = kMaxTraitBytes;
  static constexpr bool free_text = false;
  static constexpr auto first_char_ok = trait_first;
  static constexpr auto rest_char_ok = trait_rest;
};

struct DisplayLabelPolicy {
  static constexpr std::string_view kind = "DisplayLabel";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxLabelBytes;
  static constexpr std::size_t max_body_bytes = kMaxLabelBytes;
  static constexpr bool free_text = true;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

struct NotePolicy {
  static constexpr std::string_view kind = "Note";
  static constexpr std::string_view prefix = "";
  static constexpr std::size_t max_bytes = kMaxNoteBytes;
  static constexpr std::size_t max_body_bytes = kMaxNoteBytes;
  static constexpr bool free_text = true;
  static constexpr auto first_char_ok = ident_first;
  static constexpr auto rest_char_ok = opaque_rest;
};

}  // namespace detail

// Canonical rack identity. Text form is "rack:" followed by 1..96 characters
// from [A-Za-z0-9._:-] beginning with an alphanumeric character. Comparison and
// ordering are byte-wise over the full text.
class RackId final : public detail::CanonicalText<RackId, detail::RackIdPolicy> {
 private:
  friend class detail::CanonicalText<RackId, detail::RackIdPolicy>;
  using Base = detail::CanonicalText<RackId, detail::RackIdPolicy>;
  explicit RackId(std::string text) : Base(std::move(text)) {}

 public:
  RackId() = default;
};

// Canonical identity of a membership record inside one rack, text form "rm:...".
class RackMemberId final : public detail::CanonicalText<RackMemberId, detail::RackMemberIdPolicy> {
 private:
  friend class detail::CanonicalText<RackMemberId, detail::RackMemberIdPolicy>;
  using Base = detail::CanonicalText<RackMemberId, detail::RackMemberIdPolicy>;
  explicit RackMemberId(std::string text) : Base(std::move(text)) {}

 public:
  RackMemberId() = default;
};

// Canonical identity of a compatibility profile, text form "cp:...".
class CompatibilityProfileId final
    : public detail::CanonicalText<CompatibilityProfileId, detail::CompatibilityProfileIdPolicy> {
 private:
  friend class detail::CanonicalText<CompatibilityProfileId, detail::CompatibilityProfileIdPolicy>;
  using Base = detail::CanonicalText<CompatibilityProfileId, detail::CompatibilityProfileIdPolicy>;
  explicit CompatibilityProfileId(std::string text) : Base(std::move(text)) {}

 public:
  CompatibilityProfileId() = default;
};

// Canonical identity of a shared-mount class, text form "smc:...". Members that
// declare the same shared-mount class and the exact same mount span may
// legally co-occupy that span up to the declared capacity.
class SharedMountClass final
    : public detail::CanonicalText<SharedMountClass, detail::SharedMountClassPolicy> {
 private:
  friend class detail::CanonicalText<SharedMountClass, detail::SharedMountClassPolicy>;
  using Base = detail::CanonicalText<SharedMountClass, detail::SharedMountClassPolicy>;
  explicit SharedMountClass(std::string text) : Base(std::move(text)) {}

 public:
  SharedMountClass() = default;
};

// Canonical identity of a writer incarnation, text form "wr:...". A writer
// identity names the process-family that may publish durable state.
class WriterId final : public detail::CanonicalText<WriterId, detail::WriterIdPolicy> {
 private:
  friend class detail::CanonicalText<WriterId, detail::WriterIdPolicy>;
  using Base = detail::CanonicalText<WriterId, detail::WriterIdPolicy>;
  explicit WriterId(std::string text) : Base(std::move(text)) {}

 public:
  WriterId() = default;
};

// Caller-supplied identity of one mutation request, used for bounded
// idempotency. It is opaque to the library.
class RequestId final : public detail::CanonicalText<RequestId, detail::RequestIdPolicy> {
 private:
  friend class detail::CanonicalText<RequestId, detail::RequestIdPolicy>;
  using Base = detail::CanonicalText<RequestId, detail::RequestIdPolicy>;
  explicit RequestId(std::string text) : Base(std::move(text)) {}

 public:
  RequestId() = default;
};

// Opaque reference to an asset owned by another runtime (Asset Registry). Rack
// Registry never interprets this value; it preserves it byte for byte and never
// reimplements asset semantics.
class AssetId final : public detail::CanonicalText<AssetId, detail::AssetIdPolicy> {
 private:
  friend class detail::CanonicalText<AssetId, detail::AssetIdPolicy>;
  using Base = detail::CanonicalText<AssetId, detail::AssetIdPolicy>;
  explicit AssetId(std::string text) : Base(std::move(text)) {}

 public:
  AssetId() = default;
};

// Typed reference to an external power domain. The type is distinct from
// CoolingDomainReference: the two can never be interchanged or compared.
class PowerDomainReference final
    : public detail::CanonicalText<PowerDomainReference, detail::PowerDomainReferencePolicy> {
 private:
  friend class detail::CanonicalText<PowerDomainReference, detail::PowerDomainReferencePolicy>;
  using Base = detail::CanonicalText<PowerDomainReference, detail::PowerDomainReferencePolicy>;
  explicit PowerDomainReference(std::string text) : Base(std::move(text)) {}

 public:
  PowerDomainReference() = default;
};

// Typed reference to an external cooling domain.
class CoolingDomainReference final
    : public detail::CanonicalText<CoolingDomainReference, detail::CoolingDomainReferencePolicy> {
 private:
  friend class detail::CanonicalText<CoolingDomainReference, detail::CoolingDomainReferencePolicy>;
  using Base = detail::CanonicalText<CoolingDomainReference, detail::CoolingDomainReferencePolicy>;
  explicit CoolingDomainReference(std::string text) : Base(std::move(text)) {}

 public:
  CoolingDomainReference() = default;
};

// Identity of one that performed a mutation, for provenance.
class ActorId final : public detail::CanonicalText<ActorId, detail::ActorIdPolicy> {
 private:
  friend class detail::CanonicalText<ActorId, detail::ActorIdPolicy>;
  using Base = detail::CanonicalText<ActorId, detail::ActorIdPolicy>;
  explicit ActorId(std::string text) : Base(std::move(text)) {}

 public:
  ActorId() = default;
};

// Opaque reference to the upstream record a mutation came from.
class SourceReference final
    : public detail::CanonicalText<SourceReference, detail::SourceReferencePolicy> {
 private:
  friend class detail::CanonicalText<SourceReference, detail::SourceReferencePolicy>;
  using Base = detail::CanonicalText<SourceReference, detail::SourceReferencePolicy>;
  explicit SourceReference(std::string text) : Base(std::move(text)) {}

 public:
  SourceReference() = default;
};

// A lowercase compatibility trait, for example "power.ac.208v" or
// "rail.depth.800mm". Traits are the vocabulary of compatibility checks.
class Trait final : public detail::CanonicalText<Trait, detail::TraitPolicy> {
 private:
  friend class detail::CanonicalText<Trait, detail::TraitPolicy>;
  using Base = detail::CanonicalText<Trait, detail::TraitPolicy>;
  explicit Trait(std::string text) : Base(std::move(text)) {}

 public:
  Trait() = default;
};

// Human-facing label. Free UTF-8 text without control characters; it never
// participates in identity.
class DisplayLabel final : public detail::CanonicalText<DisplayLabel, detail::DisplayLabelPolicy> {
 private:
  friend class detail::CanonicalText<DisplayLabel, detail::DisplayLabelPolicy>;
  using Base = detail::CanonicalText<DisplayLabel, detail::DisplayLabelPolicy>;
  explicit DisplayLabel(std::string text) : Base(std::move(text)) {}

 public:
  DisplayLabel() = default;
  // An empty label is a valid label and means "no label". The generic parser
  // rejects empty text, so this factory exists for that one case.
  [[nodiscard]] static DisplayLabel none() noexcept { return DisplayLabel{}; }
  [[nodiscard]] static Result<DisplayLabel> parse_or_none(std::string_view text) {
    if (text.empty()) {
      return DisplayLabel{};
    }
    return parse(text);
  }
};

// Free-text note attached to provenance. Empty is valid.
class Note final : public detail::CanonicalText<Note, detail::NotePolicy> {
 private:
  friend class detail::CanonicalText<Note, detail::NotePolicy>;
  using Base = detail::CanonicalText<Note, detail::NotePolicy>;
  explicit Note(std::string text) : Base(std::move(text)) {}

 public:
  Note() = default;
  [[nodiscard]] static Result<Note> parse_or_none(std::string_view text) {
    if (text.empty()) {
      return Note{};
    }
    return parse(text);
  }
};

// Monotonic counter base. The derived type is final and its value constructor
// is private, so the only ways to obtain one are the named factories below.
template <typename Derived>
class CounterValue {
 public:
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_max() const noexcept {
    return value_ == static_cast<std::uint64_t>(-1);
  }
  // Saturating successor. Callers that must reject exhaustion check is_max()
  // first; the registry does exactly that and reports LimitExceeded.
  [[nodiscard]] constexpr Derived next() const noexcept {
    return is_max() ? Derived{value_} : Derived{value_ + 1};
  }

  [[nodiscard]] constexpr bool operator==(const CounterValue& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const CounterValue& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const CounterValue& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const CounterValue& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const CounterValue& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const CounterValue& other) const noexcept {
    return value_ >= other.value_;
  }

 protected:
  constexpr CounterValue() noexcept = default;
  constexpr explicit CounterValue(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

// Generation of a whole rack record. Every accepted mutation of a rack
// advances it by exactly one. It starts at 1 when the rack is registered.
class RackGeneration final : public CounterValue<RackGeneration> {
 private:
  friend class CounterValue<RackGeneration>;
  constexpr explicit RackGeneration(std::uint64_t value) noexcept
      : CounterValue<RackGeneration>(value) {}

 public:
  constexpr RackGeneration() noexcept = default;
  [[nodiscard]] static constexpr RackGeneration initial() noexcept { return RackGeneration{1}; }
  [[nodiscard]] static Result<RackGeneration> create(std::uint64_t value) {
    if (value == 0) {
      return make_error(ErrorCode::InvalidRange, "RackGeneration must be at least 1",
                        ErrorDetail{.operation = "RackGeneration", .expected = 1, .actual = value});
    }
    return RackGeneration{value};
  }
};

// Revision of a rack's structural definition. It advances only when the
// structure changes (extent, domain associations, compatibility profile,
// label), never for membership-only or lifecycle-only mutations.
class RackRevision final : public CounterValue<RackRevision> {
 private:
  friend class CounterValue<RackRevision>;
  constexpr explicit RackRevision(std::uint64_t value) noexcept
      : CounterValue<RackRevision>(value) {}

 public:
  constexpr RackRevision() noexcept = default;
  [[nodiscard]] static constexpr RackRevision initial() noexcept { return RackRevision{1}; }
  [[nodiscard]] static Result<RackRevision> create(std::uint64_t value) {
    if (value == 0) {
      return make_error(ErrorCode::InvalidRange, "RackRevision must be at least 1",
                        ErrorDetail{.operation = "RackRevision", .expected = 1, .actual = value});
    }
    return RackRevision{value};
  }
};

// Generation of a rack's membership. It starts at 0 for a rack whose
// membership has never been mutated and advances by exactly one per accepted
// membership mutation.
class MembershipGeneration final : public CounterValue<MembershipGeneration> {
 private:
  friend class CounterValue<MembershipGeneration>;
  constexpr explicit MembershipGeneration(std::uint64_t value) noexcept
      : CounterValue<MembershipGeneration>(value) {}

 public:
  constexpr MembershipGeneration() noexcept = default;
  [[nodiscard]] static constexpr MembershipGeneration initial() noexcept {
    return MembershipGeneration{0};
  }
  [[nodiscard]] static Result<MembershipGeneration> create(std::uint64_t value) {
    return MembershipGeneration{value};
  }
};

// Ownership epoch of the durable store. It advances when writer authority is
// taken over, so that a writer holding an older epoch is fenced out.
class StoreEpoch final : public CounterValue<StoreEpoch> {
 private:
  friend class CounterValue<StoreEpoch>;
  constexpr explicit StoreEpoch(std::uint64_t value) noexcept : CounterValue<StoreEpoch>(value) {}

 public:
  constexpr StoreEpoch() noexcept = default;
  [[nodiscard]] static constexpr StoreEpoch initial() noexcept { return StoreEpoch{1}; }
  [[nodiscard]] static Result<StoreEpoch> create(std::uint64_t value) {
    if (value == 0) {
      return make_error(ErrorCode::InvalidRange, "StoreEpoch must be at least 1",
                        ErrorDetail{.operation = "StoreEpoch", .expected = 1, .actual = value});
    }
    return StoreEpoch{value};
  }
};

// Publication sequence of the durable store. It advances by exactly one for
// every atomically published generation of state.
class StoreSequence final : public CounterValue<StoreSequence> {
 private:
  friend class CounterValue<StoreSequence>;
  constexpr explicit StoreSequence(std::uint64_t value) noexcept
      : CounterValue<StoreSequence>(value) {}

 public:
  constexpr StoreSequence() noexcept = default;
  [[nodiscard]] static constexpr StoreSequence initial() noexcept { return StoreSequence{0}; }
  [[nodiscard]] static Result<StoreSequence> create(std::uint64_t value) {
    return StoreSequence{value};
  }
};

}  // namespace rackregistry
