// Rack Registry - documented resource bounds.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>

#include "rack_registry/export.hpp"
#include "rack_registry/version.hpp"

namespace rackregistry {

// Every bound below is enforced before allocation or mutation, on both the
// in-memory path and the decoding path, so a hostile state file cannot drive
// unbounded growth. Changing a bound changes what a valid registry may hold;
// the values are part of the documented contract and are asserted by tests.

// Maximum rack units in one rack. 1024 units is far beyond any physical rack
// enclosure; the bound exists to keep coordinate arithmetic and payload sizes
// finite.
inline constexpr std::uint32_t kMaxRackUnits = 1024;

// Maximum mount slot index (exclusive upper bound) for one rack. Slot numbers
// are 1-based and half-open ranges must satisfy end <= kMaxMountSlotExclusive.
inline constexpr std::uint32_t kMaxMountSlotExclusive =
    kMaxRackUnits * kMountSlotsPerRackUnit + 1;

// Maximum racks held by one registry.
inline constexpr std::size_t kMaxRacks = 4096;

// Maximum members held by one rack.
inline constexpr std::size_t kMaxMembersPerRack = 4096;

// Maximum power-domain or cooling-domain associations on one rack. The bound is
// applied to each association list independently.
inline constexpr std::size_t kMaxDomainReferencesPerRack = 64;

// Maximum traits in one compatibility profile or one member requirement set.
inline constexpr std::size_t kMaxTraitsPerSet = 256;

// Maximum number of retained previous generations kept in memory for one rack,
// used by generation diffing. Persisted generation evidence keeps its own,
// separately bounded ring of the same size. Retaining a previous generation
// costs a full copy of that rack's membership, so this bound is deliberately
// small.
inline constexpr std::size_t kMaxRetainedGenerationsPerRack = 8;

// Maximum number of recorded idempotency receipts kept per rack. The oldest
// receipt is evicted first; the eviction count is itself preserved so that
// operators can see that replay coverage is bounded.
inline constexpr std::size_t kMaxIdempotencyRecordsPerRack = 64;

// Maximum provenance records retained per rack and per member.
inline constexpr std::size_t kMaxProvenanceRecordsPerRack = 256;
inline constexpr std::size_t kMaxProvenanceRecordsPerMember = 16;

// Maximum rejection explanations retained by one registry for inspection.
inline constexpr std::size_t kMaxRejectionJournalEntries = 512;

// Text bounds, in bytes, of externally supplied strings.
inline constexpr std::size_t kMaxIdentityTextBytes = 192;
inline constexpr std::size_t kMaxOpaqueReferenceBytes = 160;
inline constexpr std::size_t kMaxTraitBytes = 48;
inline constexpr std::size_t kMaxActorBytes = 64;
inline constexpr std::size_t kMaxRequestIdBytes = 64;
inline constexpr std::size_t kMaxSourceReferenceBytes = 160;
inline constexpr std::size_t kMaxLabelBytes = 128;
inline constexpr std::size_t kMaxNoteBytes = 160;

// Maximum byte length of one encoded state file that a reader will accept. The
// declared payload length is checked against this bound before any buffer is
// reserved.
inline constexpr std::uint64_t kMaxStateFileBytes = 512ull * 1024ull * 1024ull;

// Maximum accepted unit count in one decoded trait set or collection header.
// Decoding rejects a declared count above the matching bound before growing a
// container.
inline constexpr std::uint32_t kMaxDecodedCollectionCount = 1u << 20;

}  // namespace rackregistry
