// Rack Registry - text, identity and mounting coordinate tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

}  // namespace

// ---------------------------------------------------------------------------
// UTF-8 and text
// ---------------------------------------------------------------------------

RR_TEST(utf8_accepts_well_formed_text) {
  RR_CHECK(is_valid_utf8(""));
  RR_CHECK(is_valid_utf8("plain ascii"));
  RR_CHECK(is_valid_utf8("\xC3\xA9"));          // U+00E9
  RR_CHECK(is_valid_utf8("\xE2\x82\xAC"));      // U+20AC
  RR_CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));  // U+1F600
  RR_CHECK(is_valid_utf8("\xED\x9F\xBF"));      // U+D7FF, below the surrogate block
  RR_CHECK(is_valid_utf8("\xEE\x80\x80"));      // U+E000, above the surrogate block
  RR_CHECK(is_valid_utf8("\xF4\x8F\xBF\xBF"));  // U+10FFFF, the last code point
}

RR_TEST(utf8_rejects_malformed_text) {
  RR_CHECK(!is_valid_utf8("\x80"));             // stray continuation
  RR_CHECK(!is_valid_utf8("\xBF"));             // stray continuation
  RR_CHECK(!is_valid_utf8("\xC0\x80"));         // overlong encoding of NUL
  RR_CHECK(!is_valid_utf8("\xC1\xBF"));         // overlong two-byte encoding
  RR_CHECK(!is_valid_utf8("\xC2"));             // truncated two-byte sequence
  RR_CHECK(!is_valid_utf8("\xE0\x80\x80"));     // overlong three-byte encoding
  RR_CHECK(!is_valid_utf8("\xED\xA0\x80"));     // UTF-16 surrogate half
  RR_CHECK(!is_valid_utf8("\xED\xBF\xBF"));     // UTF-16 surrogate half
  RR_CHECK(!is_valid_utf8("\xF0\x80\x80\x80")); // overlong four-byte encoding
  RR_CHECK(!is_valid_utf8("\xF4\x90\x80\x80")); // above U+10FFFF
  RR_CHECK(!is_valid_utf8("\xF5\x80\x80\x80")); // invalid lead byte
  RR_CHECK(!is_valid_utf8("\xFF"));
  RR_CHECK(!is_valid_utf8("ok\xC3"));           // truncated at the end
}

RR_TEST(control_characters_are_rejected_in_free_text) {
  RR_CHECK(has_no_control_characters("clean text"));
  RR_CHECK(has_no_control_characters("\xC2\xA0"));  // U+00A0 is a space, not a control
  RR_CHECK(!has_no_control_characters("line\nbreak"));
  RR_CHECK(!has_no_control_characters("tab\there"));
  RR_CHECK(!has_no_control_characters(std::string("nul\0byte", 8)));
  RR_CHECK(!has_no_control_characters("\x7F"));      // DEL
  RR_CHECK(!has_no_control_characters("\xC2\x85"));  // U+0085, a C1 control
  // U+2028 is a Unicode separator, not a C0/C1 control, so it is accepted.
  RR_CHECK(has_no_control_characters("\xE2\x80\xA8"));
  RR_CHECK(!has_no_control_characters("\xF0\x9F\x98\x80" "\x01"));
}

