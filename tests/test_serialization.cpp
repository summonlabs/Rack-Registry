// Rack Registry - canonical serialization and integrity tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

// State file header offsets. These are part of the documented format contract
// and are asserted by the tests below, so a layout change cannot pass silently.
constexpr std::size_t kOffsetFormatVersion = 8;
constexpr std::size_t kOffsetEndianTag = 12;
constexpr std::size_t kOffsetSlotsPerUnit = 16;
constexpr std::size_t kOffsetPayloadKind = 20;
constexpr std::size_t kOffsetPayloadLength = 52;
constexpr std::size_t kOffsetPayloadDigest = 60;
constexpr std::size_t kOffsetHasProducer = 92;
constexpr std::size_t kHeaderBytesWithoutProducer = 125;
constexpr std::size_t kOffsetPayload = kHeaderBytesWithoutProducer;
constexpr std::size_t kOffsetPayloadLayout = kOffsetPayload;
constexpr std::size_t kOffsetPayloadRackCount = kOffsetPayload + 4;

void write_le32(std::vector<std::uint8_t>& image, std::size_t offset, std::uint32_t value) {
  image[offset] = static_cast<std::uint8_t>(value);
  image[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  image[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  image[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void write_le64(std::vector<std::uint8_t>& image, std::size_t offset, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    image[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
}

std::uint32_t read_le32(const std::vector<std::uint8_t>& image, std::size_t offset) {
  return static_cast<std::uint32_t>(image[offset]) |
         (static_cast<std::uint32_t>(image[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(image[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(image[offset + 3]) << 24);
}

// Recomputes the header digest and the payload digest after a header field was
// patched, so a test can isolate exactly one format check. The images built by
// these tests carry no producer identity, which is what makes the header length
// a fixed value; state_image_layout_matches_the_documented_contract pins that
// layout, so using the constant here cannot drift silently.
//
// When `restore_length` is false the declared payload length is left exactly as
// the test wrote it, so the length check itself is what has to reject the file.
void reseal_header(std::vector<std::uint8_t>& image, bool restore_length = true) {
  constexpr std::size_t header_length = kHeaderBytesWithoutProducer;
  if (image.size() <= header_length + 8) {
    return;
  }
  const std::size_t payload_length = image.size() - header_length - 8;
  if (restore_length) {
    write_le64(image, kOffsetPayloadLength, payload_length);
  }
  const auto payload_digest = sha256(image.data() + header_length, payload_length);
  std::copy(payload_digest.begin(), payload_digest.end(), image.begin() + kOffsetPayloadDigest);
  const auto header_digest = sha256(image.data(), header_length - Sha256::kDigestBytes);
  std::copy(header_digest.begin(), header_digest.end(),
            image.begin() + static_cast<std::ptrdiff_t>(header_length - Sha256::kDigestBytes));
}

RegisterRackRequest sample_registration(std::string_view body, std::uint32_t units) {
  RegisterRackRequest request;
  request.structure.id = RackId::parse("rack:" + std::string(body)).value();
  request.structure.unit_count = units;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:serial").value();
  request.structure.profile.provides = TraitSet::parse("power.ac.208v,rail.depth.800mm").value();
  request.structure.power_domains = {PowerDomainReference::parse("pdu-a/feed-1").value()};
  request.structure.cooling_domains = {CoolingDomainReference::parse("crac-3/loop-b").value()};
  request.structure.label = DisplayLabel::parse("Serial test").value();
  request.identity.provenance = rrtest::provenance_for("serial", 1);
  return request;
}

RackSnapshot build_snapshot() {
  RackRegistry registry;
  const auto registered = registry.register_rack(sample_registration("serial-a01", 6));
  if (!registered) {
    return RackSnapshot{};
  }
  RackGeneration generation = registered.value().generation;
  MembershipGeneration membership = registered.value().membership_generation;
  for (int index = 0; index < 3; ++index) {
    InsertMemberRequest insert;
    insert.rack_id = RackId::parse("rack:serial-a01").value();
    insert.precondition.expected_generation = generation;
    insert.precondition.expected_membership_generation = membership;
    insert.member_id =
        RackMemberId::parse("rm:serial-" + std::to_string(index)).value();
    insert.asset_id = AssetId::parse("asset:serial-" + std::to_string(index)).value();
    insert.mount =
        MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(
                                                       static_cast<std::uint32_t>(index) + 1)
                                                       .value())
                                  .value())
            .value();
    insert.requirements.required = TraitSet::parse("power.ac.208v").value();
    insert.identity.provenance = rrtest::provenance_for("serial", 2);
    const auto receipt = registry.insert_member(insert);
    if (!receipt) {
      return RackSnapshot{};
    }
    generation = receipt.value().generation;
    membership = receipt.value().membership_generation;
  }
  return registry.snapshot(std::nullopt);
}

std::vector<std::uint8_t> image_of(const RackSnapshot& snapshot) { return snapshot.to_bytes(); }

}  // namespace

RR_TEST(state_image_layout_matches_the_documented_contract) {
  const RackSnapshot snapshot = build_snapshot();
  RR_REQUIRE_OK(snapshot.validate());
  const std::vector<std::uint8_t> image = image_of(snapshot);
  RR_REQUIRE(image.size() > kHeaderBytesWithoutProducer + 8);

  // Magic, format version, byte-order tag, coordinate model and payload kind.
  const std::string magic(reinterpret_cast<const char*>(image.data()), 8);
  RR_CHECK_EQ(magic, std::string("RACKREGS"));
  RR_CHECK_EQ(read_le32(image, kOffsetFormatVersion), kStateFormatVersion);
  RR_CHECK_EQ(read_le32(image, kOffsetEndianTag), 0x01020304u);
  RR_CHECK_EQ(read_le32(image, kOffsetSlotsPerUnit), kMountSlotsPerRackUnit);
  RR_CHECK_EQ(read_le32(image, kOffsetPayloadKind), 1u);
  RR_CHECK_EQ(image[kOffsetHasProducer], 0u);
  RR_CHECK_EQ(read_le32(image, kOffsetPayloadLayout), kSnapshotLayoutVersion);
  RR_CHECK_EQ(read_le32(image, kOffsetPayloadRackCount), 1u);

  // Trailer.
  const std::string trailer(reinterpret_cast<const char*>(image.data() + image.size() - 8), 8);
  RR_CHECK_EQ(trailer, std::string("RACKRGEN"));

  // Payload length must describe exactly the bytes between header and trailer.
  const std::uint64_t declared =
      static_cast<std::uint64_t>(read_le32(image, kOffsetPayloadLength)) |
      (static_cast<std::uint64_t>(read_le32(image, kOffsetPayloadLength + 4)) << 32);
  RR_CHECK_EQ(declared, static_cast<std::uint64_t>(image.size() - kOffsetPayload - 8));

  // The header digest covers the header up to and including the producer flag.
  const auto header_digest = sha256(image.data(), kOffsetHasProducer + 1);
  RR_CHECK(std::equal(header_digest.begin(), header_digest.end(),
                      image.begin() + static_cast<std::ptrdiff_t>(kOffsetHasProducer + 1)));

  // The payload digest covers the payload bytes.
  const auto payload_digest = sha256(image.data() + kOffsetPayload,
                                     static_cast<std::size_t>(declared));
  RR_CHECK(std::equal(payload_digest.begin(), payload_digest.end(),
                      image.begin() + kOffsetPayloadDigest));
}

RR_TEST(snapshot_round_trips_bit_for_bit) {
  const RackSnapshot snapshot = build_snapshot();
  RR_REQUIRE_OK(snapshot.validate());
  const std::vector<std::uint8_t> image = image_of(snapshot);

  const auto decoded = RackSnapshot::from_bytes(image.data(), image.size());
  RR_REQUIRE_OK(decoded);
  RR_CHECK_EQ(decoded.value().state_digest(), snapshot.state_digest());
  RR_CHECK_EQ(decoded.value().rack_count(), snapshot.rack_count());
  RR_CHECK_EQ(decoded.value().member_count(), snapshot.member_count());
  RR_CHECK(!decoded.value().produced_by().has_value());

  // Re-encoding is byte identical, so the encoding is genuinely canonical.
  const std::vector<std::uint8_t> again = image_of(decoded.value());
  RR_CHECK_EQ(again.size(), image.size());
  RR_CHECK(std::equal(again.begin(), again.end(), image.begin()));

  // Every rack record survives with its counters and membership intact.
  const std::vector<RackRecord>& racks = decoded.value().racks();
  RR_REQUIRE(racks.size() == 1);
  RR_CHECK_EQ(racks[0].members.size(), std::size_t{3});
  RR_CHECK_EQ(racks[0].structure.label.text(), std::string("Serial test"));
  RR_CHECK_EQ(racks[0].structure.power_domains.size(), std::size_t{1});
  RR_CHECK_EQ(racks[0].generation.value(), 4u);
  RR_CHECK_EQ(racks[0].membership_generation.value(), 3u);
}

RR_TEST(a_producer_identity_is_part_of_the_image_and_the_digest) {
  const RackSnapshot without = build_snapshot();
  RackRegistry registry;
  RR_REQUIRE_OK(registry.restore(without));
  const RackSnapshot with = registry.snapshot(SourceReference::parse("producer/1.0.0").value());
  const std::vector<std::uint8_t> image = image_of(with);
  RR_CHECK_EQ(image[kOffsetHasProducer], 1u);

  const auto decoded = RackSnapshot::from_bytes(image.data(), image.size());
  RR_REQUIRE_OK(decoded);
  RR_CHECK(decoded.value().produced_by().has_value());
  RR_CHECK_EQ(decoded.value().produced_by()->text(), std::string("producer/1.0.0"));
  RR_CHECK_NE(decoded.value().state_digest(), without.state_digest());

  // Rack-level digests are unaffected by the producer identity, because the
  // producer describes the publication, not the facility state.
  RR_CHECK_EQ(decoded.value().racks()[0].generation.value(),
              without.racks()[0].generation.value());
  RackView left(decoded.value().racks()[0]);
  RackView right(without.racks()[0]);
  RR_CHECK_EQ(left.state_digest(), right.state_digest());
}

RR_TEST(truncated_images_are_rejected) {
  const RackSnapshot snapshot = build_snapshot();
  const std::vector<std::uint8_t> image = image_of(snapshot);

  for (const std::size_t length :
       {static_cast<std::size_t>(0), static_cast<std::size_t>(8), static_cast<std::size_t>(64),
        kHeaderBytesWithoutProducer, kHeaderBytesWithoutProducer + 1, image.size() - 9,
        image.size() - 1}) {
    const auto decoded = RackSnapshot::from_bytes(image.data(), length);
    RR_CHECK(!decoded.has_value());
  }

  // An empty buffer may legitimately carry a null pointer; that is a truncated
  // state. A null pointer with a non-zero length is a programming error.
  RR_REQUIRE_CODE(RackSnapshot::from_bytes(nullptr, 0), ErrorCode::TruncatedState);
  RR_REQUIRE_CODE(RackSnapshot::from_bytes(nullptr, 1024), ErrorCode::InvalidArgument);
}

RR_TEST(each_header_check_rejects_a_precisely_wrong_value) {
  const RackSnapshot snapshot = build_snapshot();
  const std::vector<std::uint8_t> image = image_of(snapshot);

  {
    std::vector<std::uint8_t> broken = image;
    broken[0] = 'X';
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::CorruptState);
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetFormatVersion, kStateFormatVersion + 1);
    reseal_header(broken);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::UnsupportedFormatVersion);
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetEndianTag, 0x04030201u);
    reseal_header(broken);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::CorruptState);
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetSlotsPerUnit, 4u);
    reseal_header(broken);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::UnsupportedCoordinateModel);
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetPayloadKind, 7u);
    reseal_header(broken);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::UnsupportedFormatVersion);
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetPayloadRackCount, 0xFFFFFFFFu);
    reseal_header(broken);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::LimitExceeded);
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetPayloadRackCount, 5u);
    reseal_header(broken);
    const auto decoded = RackSnapshot::from_bytes(broken.data(), broken.size());
    RR_CHECK(!decoded.has_value());
  }
  {
    std::vector<std::uint8_t> broken = image;
    write_le32(broken, kOffsetPayloadLayout, kSnapshotLayoutVersion + 1);
    reseal_header(broken);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::UnsupportedFormatVersion);
  }
  {
    // A truncated declaration: the header claims more payload than exists.
    std::vector<std::uint8_t> broken = image;
    write_le64(broken, kOffsetPayloadLength, broken.size());
    reseal_header(broken, false);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::TruncatedState);
  }
  {
    // A declared payload above the accepted bound is refused before any
    // allocation is attempted.
    std::vector<std::uint8_t> broken = image;
    write_le64(broken, kOffsetPayloadLength, kMaxStateFileBytes + 1);
    reseal_header(broken, false);
    RR_REQUIRE_CODE(RackSnapshot::from_bytes(broken.data(), broken.size()),
                    ErrorCode::TruncatedState);
  }
}

