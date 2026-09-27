// Rack Registry - text validation and canonical ordering helpers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/export.hpp"

namespace rackregistry {

// Strict UTF-8 validation: rejects overlong encodings, surrogate halves,
// values above U+10FFFF, truncated sequences and raw bytes 0xC0, 0xC1 and
// 0xF5..0xFF. Text accepted by this function round-trips byte for byte.
[[nodiscard]] RACK_REGISTRY_API bool is_valid_utf8(std::string_view text) noexcept;

// True when the text contains no Unicode control characters (C0 and C1
// ranges, plus DEL). Requires valid UTF-8; invalid UTF-8 returns false.
[[nodiscard]] RACK_REGISTRY_API bool has_no_control_characters(std::string_view text) noexcept;

// Byte-wise ordering. All public and serialized ordering in this library is
// byte-wise over validated text, never locale dependent.
[[nodiscard]] RACK_REGISTRY_API bool byte_less(std::string_view left,
                                               std::string_view right) noexcept;

[[nodiscard]] constexpr bool ascii_alphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] constexpr bool ascii_lower_alphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] constexpr bool ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool ascii_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }

[[nodiscard]] constexpr char ascii_to_lower(char c) noexcept {
  return ascii_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when every byte satisfies `predicate`.
template <typename Predicate>
[[nodiscard]] bool all_bytes(std::string_view text, Predicate predicate) noexcept {
  for (const char c : text) {
    if (!predicate(c)) {
      return false;
    }
  }
  return true;
}

// Lowercase hexadecimal encoding of a byte string. Used for digests and for
// the canonical text form of lock records.
[[nodiscard]] RACK_REGISTRY_API std::string to_hex(const std::uint8_t* data, std::size_t size);
[[nodiscard]] RACK_REGISTRY_API std::string to_hex(const std::vector<std::uint8_t>& data);

// Splits on a single ASCII delimiter. Empty fields are preserved so that a
// malformed record is rejected rather than silently shortened.
[[nodiscard]] RACK_REGISTRY_API std::vector<std::string> split(std::string_view text, char delimiter);

// Joins with a delimiter, inserting nothing before the first element.
[[nodiscard]] RACK_REGISTRY_API std::string join(const std::vector<std::string>& parts, char delimiter);

// Sorts and removes duplicates using byte-wise ordering, returning true when
// the input already satisfied "sorted and unique".
[[nodiscard]] RACK_REGISTRY_API bool canonicalize(std::vector<std::string>& values);

}  // namespace rackregistry