RR_TEST(byte_wise_ordering_is_not_locale_dependent) {
  RR_CHECK(byte_less("A", "a"));
  RR_CHECK(byte_less("rack:a", "rack:b"));
  RR_CHECK(!byte_less("rack:b", "rack:a"));
  RR_CHECK(!byte_less("same", "same"));

  std::vector<std::string> values = {"Zulu", "alpha", "Alpha"};
  const bool already_canonical = canonicalize(values);
  RR_CHECK(!already_canonical);
  RR_CHECK_EQ(values.size(), std::size_t{3});
  RR_CHECK_EQ(values[0], std::string("Alpha"));
  RR_CHECK_EQ(values[1], std::string("Zulu"));
  RR_CHECK_EQ(values[2], std::string("alpha"));

  std::vector<std::string> duplicates = {"b", "a", "b"};
  const bool duplicates_were_canonical = canonicalize(duplicates);
  RR_CHECK(!duplicates_were_canonical);
  RR_CHECK_EQ(duplicates.size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// Identities
// ---------------------------------------------------------------------------

RR_TEST(rack_identity_requires_its_prefix_and_domain) {
  RR_REQUIRE_OK(RackId::parse("rack:a"));
  RR_REQUIRE_OK(RackId::parse("rack:A-01.zone:west"));
  RR_REQUIRE_OK(RackId::parse("rack:0"));

  RR_REQUIRE_CODE(RackId::parse(""), ErrorCode::EmptyValue);
  RR_REQUIRE_CODE(RackId::parse("rack:"), ErrorCode::EmptyValue);
  RR_REQUIRE_CODE(RackId::parse("a01"), ErrorCode::MalformedIdentity);
  RR_REQUIRE_CODE(RackId::parse("RACK:a01"), ErrorCode::MalformedIdentity);
  RR_REQUIRE_CODE(RackId::parse("rack:-leading"), ErrorCode::InvalidCharacter);
  RR_REQUIRE_CODE(RackId::parse("rack:has space"), ErrorCode::InvalidCharacter);
  RR_REQUIRE_CODE(RackId::parse("rack:slash/no"), ErrorCode::InvalidCharacter);
  RR_REQUIRE_CODE(RackId::parse(std::string("rack:") + std::string(97, 'a')),
                  ErrorCode::IdentityTooLong);
}

RR_TEST(identity_families_are_distinct_types_with_distinct_prefixes) {
  RR_REQUIRE_OK(RackMemberId::parse("rm:x"));
  RR_REQUIRE_CODE(RackMemberId::parse("rack:x"), ErrorCode::MalformedIdentity);
  RR_REQUIRE_OK(CompatibilityProfileId::parse("cp:x"));
  RR_REQUIRE_CODE(CompatibilityProfileId::parse("rm:x"), ErrorCode::MalformedIdentity);
  RR_REQUIRE_OK(SharedMountClass::parse("smc:bay"));
  RR_REQUIRE_CODE(SharedMountClass::parse("cp:bay"), ErrorCode::MalformedIdentity);
  RR_REQUIRE_OK(WriterId::parse("wr:w"));
  RR_REQUIRE_CODE(WriterId::parse("wr:"), ErrorCode::EmptyValue);
}

RR_TEST(opaque_references_accept_scheme_styles_and_reject_control_text) {
  RR_REQUIRE_OK(AssetId::parse("asset:server-01"));
  RR_REQUIRE_OK(AssetId::parse("vendor.example/asset@rev+1"));
  RR_REQUIRE_CODE(AssetId::parse("asset:has space"), ErrorCode::InvalidCharacter);
  RR_REQUIRE_CODE(AssetId::parse(""), ErrorCode::EmptyValue);
  RR_REQUIRE_CODE(AssetId::parse(std::string(161, 'a')), ErrorCode::IdentityTooLong);

  RR_REQUIRE_OK(PowerDomainReference::parse("pdu-a/feed-1"));
  RR_REQUIRE_OK(CoolingDomainReference::parse("crac-3/loop-b"));
}

RR_TEST(traits_are_lowercase_and_labels_are_free_utf8) {
  RR_REQUIRE_OK(Trait::parse("power.ac.208v"));
  RR_REQUIRE_CODE(Trait::parse("Power.AC"), ErrorCode::InvalidCharacter);
  RR_REQUIRE_CODE(Trait::parse(""), ErrorCode::EmptyValue);
  RR_REQUIRE_CODE(Trait::parse(std::string(49, 'a')), ErrorCode::IdentityTooLong);

  RR_REQUIRE_OK(DisplayLabel::parse("Rack A-01 \xE2\x82\xAC"));
  RR_REQUIRE_CODE(DisplayLabel::parse("bad \xFF utf8"), ErrorCode::InvalidUtf8);
  RR_REQUIRE_CODE(DisplayLabel::parse("control\nchar"), ErrorCode::InvalidCharacter);
  RR_CHECK(DisplayLabel::none().empty());
  RR_REQUIRE_OK(Note::parse_or_none(""));
  RR_REQUIRE_CODE(Note::parse("bad \xFF utf8"), ErrorCode::InvalidUtf8);
}

// ---------------------------------------------------------------------------
// Interval semantics
// ---------------------------------------------------------------------------

RR_TEST(rack_unit_ranges_are_half_open_and_inclusive_parsing_is_explicit) {
  const auto inclusive = RackUnitRange::inclusive(10, 12);
  RR_REQUIRE_OK(inclusive);
  RR_CHECK_EQ(inclusive.value().first(), 10u);
  RR_CHECK_EQ(inclusive.value().last(), 13u);
  RR_CHECK_EQ(inclusive.value().unit_count(), 3u);
  RR_CHECK_EQ(inclusive.value().inclusive_last(), 12u);
  RR_CHECK_EQ(inclusive.value().to_text(), std::string("U10-U12"));
  RR_CHECK(inclusive.value().contains(RackUnitIndex::create(12).value()));
  RR_CHECK(!inclusive.value().contains(RackUnitIndex::create(13).value()));

  const auto single = RackUnitRange::single(RackUnitIndex::create(4).value());
  RR_REQUIRE_OK(single);
  RR_CHECK_EQ(single.value().to_text(), std::string("U4"));
  RR_CHECK_EQ(single.value().unit_count(), 1u);

  const auto adjacent = RackUnitRange::create(13, 15);
  RR_REQUIRE_OK(adjacent);
  RR_CHECK(!inclusive.value().overlaps(adjacent.value()));

  RR_REQUIRE_CODE(RackUnitRange::inclusive(5, 4), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::create(0, 1), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::create(1, 1), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::create(1, kMaxRackUnits + 2), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitIndex::create(0), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitIndex::create(kMaxRackUnits + 1), ErrorCode::InvalidRange);
  RR_REQUIRE_OK(RackUnitRange::create(1, kMaxRackUnits + 1));
}

RR_TEST(unit_range_parsing_round_trips_and_rejects_junk) {
  const auto range = RackUnitRange::parse("u3-U5");
  RR_REQUIRE_OK(range);
  RR_CHECK_EQ(range.value().first(), 3u);
  RR_CHECK_EQ(range.value().last(), 6u);
  const auto round_trip = RackUnitRange::parse(range.value().to_text());
  RR_REQUIRE_OK(round_trip);
  RR_CHECK_EQ(round_trip.value(), range.value());

  const auto single = RackUnitRange::parse("8");
  RR_REQUIRE_OK(single);
  RR_CHECK_EQ(single.value().to_text(), std::string("U8"));

  RR_REQUIRE_CODE(RackUnitRange::parse(""), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::parse("U"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::parse("U1-"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::parse("-U1"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::parse("U1-U2-U3"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::parse("U0"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(RackUnitRange::parse("U99999999999999999999999"), ErrorCode::InvalidRange);
}

RR_TEST(slot_ranges_are_half_open_and_adjacency_is_not_overlap) {
  const auto left = SlotRange::create(1, 3);
  const auto right = SlotRange::create(3, 5);
  RR_REQUIRE_OK(left);
  RR_REQUIRE_OK(right);
  RR_CHECK(!left.value().overlaps(right.value()));
  RR_CHECK(!right.value().overlaps(left.value()));
  RR_CHECK_EQ(left.value().to_text(), std::string("[1,3)"));
  RR_CHECK(left.value().is_identical_to(SlotRange::create(1, 3).value()));
  RR_CHECK(!left.value().is_identical_to(right.value()));

  const auto touching = SlotRange::create(2, 4);
  RR_REQUIRE_OK(touching);
  RR_CHECK(left.value().overlaps(touching.value()));
  RR_CHECK(touching.value().overlaps(left.value()));
  RR_CHECK(left.value().contains(SlotRange::create(1, 2).value()));
  RR_CHECK(!left.value().contains(touching.value()));

  RR_REQUIRE_CODE(SlotRange::create(0, 1), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(SlotRange::create(3, 3), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(SlotRange::create(5, 4), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(SlotRange::create(1, kMaxMountSlotExclusive + 1), ErrorCode::InvalidRange);
  RR_REQUIRE_OK(SlotRange::create(1, kMaxMountSlotExclusive));
}

RR_TEST(slot_boundary_values_are_exact) {
  const auto last = SlotRange::create(kMaxMountSlotExclusive - 1, kMaxMountSlotExclusive);
  RR_REQUIRE_OK(last);
  RR_CHECK_EQ(last.value().inclusive_last(), kMaxMountSlotExclusive - 1);
  RR_CHECK_EQ(last.value().slot_count(), 1u);
  RR_CHECK_EQ(last.value().touched_units().to_text(), std::string("U1024"));

  const auto whole = SlotRange::create(1, kMaxMountSlotExclusive);
  RR_REQUIRE_OK(whole);
  RR_CHECK_EQ(whole.value().slot_count(), kMaxRackUnits * kMountSlotsPerRackUnit);
  RR_CHECK_EQ(whole.value().touched_units().to_text(), std::string("U1-U1024"));

  RR_REQUIRE_CODE(MountSlotIndex::create(0), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(MountSlotIndex::create(kMaxMountSlotExclusive), ErrorCode::InvalidRange);
  RR_CHECK_EQ(MountSlotIndex::create(3).value().rack_unit(), 2u);
  RR_CHECK_EQ(MountSlotIndex::create(4).value().rack_unit(), 2u);
  RR_CHECK_EQ(MountSlotIndex::create(5).value().rack_unit(), 3u);
}

RR_TEST(unit_and_slot_coordinates_agree) {
  const auto unit = RackUnitIndex::create(2).value();
  const auto lower = SlotRange::half_unit(unit, HalfSlot::Lower);
  const auto upper = SlotRange::half_unit(unit, HalfSlot::Upper);
  RR_REQUIRE_OK(lower);
  RR_REQUIRE_OK(upper);
  RR_CHECK_EQ(lower.value().to_text(), std::string("[3,4)"));
  RR_CHECK_EQ(upper.value().to_text(), std::string("[4,5)"));
  RR_CHECK_EQ(lower.value().touched_units().to_text(), std::string("U2"));
  RR_CHECK_EQ(upper.value().touched_units().to_text(), std::string("U2"));
  RR_CHECK(!lower.value().overlaps(upper.value()));

  const auto units = RackUnitRange::inclusive(2, 3);
  RR_REQUIRE_OK(units);
  const auto slots = SlotRange::whole_units(units.value());
  RR_REQUIRE_OK(slots);
  RR_CHECK_EQ(slots.value().to_text(), std::string("[3,7)"));
  RR_CHECK_EQ(slots.value().touched_units().to_text(), std::string("U2-U3"));

  const auto extent = rack_slot_extent(4);
  RR_REQUIRE_OK(extent);
  RR_CHECK_EQ(extent.value().to_text(), std::string("[1,9)"));
  RR_REQUIRE_CODE(rack_slot_extent(0), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(validate_unit_count(kMaxRackUnits + 1), ErrorCode::InvalidRange);
}

RR_TEST(range_subtraction_covers_every_shape) {
  const SlotRange whole = SlotRange::create(1, 11).value();
  const SlotRange disjoint = SlotRange::create(20, 30).value();

  const auto untouched = whole.subtract(disjoint);
  RR_CHECK_EQ(untouched.size(), std::size_t{1});
  RR_CHECK_EQ(untouched[0], whole);

  const auto head = whole.subtract(SlotRange::create(5, 20).value());
  RR_CHECK_EQ(head.size(), std::size_t{1});
  RR_CHECK_EQ(head[0].to_text(), std::string("[1,5)"));

  const auto tail = whole.subtract(SlotRange::create(1, 5).value());
  RR_CHECK_EQ(tail.size(), std::size_t{1});
  RR_CHECK_EQ(tail[0].to_text(), std::string("[5,11)"));

  const auto middle = whole.subtract(SlotRange::create(4, 7).value());
  RR_CHECK_EQ(middle.size(), std::size_t{2});
  RR_CHECK_EQ(middle[0].to_text(), std::string("[1,4)"));
  RR_CHECK_EQ(middle[1].to_text(), std::string("[7,11)"));

  const auto exactly = whole.subtract(SlotRange::create(1, 11).value());
  RR_CHECK(exactly.empty());

  const auto wider = whole.subtract(SlotRange::create(1, 40).value_or(whole));
  RR_CHECK(wider.empty());
}

RR_TEST(slot_range_parsing_round_trips_and_rejects_junk) {
  const auto range = SlotRange::parse("[3,5)");
  RR_REQUIRE_OK(range);
  RR_CHECK_EQ(range.value().begin(), 3u);
  RR_CHECK_EQ(range.value().end(), 5u);
  RR_CHECK_EQ(range.value().to_text(), std::string("[3,5)"));

  RR_REQUIRE_CODE(SlotRange::parse("3,5"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(SlotRange::parse("[3,5]"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(SlotRange::parse("[5,3)"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(SlotRange::parse("[a,b)"), ErrorCode::InvalidArgument);
  RR_REQUIRE_CODE(SlotRange::parse("[]"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(SlotRange::parse("[1,2,3)"), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Mount spans
// ---------------------------------------------------------------------------

RR_TEST(mount_span_factories_enforce_their_own_rules) {
  const auto full = MountSpan::full(SlotRange::create(1, 3).value());
  RR_REQUIRE_OK(full);
  RR_CHECK_EQ(full.value().to_text(), std::string("full:[1,3)"));
  RR_CHECK(full.value().kind() == MountKind::FullSpan);
  RR_CHECK(!full.value().is_shared());
  RR_CHECK(!full.value().is_zero_u());
  RR_CHECK_EQ(full.value().share_capacity(), 0u);

  const auto shared = MountSpan::shared(SlotRange::create(1, 3).value(),
                                        SharedMountClass::parse("smc:bay").value(), 2);
  RR_REQUIRE_OK(shared);
  RR_CHECK_EQ(shared.value().to_text(), std::string("shared:[1,3)/smc:bay/2"));
  RR_CHECK(shared.value().is_shared());
  RR_CHECK_EQ(shared.value().share_capacity(), 2u);

  RR_REQUIRE_CODE(MountSpan::shared(SlotRange::create(1, 3).value(), SharedMountClass{}, 2),
                  ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::shared(SlotRange::create(1, 3).value(),
                                    SharedMountClass::parse("smc:bay").value(), 0),
                  ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::full(SlotRange{}), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::full_units(RackUnitRange{}), ErrorCode::InvalidRange);

  const MountSpan zero = MountSpan::zero_u();
  RR_CHECK(zero.is_zero_u());
  RR_CHECK_EQ(zero.to_text(), std::string("zero-u"));
  RR_CHECK(!zero.touched_units().has_value());
  RR_CHECK(!zero.span().is_valid());
  RR_CHECK(zero.equals(MountSpan::zero_u()));
  RR_CHECK(!zero.equals(full.value()));
}

RR_TEST(mount_span_text_round_trips_and_rejects_junk) {
  const std::vector<std::string> canonical = {"full:[1,3)", "shared:[3,5)/smc:psu-bay/4",
                                              "zero-u"};
  for (const std::string& text : canonical) {
    const auto span = MountSpan::parse(text);
    RR_REQUIRE_OK(span);
    RR_CHECK_EQ(span.value().to_text(), text);
  }

  const auto unit_form = MountSpan::parse("full:U10-U12");
  RR_REQUIRE_OK(unit_form);
  RR_CHECK_EQ(unit_form.value().to_text(), std::string("full:[19,25)"));
  RR_REQUIRE(unit_form.value().touched_units().has_value());
  RR_CHECK_EQ(unit_form.value().touched_units()->to_text(), std::string("U10-U12"));

  const auto shared_units = MountSpan::parse("shared:U1-U2/smc:bay/2");
  RR_REQUIRE_OK(shared_units);
  RR_CHECK_EQ(shared_units.value().to_text(), std::string("shared:[1,5)/smc:bay/2"));

  RR_REQUIRE_OK(MountSpan::parse("zero-u"));
  RR_REQUIRE_CODE(MountSpan::parse("nonsense"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("full"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("full:"), ErrorCode::InvalidRange);
  RR_REQUIRE_CODE(MountSpan::parse("shared:[1,3)"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("shared:[1,3)/smc:bay"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("shared:[1,3)/smc:bay/0"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("shared:[1,3)/smc:bay/9999"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("shared:[1,3)/cp:bay/2"), ErrorCode::MalformedIdentity);
  RR_REQUIRE_CODE(MountSpan::parse("zero-u:[1,3)"), ErrorCode::InvalidMountSpan);
  RR_REQUIRE_CODE(MountSpan::parse("unknown:[1,3)"), ErrorCode::InvalidEnumValue);
}

RR_TEST(conflict_rules_follow_the_documented_occupation_model) {
  const SlotRange one_to_three = SlotRange::create(1, 3).value();
  const SlotRange three_to_five = SlotRange::create(3, 5).value();
  const SlotRange two_to_four = SlotRange::create(2, 4).value();

  const MountSpan full_a = MountSpan::full(one_to_three).value();
  const MountSpan full_b = MountSpan::full(three_to_five).value();
  const MountSpan full_overlap = MountSpan::full(two_to_four).value();
  const MountSpan shared_a =
      MountSpan::shared(one_to_three, SharedMountClass::parse("smc:bay").value(), 2).value();
  const MountSpan shared_b =
      MountSpan::shared(one_to_three, SharedMountClass::parse("smc:bay").value(), 2).value();
  const MountSpan shared_other =
      MountSpan::shared(one_to_three, SharedMountClass::parse("smc:other").value(), 2).value();
  const MountSpan shared_other_capacity =
      MountSpan::shared(one_to_three, SharedMountClass::parse("smc:bay").value(), 4).value();

  RR_CHECK(!full_a.conflicts_with(full_b));       // adjacent, half open
  RR_CHECK(full_a.conflicts_with(full_overlap));  // partial overlap
  RR_CHECK(full_a.conflicts_with(shared_a));      // exclusive against shared
  RR_CHECK(!shared_a.conflicts_with(shared_b));   // legal co-occupancy
  RR_CHECK(shared_a.co_occupies(shared_b));
  RR_CHECK(shared_a.co_occupies(shared_other_capacity));  // capacity is the registry's check
  RR_CHECK(shared_a.conflicts_with(shared_other));
  RR_CHECK(!shared_a.conflicts_with(full_b));  // disjoint
  RR_CHECK(!full_a.conflicts_with(MountSpan::zero_u()));
  RR_CHECK(!MountSpan::zero_u().conflicts_with(MountSpan::zero_u()));
  RR_CHECK(!MountSpan::zero_u().conflicts_with(shared_a));
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

RR_TEST(sha256_matches_the_published_vectors) {
  struct Vector {
    std::string input;
    std::string expected;
  };
  const std::vector<Vector> vectors = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
  for (const Vector& vector : vectors) {
    const auto digest =
        sha256(reinterpret_cast<const std::uint8_t*>(vector.input.data()), vector.input.size());
    RR_CHECK_EQ(to_hex(digest.data(), digest.size()), vector.expected);
  }

  // A one-million-byte input exercises multi-block streaming.
  Sha256 hasher;
  const std::string block(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    hasher.update(block);
  }
  const auto digest = hasher.finish();
  RR_CHECK_EQ(to_hex(digest.data(), digest.size()),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  // Padding boundaries: 55, 56, 63 and 64 bytes all cross a block edge.
  for (const std::size_t length : {static_cast<std::size_t>(55), static_cast<std::size_t>(56),
                                   static_cast<std::size_t>(63), static_cast<std::size_t>(64)}) {
    const std::string input(length, 'x');
    Sha256 streaming;
    streaming.update(input);
    const auto streamed = streaming.finish();
    const auto one_shot =
        sha256(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    RR_CHECK_EQ(to_hex(one_shot.data(), one_shot.size()), to_hex(streamed.data(), streamed.size()));
  }
}

RR_TEST(state_digest_hex_round_trips_and_is_domain_separated) {
  const std::vector<std::uint8_t> payload = {1, 2, 3, 4};
  const StateDigest first = StateDigest::domain("domain.one", payload);
  const StateDigest second = StateDigest::domain("domain.two", payload);
  RR_CHECK_NE(first, second);

  const auto parsed = StateDigest::from_hex(first.to_hex());
  RR_REQUIRE_OK(parsed);
  RR_CHECK_EQ(parsed.value(), first);

  const StateDigest empty;
  RR_CHECK(empty.is_zero());
  RR_REQUIRE_CODE(StateDigest::from_hex("abc"), ErrorCode::InvalidArgument);
  RR_REQUIRE_CODE(StateDigest::from_hex(std::string(64, 'z')), ErrorCode::InvalidCharacter);
  RR_REQUIRE_OK(StateDigest::from_hex(std::string(64, 'A')));
}