RR_TEST(payload_bit_flips_are_caught_by_the_integrity_check) {
  const RackSnapshot snapshot = build_snapshot();
  const std::vector<std::uint8_t> image = image_of(snapshot);

  // Flipping one bit anywhere in the payload, the header or the trailer must be
  // detected rather than reinterpreted.
  std::size_t checked = 0;
  for (std::size_t offset = 0; offset < image.size(); offset += 7) {
    std::vector<std::uint8_t> broken = image;
    broken[offset] = static_cast<std::uint8_t>(broken[offset] ^ 0x40u);
    const auto decoded = RackSnapshot::from_bytes(broken.data(), broken.size());
    RR_CHECK(!decoded.has_value());
    ++checked;
  }
  RR_CHECK(checked > 20);
}

RR_TEST(trailing_and_leading_garbage_is_rejected) {
  const RackSnapshot snapshot = build_snapshot();
  const std::vector<std::uint8_t> image = image_of(snapshot);

  std::vector<std::uint8_t> longer = image;
  longer.push_back(0x00);
  RR_CHECK(!RackSnapshot::from_bytes(longer.data(), longer.size()).has_value());

  std::vector<std::uint8_t> prefixed = {0x00, 0x01};
  prefixed.insert(prefixed.end(), image.begin(), image.end());
  RR_CHECK(!RackSnapshot::from_bytes(prefixed.data(), prefixed.size()).has_value());
}

