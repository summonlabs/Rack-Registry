// Rack Registry - provenance of authoritative state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/result.hpp"

namespace rackregistry {

// Where an authoritative change came from. The source is part of the record,
// not a log line: it survives restart and is serialized with the state.
enum class ProvenanceSource : std::uint8_t {
  Registration = 0,
  Operator = 1,
  Import = 2,
  Recovery = 3,
  Reconciliation = 4,
  System = 5,
};

inline constexpr std::size_t kProvenanceSourceCount = 6;

[[nodiscard]] RACK_REGISTRY_API std::string_view provenance_source_name(
    ProvenanceSource source) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<ProvenanceSource> parse_provenance_source(
    std::string_view text);
[[nodiscard]] RACK_REGISTRY_API Result<ProvenanceSource> provenance_source_from_value(
    std::uint8_t value);

// One provenance record. The library never reads a wall clock: the caller
// supplies `observed_at_unix_ns`, which keeps mutation outcomes reproducible
// in tests and makes the recorded time an explicit part of the claim.
struct ProvenanceRecord {
  ProvenanceSource source = ProvenanceSource::Operator;
  ActorId actor{};
  std::optional<SourceReference> source_reference{};
  std::uint64_t source_sequence = 0;
  std::uint64_t observed_at_unix_ns = 0;
  Note note{};

  [[nodiscard]] bool operator==(const ProvenanceRecord& other) const noexcept;
  [[nodiscard]] bool operator!=(const ProvenanceRecord& other) const noexcept {
    return !(*this == other);
  }

  // Compact human-readable rendering used by the inspection tooling.
  [[nodiscard]] std::string to_text() const;
};

// Every mutation must carry a provenance record naming the actor that
// requested it, so that authoritative state can always be attributed.
[[nodiscard]] RACK_REGISTRY_API Status validate_provenance(const ProvenanceRecord& provenance);

}  // namespace rackregistry
