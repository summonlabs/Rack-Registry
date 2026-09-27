// Rack Registry - adversarial input tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Everything here treats the state file, the identities and the command
// arguments as hostile. The obligations are: never crash, never allocate for a
// declared size before checking it, never normalize a malformed value into
// authoritative state, and always report a code from the documented taxonomy.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

bool is_persistence_code(ErrorCode code) {
  const ErrorCategory category = category_of(code);
  return category == ErrorCategory::Persistence || category == ErrorCategory::Limits ||
         category == ErrorCategory::Occupancy || category == ErrorCategory::Identity ||
         category == ErrorCategory::Input;
}

RackSnapshot sample_snapshot() {
  RackRegistry registry;
  RegisterRackRequest request;
  request.structure.id = RackId::parse("rack:adversarial").value();
  request.structure.unit_count = 8;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:adv").value();
  request.structure.profile.provides = TraitSet::parse("power.ac.208v").value();
  request.structure.power_domains = {PowerDomainReference::parse("pdu-a").value()};
  request.structure.cooling_domains = {CoolingDomainReference::parse("crac-a").value()};
  request.structure.label = DisplayLabel::parse("adversarial").value();
  request.identity.provenance = rrtest::provenance_for("adversarial", 1);
  const auto registered = registry.register_rack(request);
  if (!registered) {
    return RackSnapshot{};
  }
  RackGeneration generation = registered.value().generation;
  MembershipGeneration membership = registered.value().membership_generation;
  for (int index = 0; index < 4; ++index) {
    InsertMemberRequest insert;
    insert.rack_id = request.structure.id;
    insert.precondition.expected_generation = generation;
    insert.precondition.expected_membership_generation = membership;
    insert.member_id = RackMemberId::parse("rm:a" + std::to_string(index)).value();
    insert.asset_id = AssetId::parse("asset:a" + std::to_string(index)).value();
    insert.mount =
        MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(
                                                       static_cast<std::uint32_t>(index) + 1)
                                                       .value())
                                  .value())
            .value();
    insert.requirements.required = TraitSet::parse("power.ac.208v").value();
    insert.identity.provenance = rrtest::provenance_for("adversarial", 2);
    const auto receipt = registry.insert_member(insert);
    if (!receipt) {
      return RackSnapshot{};
    }
    generation = receipt.value().generation;
    membership = receipt.value().membership_generation;
  }
  return registry.snapshot(std::nullopt);
}

}  // namespace

RR_TEST(random_mutations_of_a_state_image_never_crash_the_reader) {
  constexpr std::uint64_t kSeed = 0xA5A50001ull;
  std::cout << "  adversarial seed " << kSeed << " state image mutation\n";
  const RackSnapshot snapshot = sample_snapshot();
  RR_REQUIRE_OK(snapshot.validate());
  const std::vector<std::uint8_t> original = snapshot.to_bytes();

  rrtest::Random random(kSeed);
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::vector<std::uint8_t> image = original;
    const std::uint32_t shape = random.below(5);
    if (shape == 0) {
      // Single byte replacement.
      image[random.below(static_cast<std::uint32_t>(image.size()))] =
          static_cast<std::uint8_t>(random.below(256));
    } else if (shape == 1) {
      // Truncation at an arbitrary offset.
      image.resize(random.below(static_cast<std::uint32_t>(image.size()) + 1));
    } else if (shape == 2) {
      // Extension with arbitrary bytes.
      const std::uint32_t extra = 1 + random.below(32);
      for (std::uint32_t i = 0; i < extra; ++i) {
        image.push_back(static_cast<std::uint8_t>(random.below(256)));
      }
    } else if (shape == 3) {
      // Burst corruption.
      const std::uint32_t start = random.below(static_cast<std::uint32_t>(image.size()));
      const std::uint32_t length =
          std::min<std::uint32_t>(8, static_cast<std::uint32_t>(image.size()) - start);
      for (std::uint32_t i = 0; i < length; ++i) {
        image[start + i] = static_cast<std::uint8_t>(random.below(256));
      }
    } else {
      // Truncate and then extend, producing a file whose declared lengths lie.
      image.resize(1 + random.below(static_cast<std::uint32_t>(image.size())));
      const std::uint32_t extra = random.below(64);
      for (std::uint32_t i = 0; i < extra; ++i) {
        image.push_back(static_cast<std::uint8_t>(random.below(256)));
      }
    }

    const auto decoded = RackSnapshot::from_bytes(image.data(), image.size());
    if (decoded.has_value()) {
      ++accepted;
      // Anything the reader accepts must satisfy every invariant.
      RR_REQUIRE_OK(decoded.value().validate());
    } else {
      ++rejected;
      RR_CHECK(is_persistence_code(decoded.error().code));
      RR_CHECK(!decoded.error().message.empty());
      // Rejections must be stable for the same bytes.
      const auto again = RackSnapshot::from_bytes(image.data(), image.size());
      RR_REQUIRE(!again.has_value());
      RR_CHECK(again.error().code == decoded.error().code);
    }
  }
  std::cout << "  adversarial accepted=" << accepted << " rejected=" << rejected << "\n";
  RR_CHECK(rejected > 3900);
}

