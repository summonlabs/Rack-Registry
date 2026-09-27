// Rack Registry - provenance implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/provenance.hpp"

#include <string>

namespace rackregistry {

std::string_view provenance_source_name(ProvenanceSource source) noexcept {
  switch (source) {
    case ProvenanceSource::Registration:
      return "registration";
    case ProvenanceSource::Operator:
      return "operator";
    case ProvenanceSource::Import:
      return "import";
    case ProvenanceSource::Recovery:
      return "recovery";
    case ProvenanceSource::Reconciliation:
      return "reconciliation";
    case ProvenanceSource::System:
      return "system";
  }
  return "unknown";
}

Result<ProvenanceSource> parse_provenance_source(std::string_view text) {
  for (std::size_t i = 0; i < kProvenanceSourceCount; ++i) {
    const auto source = static_cast<ProvenanceSource>(i);
    if (provenance_source_name(source) == text) {
      return source;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown provenance source",
                    ErrorDetail{.operation = "ProvenanceSource", .subject = std::string(text)});
}

Result<ProvenanceSource> provenance_source_from_value(std::uint8_t value) {
  if (value >= kProvenanceSourceCount) {
    return make_error(ErrorCode::InvalidEnumValue, "provenance source value is outside its domain",
                      ErrorDetail{.operation = "ProvenanceSource",
                                  .expected = kProvenanceSourceCount - 1,
                                  .actual = value});
  }
  return static_cast<ProvenanceSource>(value);
}

bool ProvenanceRecord::operator==(const ProvenanceRecord& other) const noexcept {
  return source == other.source && actor == other.actor &&
         source_reference.has_value() == other.source_reference.has_value() &&
         (!source_reference.has_value() || source_reference->text() ==
                                               other.source_reference->text()) &&
         source_sequence == other.source_sequence &&
         observed_at_unix_ns == other.observed_at_unix_ns && note == other.note;
}

std::string ProvenanceRecord::to_text() const {
  std::string out(provenance_source_name(source));
  out.append(" actor=");
  out.append(actor.empty() ? "<none>" : actor.text());
  if (source_reference.has_value()) {
    out.append(" ref=");
    out.append(source_reference->text());
  }
  out.append(" seq=");
  out.append(std::to_string(source_sequence));
  out.append(" at=");
  out.append(std::to_string(observed_at_unix_ns));
  if (!note.empty()) {
    out.append(" note=");
    out.append(note.text());
  }
  return out;
}

Status validate_provenance(const ProvenanceRecord& provenance) {
  if (provenance.actor.empty()) {
    return make_error(ErrorCode::EmptyValue,
                      "provenance must name the actor that requested the mutation",
                      ErrorDetail{.operation = "provenance"});
  }
  return Status{};
}

}  // namespace rackregistry
