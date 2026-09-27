// Rack Registry - internal declarations shared between translation units.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Nothing in this header is installed: it is not part of the public API.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/digest.hpp"
#include "rack_registry/member.hpp"
#include "rack_registry/mount.hpp"
#include "rack_registry/rack.hpp"
#include "rack_registry/result.hpp"

namespace rackregistry {
namespace internal {

// State file identity. These values are part of the format contract: a reader
// rejects a file whose magic, byte-order tag, payload kind or coordinate model
// differs from what this build implements.
inline constexpr std::array<std::uint8_t, 8> kFileMagic = {'R', 'A', 'C', 'K',
                                                           'R', 'E', 'G', 'S'};
inline constexpr std::array<std::uint8_t, 8> kTrailerMagic = {'R', 'A', 'C', 'K',
                                                              'R', 'G', 'E', 'N'};
inline constexpr std::uint32_t kStateEndianTag = 0x01020304u;
inline constexpr std::uint32_t kPayloadKindSnapshot = 1u;

// A member whose mount span cannot coexist with a candidate span.
struct OccupancyConflict {
  RackMemberId member_id{};
  ErrorCode code = ErrorCode::OccupancyOverlap;
  std::string message{};
};

// Checks that a candidate span lies inside the physical extent of the rack.
[[nodiscard]] Status check_mount_bounds(const RackStructure& structure,
                                        const MountSpan& candidate,
                                        std::string_view operation);

// Finds the first member, in member-identity order, whose span cannot coexist
// with `candidate`. `ignore_member` names the member being moved or replaced,
// which never conflicts with itself; pass nullptr when inserting.
[[nodiscard]] Result<std::optional<OccupancyConflict>> find_occupancy_conflict(
    const RackStructure& structure,
    const std::map<RackMemberId, MemberRecord>& members,
    const MountSpan& candidate,
    const RackMemberId* ignore_member,
    std::string_view operation);

// Validates the complete occupancy of a rack: every member in bounds, no
// illegal overlap, shared-mount classes and capacities consistent. Returns the
// first violation in member-identity order.
[[nodiscard]] Status validate_occupancy(const RackStructure& structure,
                                        const std::map<RackMemberId, MemberRecord>& members,
                                        std::string_view operation);

// Validates that a member record is internally well formed and belongs to the
// rack: identity, asset reference, provenance bound, and compatibility against
// the rack profile.
[[nodiscard]] Status validate_member_consistency(const RackStructure& structure,
                                                 const MemberRecord& member,
                                                 std::string_view operation);

// Validates every invariant a RackRecord must satisfy.
[[nodiscard]] Status validate_rack_record(const RackRecord& record, std::string_view operation);

// ---------------------------------------------------------------------------
// Canonical encoding primitives
// ---------------------------------------------------------------------------

// Little-endian byte writer with explicit lengths. Never writes a raw struct.
class ByteWriter {
 public:
  void u8(std::uint8_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void raw(const std::uint8_t* data, std::size_t size);
  void text(std::string_view value);
  void digest(const StateDigest& value);

  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::uint8_t> buffer_{};
};

// Bounds-checked reader. Every method returns false and leaves the output
// untouched when the requested bytes are not available.
class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool raw(std::size_t count, const std::uint8_t*& out) noexcept;
  [[nodiscard]] bool text(std::string& out, std::size_t max_bytes);
  [[nodiscard]] bool digest(StateDigest& out) noexcept;
  [[nodiscard]] bool skip(std::size_t count) noexcept;

  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }
  [[nodiscard]] bool at_end() const noexcept { return position_ == size_; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

// ---------------------------------------------------------------------------
// Canonical record encoding
// ---------------------------------------------------------------------------

// Canonical body of one rack record, used for per-rack and per-generation
// digests. Two records with equal authoritative content encode identically.
[[nodiscard]] std::vector<std::uint8_t> encode_rack_record(const RackRecord& record);

// The authoritative state of a rack: identity, structure, counters, lifecycle
// and membership. History - provenance trails, the generation evidence ring and
// the idempotency table - is deliberately excluded so that the digest is not
// self-referential and so that appending history never changes the digest of
// the state it describes.
void write_rack_state(ByteWriter& writer, const RackRecord& record);

// The complete persisted form of a rack, including history.
void write_rack_record(ByteWriter& writer, const RackRecord& record);

// Reads one rack record. `ok` is set to false when the bytes are malformed,
// truncated or violate a bound; `error` then carries the reason.
[[nodiscard]] bool read_rack_record(ByteReader& reader, RackRecord& record, RackError& error);

// The snapshot payload, without the file header.
[[nodiscard]] std::vector<std::uint8_t> encode_snapshot_payload(const RackSnapshot& snapshot);
[[nodiscard]] Result<std::vector<RackRecord>> decode_snapshot_payload(const std::uint8_t* data,
                                                                     std::size_t size);

// Digest of the authoritative content of one rack record.
[[nodiscard]] StateDigest rack_record_digest(const RackRecord& record);

}  // namespace internal
}  // namespace rackregistry