RR_TEST(integer_overflow_edges_in_declared_lengths_are_handled) {
  const RackSnapshot snapshot = sample_snapshot();
  const std::vector<std::uint8_t> image = snapshot.to_bytes();

  // A declared payload length of 2^64-1 is rejected either by the length check
  // or by the header digest that covers the length field, never by wrapping
  // into a small allocation.
  std::vector<std::uint8_t> broken = image;
  for (unsigned i = 0; i < 8; ++i) {
    broken[52 + i] = 0xFF;
  }
  const auto huge_length = RackSnapshot::from_bytes(broken.data(), broken.size());
  RR_REQUIRE(!huge_length.has_value());
  RR_CHECK(huge_length.error().code == ErrorCode::TruncatedState ||
           huge_length.error().code == ErrorCode::IntegrityCheckFailed ||
           huge_length.error().code == ErrorCode::StateTooLarge);

  // A declared payload length just inside the bound but larger than the file.
  broken = image;
  const std::uint64_t almost = kMaxStateFileBytes - 1;
  for (unsigned i = 0; i < 8; ++i) {
    broken[52 + i] = static_cast<std::uint8_t>(almost >> (i * 8));
  }
  const auto large_length = RackSnapshot::from_bytes(broken.data(), broken.size());
  RR_REQUIRE(!large_length.has_value());
  RR_CHECK(large_length.error().code == ErrorCode::TruncatedState ||
           large_length.error().code == ErrorCode::IntegrityCheckFailed ||
           large_length.error().code == ErrorCode::StateTooLarge);

  // A buffer of the right size but the wrong content is refused.
  std::vector<std::uint8_t> zeros(image.size(), 0);
  RR_REQUIRE_CODE(RackSnapshot::from_bytes(zeros.data(), zeros.size()), ErrorCode::CorruptState);

  // A file larger than the accepted bound is refused outright.
  std::vector<std::uint8_t> oversize(static_cast<std::size_t>(kMaxStateFileBytes) + 1, 0);
  RR_REQUIRE_CODE(RackSnapshot::from_bytes(oversize.data(), oversize.size()),
                  ErrorCode::StateTooLarge);
}

RR_TEST(identities_reject_every_malformed_shape) {
  const std::vector<std::string> bad_rack_ids = {
      "",           "rack",        "rack:",       ":rack:a",     "rack::a",
      " rack:a",    "rack:a ",     "rack:a\tb",   "rack:a/b",    "rack:a\\b",
      "rack:a b",   "rack:\x7F",   "rack:\xC3\xA9",
      std::string("rack:") + std::string(97, 'x'),
      std::string(200, 'r')};
  for (const std::string& text : bad_rack_ids) {
    const auto parsed = RackId::parse(text);
    RR_CHECK(!parsed.has_value());
    if (!parsed.has_value()) {
      RR_CHECK(is_persistence_code(parsed.error().code));
    }
  }

  const std::vector<std::string> bad_traits = {"", "UPPER", "has space", ".dot", "-x",
                                               "caf\xC3\xA9", std::string(49, 'z')};
  for (const std::string& text : bad_traits) {
    RR_CHECK(!Trait::parse(text).has_value());
  }
  // A trailing separator inside the body is part of the trait domain, so this
  // one is accepted: traits are not required to end in an alphanumeric.
  RR_CHECK(Trait::parse("dot.").has_value());

  const std::vector<std::string> bad_mounts = {
      "", "full", "full:", "zero", "zero-u:", "shared://", "shared:[1,3)//",
      "full:[-1,3)", "full:[1,-3)", "full:[0,3)", "full:[1,0)", "full:[1,99999999999999999999)"};
  for (const std::string& text : bad_mounts) {
    RR_CHECK(!MountSpan::parse(text).has_value());
  }
}

