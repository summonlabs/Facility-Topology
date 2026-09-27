// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

#include "dccp/facility_topology/strong_id.hpp"

namespace dccp::facility_topology {

bool is_valid_identifier_syntax(std::string_view raw) noexcept {
  if (raw.empty() || raw.size() > kMaxIdentifierBytes) {
    return false;
  }
  const auto alnum = [](char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };
  const auto interior = [&alnum](char c) noexcept { return alnum(c) || c == '.' || c == ':' || c == '-'; };
  if (!alnum(raw.front()) || !alnum(raw.back())) {
    return false;
  }
  for (const char c : raw) {
    if (!interior(c)) {
      return false;
    }
  }
  return true;
}

std::string_view identifier_syntax_help() noexcept {
  return "identifiers are 1..128 bytes, start and end with [0-9A-Za-z], and contain only [0-9A-Za-z._:-]";
}

namespace text {
namespace {

constexpr bool is_continuation(unsigned char byte) noexcept { return (byte & 0xC0U) == 0x80U; }

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
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
}

}  // namespace

bool is_valid_utf8(std::string_view input) noexcept {
  std::size_t i = 0;
  const std::size_t n = input.size();
  while (i < n) {
    const auto b0 = static_cast<unsigned char>(input[i]);
    if (b0 < 0x80U) {
      ++i;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = b0 & 0x1FU;
    } else if ((b0 & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = b0 & 0x0FU;
    } else if ((b0 & 0xF8U) == 0xF0U) {
      extra = 3;
      code_point = b0 & 0x07U;
    } else {
      return false;  // continuation byte or invalid lead
    }
    if (i + extra >= n) {
      return false;  // truncated sequence
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto bk = static_cast<unsigned char>(input[i + k]);
      if (!is_continuation(bk)) {
        return false;
      }
      code_point = (code_point << 6U) | (bk & 0x3FU);
    }
    const std::uint32_t minimum = (extra == 1) ? 0x80U : (extra == 2 ? 0x800U : 0x10000U);
    if (code_point < minimum) {
      return false;  // overlong encoding
    }
    if (code_point > 0x10FFFFU) {
      return false;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;  // surrogate half
    }
    i += extra + 1;
  }
  return true;
}

bool contains_control_characters(std::string_view input) noexcept {
  for (const char c : input) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20U || byte == 0x7FU) {
      return true;
    }
  }
  return false;
}

bool is_valid_label(std::string_view input, std::size_t max_bytes) noexcept {
  if (input.size() > max_bytes) {
    return false;
  }
  if (contains_control_characters(input)) {
    return false;
  }
  if (!is_valid_utf8(input)) {
    return false;
  }
  if (!input.empty()) {
    const char front = input.front();
    const char back = input.back();
    if (front == ' ' || front == '\t' || back == ' ' || back == '\t') {
      return false;
    }
  }
  return true;
}

std::string escape_quoted(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (const char c : input) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

Result<std::string> unescape_quoted(std::string_view input, std::size_t max_bytes) {
  std::string out;
  out.reserve(std::min(input.size(), max_bytes));
  for (std::size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (c == '\\') {
      if (i + 1 >= input.size()) {
        return Error(ErrorCode::MalformedRecord, "truncated escape sequence in quoted field");
      }
      const char escaped = input[++i];
      if (escaped != '"' && escaped != '\\') {
        return Error(ErrorCode::MalformedRecord, "unsupported escape sequence in quoted field")
            .with_subject(std::string(1, escaped));
      }
      out.push_back(escaped);
    } else {
      out.push_back(c);
    }
    if (out.size() > max_bytes) {
      return Error(ErrorCode::TextTooLong, "quoted field exceeds the configured maximum length");
    }
  }
  return out;
}

std::string to_lower_ascii(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (const char c : input) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::optional<std::uint64_t> parse_u64(std::string_view input) noexcept {
  if (input.empty() || input.size() > 20) {
    return std::nullopt;
  }
  if (input.size() > 1 && input.front() == '0') {
    return std::nullopt;  // canonical form has no leading zeros
  }
  std::uint64_t value = 0;
  for (const char c : input) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    value = value * 10U + digit;
  }
  return value;
}

std::string format_u64(std::uint64_t value) {
  if (value == 0) {
    return "0";
  }
  std::array<char, 20> buffer{};
  std::size_t pos = buffer.size();
  while (value != 0) {
    buffer[--pos] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  }
  return std::string(buffer.data() + pos, buffer.size() - pos);
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kHexDigits[data[i] >> 4U]);
    out.push_back(kHexDigits[data[i] & 0x0FU]);
  }
  return out;
}

std::string to_hex(std::string_view bytes) {
  return to_hex(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view input, std::size_t max_bytes) {
  if ((input.size() % 2) != 0) {
    return Error(ErrorCode::MalformedRecord, "hexadecimal field has an odd number of digits");
  }
  const std::size_t byte_count = input.size() / 2;
  if (byte_count > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "hexadecimal field exceeds the configured maximum size");
  }
  std::vector<std::uint8_t> out;
  out.reserve(byte_count);
  for (std::size_t i = 0; i < byte_count; ++i) {
    const int high = hex_value(input[i * 2]);
    const int low = hex_value(input[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return Error(ErrorCode::MalformedRecord, "hexadecimal field contains a non-hexadecimal digit");
    }
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return out;
}

std::vector<std::string_view> split_tokens(std::string_view line) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
      ++i;
    }
    if (i > start) {
      tokens.push_back(line.substr(start, i - start));
    }
  }
  return tokens;
}

std::string_view trim(std::string_view input) noexcept {
  std::size_t start = 0;
  std::size_t end = input.size();
  while (start < end && (input[start] == ' ' || input[start] == '\t' || input[start] == '\r')) {
    ++start;
  }
  while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t' || input[end - 1] == '\r')) {
    --end;
  }
  return input.substr(start, end - start);
}

bool byte_less(std::string_view lhs, std::string_view rhs) noexcept { return lhs < rhs; }

}  // namespace text

}  // namespace dccp::facility_topology
