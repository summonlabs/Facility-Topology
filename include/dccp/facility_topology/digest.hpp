// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_DIGEST_HPP
#define DCCP_FACILITY_TOPOLOGY_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology {

/// Length in bytes of the integrity digest used by durable topology state.
inline constexpr std::size_t kDigestBytes = 32;

/// A SHA-256 integrity digest.
///
/// Facility Topology uses this digest to detect corruption and tampering of
/// persisted authoritative state, and to make canonical documents
/// content-addressable. It is not used as a key or a password primitive.
using Digest = std::array<std::uint8_t, kDigestBytes>;

/// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256() noexcept;

  void update(const std::uint8_t* data, std::size_t size) noexcept;
  void update(std::string_view bytes) noexcept { update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()); }
  /// Convenience overload for character buffers that are not string views.
  void update(const char* data, std::size_t size) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(data), size);
  }

  /// Finalizes the digest. The object is left unusable; construct a new one to
  /// compute another digest.
  Digest finish() noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_;
  std::uint64_t total_bytes_;
  std::size_t buffered_;
  bool finished_;
};

/// Lower-case hexadecimal encoding of a digest (64 characters).
std::string digest_hex(const Digest& digest);

/// Strictly parses 64 lower/upper-case hexadecimal characters.
Result<Digest> digest_parse(std::string_view text);

/// Convenience: digest of a byte range.
Digest digest_of(std::string_view bytes) noexcept;

/// "sha256:<hex>" prefix used by canonical documents.
inline constexpr std::string_view kDigestAlgorithmToken = "sha256";

/// Formats "sha256:<hex>".
std::string digest_tagged_hex(const Digest& digest);

/// Parses "sha256:<hex>"; rejects any other algorithm token.
Result<Digest> digest_parse_tagged(std::string_view text);

/// True when every byte is zero. The all-zero digest means "absent".
bool digest_is_zero(const Digest& digest) noexcept;

/// Comparison helper (never use operator== on a digest by accident in
/// security-relevant code paths; this is an ordinary equality, not constant
/// time, and is documented as such).
bool digest_equal(const Digest& lhs, const Digest& rhs) noexcept;

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_DIGEST_HPP
