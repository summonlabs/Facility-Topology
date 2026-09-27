// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_TEXT_HPP
#define DCCP_FACILITY_TOPOLOGY_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology::text {

/// Strict UTF-8 validation: rejects overlong encodings, UTF-16 surrogates,
/// code points above U+10FFFF, and truncated sequences.
bool is_valid_utf8(std::string_view input) noexcept;

/// True when the input contains an ASCII control byte (0x00..0x1F, 0x7F).
bool contains_control_characters(std::string_view input) noexcept;

/// Validates a human-readable label: valid UTF-8, no control characters, no
/// leading or trailing whitespace ambiguity, at most max_bytes bytes.
bool is_valid_label(std::string_view input, std::size_t max_bytes) noexcept;

/// Escapes '"' and '\' inside a quoted canonical field. Labels are validated to
/// contain no control characters, so no other escape is required or emitted.
std::string escape_quoted(std::string_view input);

/// Inverse of escape_quoted(); rejects unknown escapes and truncated escapes.
Result<std::string> unescape_quoted(std::string_view input, std::size_t max_bytes);

/// ASCII lower-casing (bytes >= 0x80 are left untouched).
std::string to_lower_ascii(std::string_view input);

/// Strict unsigned decimal parsing: no sign, no whitespace, no leading zeros
/// (except the literal "0"), overflow rejected.
std::optional<std::uint64_t> parse_u64(std::string_view input) noexcept;

/// Canonical unsigned decimal formatting.
std::string format_u64(std::uint64_t value);

/// Lower-case hexadecimal encoding.
std::string to_hex(const std::uint8_t* data, std::size_t size);
std::string to_hex(std::string_view bytes);

/// Strict lower/upper-case hexadecimal decoding with an output bound.
Result<std::vector<std::uint8_t>> from_hex(std::string_view input, std::size_t max_bytes);

/// Splits a line into whitespace-separated tokens (no quoting).
std::vector<std::string_view> split_tokens(std::string_view line);

/// Trims ASCII whitespace from both ends.
std::string_view trim(std::string_view input) noexcept;

/// Lexicographic byte-wise comparison helper used for canonical ordering.
bool byte_less(std::string_view lhs, std::string_view rhs) noexcept;

}  // namespace dccp::facility_topology::text

#endif  // DCCP_FACILITY_TOPOLOGY_TEXT_HPP