RR_TEST(a_rack_at_the_documented_bounds_is_accepted_and_one_past_is_not) {
  RackRegistry registry;
  RegisterRackRequest request;
  request.structure.id = RackId::parse("rack:max").value();
  request.structure.unit_count = kMaxRackUnits;
  request.structure.profile.id = CompatibilityProfileId::parse("cp:max").value();
  request.identity.provenance = rrtest::provenance_for("adversarial", 3);
  RR_REQUIRE_OK(registry.register_rack(request));

  request.structure.id = RackId::parse("rack:over").value();
  request.structure.unit_count = kMaxRackUnits + 1;
  RR_REQUIRE_CODE(registry.register_rack(request), ErrorCode::InvalidRange);

  // A member on the very last slot of the largest permitted rack fits.
  InsertMemberRequest insert;
  insert.rack_id = RackId::parse("rack:max").value();
  const auto view = registry.rack(insert.rack_id);
  RR_REQUIRE_OK(view);
  insert.precondition.expected_generation = view.value().generation();
  insert.precondition.expected_membership_generation = view.value().membership_generation();
  insert.member_id = RackMemberId::parse("rm:last").value();
  insert.asset_id = AssetId::parse("asset:last").value();
  const auto last_slot = SlotRange::create(kMaxMountSlotExclusive - 1, kMaxMountSlotExclusive);
  RR_REQUIRE_OK(last_slot);
  insert.mount = MountSpan::full(last_slot.value()).value();
  insert.identity.provenance = rrtest::provenance_for("adversarial", 4);
  RR_REQUIRE_OK(registry.insert_member(insert));

  // On a smaller rack, a placement beyond the physical extent is refused. The
  // span itself is a legal coordinate; it is the rack that cannot host it.
  RegisterRackRequest small;
  small.structure.id = RackId::parse("rack:small").value();
  small.structure.unit_count = 2;
  small.structure.profile.id = CompatibilityProfileId::parse("cp:max").value();
  small.identity.provenance = rrtest::provenance_for("adversarial", 5);
  const auto registered_small = registry.register_rack(small);
  RR_REQUIRE_OK(registered_small);

  InsertMemberRequest beyond;
  beyond.rack_id = small.structure.id;
  beyond.precondition.expected_generation = registered_small.value().generation;
  beyond.precondition.expected_membership_generation =
      registered_small.value().membership_generation;
  beyond.member_id = RackMemberId::parse("rm:beyond").value();
  beyond.asset_id = AssetId::parse("asset:beyond").value();
  const auto outside = SlotRange::create(5, 7);
  RR_REQUIRE_OK(outside);
  beyond.mount = MountSpan::full(outside.value()).value();
  beyond.identity.provenance = rrtest::provenance_for("adversarial", 6);
  RR_REQUIRE_CODE(registry.insert_member(beyond), ErrorCode::MountOutOfBounds);
}

RR_TEST(trait_sets_and_domain_lists_reject_duplicates_and_overflow) {
  RR_REQUIRE_CODE(TraitSet::create({Trait::parse("a").value(), Trait::parse("a").value()}),
                  ErrorCode::DuplicateTrait);

  std::vector<Trait> many;
  for (std::size_t i = 0; i <= kMaxTraitsPerSet; ++i) {
    many.push_back(Trait::parse("t" + std::to_string(i)).value());
  }
  RR_REQUIRE_CODE(TraitSet::create(many), ErrorCode::LimitExceeded);

  std::vector<Trait> at_bound;
  for (std::size_t i = 0; i < kMaxTraitsPerSet; ++i) {
    at_bound.push_back(Trait::parse("t" + std::to_string(i)).value());
  }
  RR_REQUIRE_OK(TraitSet::create(at_bound));

  // A set that arrives in descending order is sorted, not rejected.
  const auto parsed = TraitSet::parse("zeta,alpha,mid");
  RR_REQUIRE_OK(parsed);
  RR_CHECK_EQ(parsed.value().traits()[0].text(), std::string("alpha"));
  RR_CHECK_EQ(parsed.value().traits()[2].text(), std::string("zeta"));
}

RR_TEST(a_state_directory_that_does_not_exist_is_reported_not_created_silently) {
  rrtest::TempDir directory("missing");
  const std::filesystem::path nested = directory.file("does-not-exist") / "state.rrstate";
  const auto opened = RackStore::open(rrtest::store_options_for(nested, "wr:adv"));
  RR_CHECK(!opened.has_value());
  if (!opened.has_value()) {
    RR_CHECK(opened.error().code == ErrorCode::IoFailure ||
             opened.error().code == ErrorCode::WriterLockHeld);
  }

  const auto inspected = RackStore::inspect(nested);
  RR_REQUIRE_CODE(inspected, ErrorCode::IoFailure);
}

RR_TEST(unreadable_and_empty_state_files_are_rejected) {
  rrtest::TempDir directory("empty");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  // A zero length file is not a valid generation.
  {
    std::ofstream stream(state_file, std::ios::binary | std::ios::trunc);
    RR_REQUIRE(stream.good());
  }
  RR_REQUIRE_CODE(RackStore::inspect(state_file), ErrorCode::TruncatedState);

  const auto opened = RackStore::open(rrtest::store_options_for(state_file, "wr:adv"));
  RR_CHECK(!opened.has_value());

  // A file of the right size but the wrong content is rejected too.
  {
    std::ofstream stream(state_file, std::ios::binary | std::ios::trunc);
    RR_REQUIRE(stream.good());
    const std::vector<char> filler(512, 'z');
    stream.write(filler.data(), static_cast<std::streamsize>(filler.size()));
  }
  RR_REQUIRE_CODE(RackStore::inspect(state_file), ErrorCode::CorruptState);
}

RR_TEST(oversized_state_files_are_refused_before_they_are_read) {
  rrtest::TempDir directory("oversize");
  const std::filesystem::path state_file = directory.file("state.rrstate");

  // A sparse file whose declared size exceeds the bound is refused by size
  // alone, without reading its contents.
  {
    std::ofstream stream(state_file, std::ios::binary | std::ios::trunc);
    RR_REQUIRE(stream.good());
    stream.seekp(static_cast<std::streamoff>(kMaxStateFileBytes) + 1024);
    const char zero = '\0';
    stream.write(&zero, 1);
  }
  RR_REQUIRE_CODE(RackStore::inspect(state_file), ErrorCode::StateTooLarge);
}
