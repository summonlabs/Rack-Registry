// Rack Registry - SHA-256 and canonical digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/digest.hpp"

#include <array>
#include <cstring>

#include "rack_registry/text.hpp"

namespace rackregistry {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32u - shift));
}

}  // namespace

void Sha256::reset() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  total_bytes_ = 0;
  buffered_ = 0;
  finalized_ = false;
}

void Sha256::process_block(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                  (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                  (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                  static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^ rotate_right(schedule[i - 15], 18) ^
                             (schedule[i - 15] >> 3);
    const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^
                             (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + sigma1 + choice + kRoundConstants[i] + schedule[i];
    const std::uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
  if (finalized_ || data == nullptr || size == 0) {
    return;
  }
  total_bytes_ += size;
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t needed = kBlockBytes - buffered_;
    const std::size_t take = size < needed ? size : needed;
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == kBlockBytes) {
      process_block(buffer_.data());
      buffered_ = 0;
    }
  }
  while (offset + kBlockBytes <= size) {
    process_block(data + offset);
    offset += kBlockBytes;
  }
  if (offset < size) {
    const std::size_t remaining = size - offset;
    std::memcpy(buffer_.data() + buffered_, data + offset, remaining);
    buffered_ += remaining;
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Sha256::update(const std::vector<std::uint8_t>& data) noexcept {
  update(data.data(), data.size());
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::finish() noexcept {
  std::array<std::uint8_t, kDigestBytes> digest{};
  if (finalized_) {
    for (std::size_t i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
      digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
      digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
      digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
  }

  const std::uint64_t total_bits = total_bytes_ * 8u;
  const std::uint8_t padding = 0x80;
  update(&padding, 1);
  const std::uint8_t zero = 0x00;
  while (buffered_ != kBlockBytes - 8) {
    update(&zero, 1);
  }
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>(total_bits >> (56 - i * 8));
  }
  update(length_bytes.data(), length_bytes.size());
  finalized_ = true;

  for (std::size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
    digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
    digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
  }
  return digest;
}

std::array<std::uint8_t, Sha256::kDigestBytes> sha256(const std::uint8_t* data, std::size_t size) {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

StateDigest StateDigest::of(const std::uint8_t* data, std::size_t size) {
  return StateDigest(sha256(data, size));
}

StateDigest StateDigest::of(const std::vector<std::uint8_t>& data) {
  return StateDigest::of(data.data(), data.size());
}

StateDigest StateDigest::domain(std::string_view domain_label,
                                const std::vector<std::uint8_t>& payload) {
  Sha256 hasher;
  const std::uint8_t separator = 0x1F;
  hasher.update(domain_label);
  hasher.update(&separator, 1);
  const std::array<std::uint8_t, 8> length_bytes = {
      static_cast<std::uint8_t>(payload.size() >> 56),
      static_cast<std::uint8_t>(payload.size() >> 48),
      static_cast<std::uint8_t>(payload.size() >> 40),
      static_cast<std::uint8_t>(payload.size() >> 32),
      static_cast<std::uint8_t>(payload.size() >> 24),
      static_cast<std::uint8_t>(payload.size() >> 16),
      static_cast<std::uint8_t>(payload.size() >> 8),
      static_cast<std::uint8_t>(payload.size())};
  hasher.update(length_bytes.data(), length_bytes.size());
  hasher.update(payload);
  return StateDigest(hasher.finish());
}

Result<StateDigest> StateDigest::from_hex(std::string_view hex) {
  if (hex.size() != Sha256::kDigestBytes * 2) {
    return make_error(ErrorCode::InvalidArgument, "digest hex text must be 64 characters",
                      ErrorDetail{.operation = "StateDigest",
                                  .expected = Sha256::kDigestBytes * 2,
                                  .actual = hex.size()});
  }
  std::array<std::uint8_t, Sha256::kDigestBytes> bytes{};
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t i = 0; i < Sha256::kDigestBytes; ++i) {
    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return make_error(ErrorCode::InvalidCharacter, "digest hex text contains a non-hex character",
                        ErrorDetail{.operation = "StateDigest", .subject = std::string(hex)});
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return StateDigest(bytes);
}

std::string StateDigest::to_hex() const { return rackregistry::to_hex(bytes_.data(), bytes_.size()); }

bool StateDigest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace rackregistry