RR_TEST(state_image_rejects_a_rack_record_that_violates_an_invariant) {
  // The image below is assembled by hand from the documented layout: one rack,
  // two exclusive members occupying the same rack unit. Every checksum is
  // correct, so the only thing that can reject it is invariant validation.
  std::vector<std::uint8_t> payload;
  const auto u8 = [&payload](std::uint8_t value) { payload.push_back(value); };
  const auto u32 = [&payload](std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
      payload.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
  };
  const auto u64 = [&payload](std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
      payload.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
  };
  const auto text = [&payload, &u32](std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
  };
  const auto traits = [&u32](std::size_t count) { u32(static_cast<std::uint32_t>(count)); };

  u32(kSnapshotLayoutVersion);
  u32(1);  // rack count

  text("rack:crafted");
  u64(2);  // generation
  u64(1);  // revision
  u64(2);  // membership generation
  u8(0);   // lifecycle: defined
  u32(1);  // one rack unit
  text("cp:crafted");
  traits(0);  // provides
  u32(0);         // power domains
  u32(0);         // cooling domains
  text("");       // label

  u32(2);  // two members
  for (const char* name : {"rm:crafted-1", "rm:crafted-2"}) {
    text(name);
    text(std::string("asset:") + name);
    u8(0);   // full span
    u32(1);  // span begin
    u32(3);  // span end
    text("");  // no shared class
    u32(0);    // no shared capacity
    u8(0);     // installed
    u64(2);
    u64(2);
    u64(2);
    traits(0);
    traits(0);
    u32(0);  // provenance count
    u64(0);  // provenance dropped
  }

  u32(0);  // rack provenance
  u64(0);  // provenance dropped
  u32(0);  // generation evidence
  u32(0);  // idempotency
  u64(0);  // idempotency dropped

  std::vector<std::uint8_t> image;
  const auto append = [&image](const std::vector<std::uint8_t>& bytes) {
    image.insert(image.end(), bytes.begin(), bytes.end());
  };
  const std::string magic = "RACKREGS";
  append(std::vector<std::uint8_t>(magic.begin(), magic.end()));
  const auto header_u32 = [&append](std::uint32_t value) {
    std::vector<std::uint8_t> bytes;
    for (unsigned i = 0; i < 4; ++i) {
      bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    append(bytes);
  };
  const auto header_u64 = [&append](std::uint64_t value) {
    std::vector<std::uint8_t> bytes;
    for (unsigned i = 0; i < 8; ++i) {
      bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    append(bytes);
  };
  header_u32(kStateFormatVersion);
  header_u32(0x01020304u);
  header_u32(kMountSlotsPerRackUnit);
  header_u32(1u);  // payload kind
  header_u64(1);   // epoch
  header_u64(3);   // sequence
  header_u32(kVersionMajor);
  header_u32(kVersionMinor);
  header_u32(kVersionPatch);
  header_u64(payload.size());
  const auto payload_digest = sha256(payload.data(), payload.size());
  image.insert(image.end(), payload_digest.begin(), payload_digest.end());
  image.push_back(0);  // no producer
  const auto header_digest = sha256(image.data(), image.size());
  image.insert(image.end(), header_digest.begin(), header_digest.end());
  append(payload);
  const std::string trailer = "RACKRGEN";
  append(std::vector<std::uint8_t>(trailer.begin(), trailer.end()));

  const auto decoded = RackSnapshot::from_bytes(image.data(), image.size());
  RR_REQUIRE_CODE(decoded, ErrorCode::SnapshotInvalid);
  RR_CHECK(decoded.error().message.find("overlap") != std::string::npos ||
           decoded.error().message.find("occup") != std::string::npos);
}

RR_TEST(restoring_a_snapshot_twice_is_idempotent) {
  const RackSnapshot snapshot = build_snapshot();
  RackRegistry registry;
  RR_REQUIRE_OK(registry.restore(snapshot));
  const StateDigest first = registry.state_digest();
  RR_REQUIRE_OK(registry.restore(snapshot));
  RR_CHECK_EQ(registry.state_digest(), first);
  RR_CHECK_EQ(registry.rack_count(), std::size_t{1});
  RR_CHECK_EQ(registry.stats().member_count, std::size_t{3});
}
