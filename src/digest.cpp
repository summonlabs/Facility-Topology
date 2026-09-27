// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/digest.hpp"

#include <cstring>

#include "dccp/facility_topology/text.hpp"

namespace dccp::facility_topology {
namespace {

// FIPS 180-4 round constants: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 2U) ^ rotr(x, 13U) ^ rotr(x, 22U);
}

constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 6U) ^ rotr(x, 11U) ^ rotr(x, 25U);
}

constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept { return rotr(x, 7U) ^ rotr(x, 18U) ^ (x >> 3U); }

constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept { return rotr(x, 17U) ^ rotr(x, 19U) ^ (x >> 10U); }

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U},
      buffer_{},
      total_bytes_(0),
      buffered_(0),
      finished_(false) {}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) | static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
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
    const std::uint32_t t1 = h + big_sigma1(e) + ((e & f) ^ (~e & g)) + kRoundConstants[i] + w[i];
    const std::uint32_t t2 = big_sigma0(a) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
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
  if (finished_ || size == 0) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(size);
  std::size_t offset = 0;
  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    const std::size_t take = (size < need) ? size : need;
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (size - offset >= 64) {
    compress(data + offset);
    offset += 64;
  }
  if (offset < size) {
    const std::size_t remaining = size - offset;
    std::memcpy(buffer_.data(), data + offset, remaining);
    buffered_ = remaining;
  }
}

Digest Sha256::finish() noexcept {
  Digest out{};
  if (finished_) {
    return out;
  }
  finished_ = true;

  // Padding: a single 0x80 byte, then zeros, then the 64-bit big-endian bit
  // length, so that the final block ends with the length in its last 8 bytes.
  // The length used here is the message length, so padding is appended
  // directly instead of going through update().
  const std::uint64_t bit_length = total_bytes_ * 8U;
  std::array<std::uint8_t, 128> tail{};
  std::size_t tail_length = 0;
  tail[tail_length++] = 0x80U;
  const std::size_t zero_count = (buffered_ < 56) ? (55 - buffered_) : (119 - buffered_);
  tail_length += zero_count;  // already zero-initialized
  for (unsigned i = 0; i < 8; ++i) {
    tail[tail_length++] = static_cast<std::uint8_t>((bit_length >> (56U - 8U * i)) & 0xFFU);
  }
  for (std::size_t i = 0; i < tail_length; ++i) {
    buffer_[buffered_++] = tail[i];
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24U) & 0xFFU);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16U) & 0xFFU);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8U) & 0xFFU);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
  }
  return out;
}

std::string digest_hex(const Digest& digest) { return text::to_hex(digest.data(), digest.size()); }

Result<Digest> digest_parse(std::string_view value) {
  if (value.size() != kDigestBytes * 2) {
    return Error(ErrorCode::MalformedRecord, "digest must be exactly 64 hexadecimal characters")
        .with_subject(std::string(value.substr(0, 80)));
  }
  FT_TRY(bytes, text::from_hex(value, kDigestBytes));
  Digest out{};
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    out[i] = bytes[i];
  }
  return out;
}

Digest digest_of(std::string_view bytes) noexcept {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

std::string digest_tagged_hex(const Digest& digest) {
  std::string out(kDigestAlgorithmToken);
  out.push_back(':');
  out.append(digest_hex(digest));
  return out;
}

Result<Digest> digest_parse_tagged(std::string_view value) {
  const std::size_t colon = value.find(':');
  if (colon == std::string_view::npos) {
    return Error(ErrorCode::MalformedRecord, "digest must be written as \"sha256:<64 hex characters>\"");
  }
  const std::string_view algorithm = value.substr(0, colon);
  if (algorithm != kDigestAlgorithmToken) {
    return Error(ErrorCode::MalformedRecord, "unsupported digest algorithm").with_subject(std::string(algorithm));
  }
  return digest_parse(value.substr(colon + 1));
}

bool digest_is_zero(const Digest& digest) noexcept {
  for (const std::uint8_t byte : digest) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

bool digest_equal(const Digest& lhs, const Digest& rhs) noexcept { return lhs == rhs; }

}  // namespace dccp::facility_topology
