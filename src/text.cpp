// Rack Registry - text validation and canonical ordering helpers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/text.hpp"

#include <algorithm>
#include <array>

namespace rackregistry {
namespace {

// Decodes one UTF-8 sequence starting at `index`. Returns the code point and
// advances `index`, or returns 0xFFFFFFFF and leaves `index` untouched when the
// sequence is invalid. Used only after is_valid_utf8 has accepted the text.
constexpr std::uint32_t kInvalidCodePoint = 0xFFFFFFFFu;

std::uint32_t decode_one(std::string_view text, std::size_t& index) noexcept {
  const auto byte_at = [&text](std::size_t position) {
    return static_cast<unsigned char>(text[position]);
  };
  const unsigned char first = byte_at(index);
  if (first < 0x80) {
    ++index;
    return first;
  }
  std::size_t extra = 0;
  std::uint32_t value = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    extra = 1;
    value = first & 0x1Fu;
  } else if (first >= 0xE0 && first <= 0xEF) {
    extra = 2;
    value = first & 0x0Fu;
  } else if (first >= 0xF0 && first <= 0xF4) {
    extra = 3;
    value = first & 0x07u;
  } else {
    return kInvalidCodePoint;
  }
  if (index + extra >= text.size()) {
    return kInvalidCodePoint;
  }
  for (std::size_t k = 1; k <= extra; ++k) {
    const unsigned char continuation = byte_at(index + k);
    if (continuation < 0x80 || continuation > 0xBF) {
      return kInvalidCodePoint;
    }
    value = (value << 6) | (continuation & 0x3Fu);
  }
  index += extra + 1;
  return value;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto current = static_cast<unsigned char>(text[index]);
    if (current < 0x80) {
      ++index;
      continue;
    }

    std::size_t extra = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (current >= 0xC2 && current <= 0xDF) {
      extra = 1;  // 0xC0 and 0xC1 would be overlong two-byte encodings
    } else if (current == 0xE0) {
      extra = 2;
      low = 0xA0;  // reject overlong three-byte encodings
    } else if (current >= 0xE1 && current <= 0xEC) {
      extra = 2;
    } else if (current == 0xED) {
      extra = 2;
      high = 0x9F;  // reject UTF-16 surrogate halves
    } else if (current >= 0xEE && current <= 0xEF) {
      extra = 2;
    } else if (current == 0xF0) {
      extra = 3;
      low = 0x90;  // reject overlong four-byte encodings
    } else if (current >= 0xF1 && current <= 0xF3) {
      extra = 3;
    } else if (current == 0xF4) {
      extra = 3;
      high = 0x8F;  // reject code points above U+10FFFF
    } else {
      // 0x80..0xC1 (stray continuation or overlong lead) and 0xF5..0xFF.
      return false;
    }

    if (index + extra >= text.size()) {
      return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto continuation = static_cast<unsigned char>(text[index + k]);
      const unsigned char lower_bound = (k == 1) ? low : 0x80;
      const unsigned char upper_bound = (k == 1) ? high : 0xBF;
      if (continuation < lower_bound || continuation > upper_bound) {
        return false;
      }
    }
    index += extra + 1;
  }
  return true;
}

bool has_no_control_characters(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const std::uint32_t code_point = decode_one(text, index);
    if (code_point == kInvalidCodePoint) {
      return false;
    }
    const bool c0_control = code_point < 0x20;
    const bool del = code_point == 0x7F;
    const bool c1_control = code_point >= 0x80 && code_point <= 0x9F;
    if (c0_control || del || c1_control) {
      return false;
    }
  }
  return true;
}

bool byte_less(std::string_view left, std::string_view right) noexcept {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  static constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                   '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

std::string to_hex(const std::vector<std::uint8_t>& data) {
  return to_hex(data.data(), data.size());
}

std::vector<std::string> split(std::string_view text, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = text.find(delimiter, start);
    if (position == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, position - start));
    start = position + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, char delimiter) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.push_back(delimiter);
    }
    out.append(parts[i]);
  }
  return out;
}

bool canonicalize(std::vector<std::string>& values) {
  const std::vector<std::string> original = values;
  std::sort(values.begin(), values.end(), [](const std::string& a, const std::string& b) {
    return byte_less(a, b);
  });
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values == original;
}

}  // namespace rackregistry
