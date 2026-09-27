// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/strong_id.hpp"
#include "dccp/facility_topology/text.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

FT_TEST(text, utf8_accepts_valid_sequences) {
  const std::vector<std::string> valid = {
      "",
      "plain ascii",
      "\xC2\xA9",                              // U+00A9
      "\xE2\x82\xAC",                          // U+20AC
      "\xF0\x9F\x98\x80",                      // U+1F600
      "a\xC3\xA9z",                            // mixed
      "\xED\x9F\xBF",                          // U+D7FF, just below the surrogate range
      "\xEE\x80\x80",                          // U+E000, just above the surrogate range
      "\xF4\x8F\xBF\xBF",                      // U+10FFFF, the last code point
  };
  for (const std::string& input : valid) {
    FT_CHECK(text::is_valid_utf8(input));
  }
}

FT_TEST(text, utf8_rejects_invalid_sequences) {
  const std::vector<std::string> invalid = {
      "\x80",                  // lone continuation byte
      "\xC0\x80",              // overlong NUL
      "\xC1\xBF",              // overlong
      "\xE0\x80\x80",          // overlong
      "\xF0\x80\x80\x80",      // overlong
      "\xC2",                  // truncated
      "\xE2\x82",              // truncated
      "\xF0\x9F\x98",          // truncated
      "\xED\xA0\x80",          // UTF-16 surrogate half U+D800
      "\xED\xBF\xBF",          // UTF-16 surrogate half U+DFFF
      "\xF4\x90\x80\x80",      // above U+10FFFF
      "\xF8\x88\x80\x80\x80",  // five byte sequence
      "\xFF",                  // invalid lead byte
      "ok\xFF",
  };
  for (const std::string& input : invalid) {
    FT_CHECK(!text::is_valid_utf8(input));
  }
}

FT_TEST(text, control_characters_and_labels) {
  FT_CHECK(text::contains_control_characters(std::string("a\nb")));
  FT_CHECK(text::contains_control_characters(std::string("a\x7F")));
  FT_CHECK(text::contains_control_characters(std::string("\x01")));
  FT_CHECK(!text::contains_control_characters("plain text"));

  FT_CHECK(text::is_valid_label("Rack 01", 64));
  FT_CHECK(text::is_valid_label("", 64));
  FT_CHECK(!text::is_valid_label(" leading", 64));
  FT_CHECK(!text::is_valid_label("trailing ", 64));
  FT_CHECK(!text::is_valid_label("tab\t", 64));
  FT_CHECK(!text::is_valid_label("newline\n", 64));
  FT_CHECK(!text::is_valid_label(std::string(65, 'a'), 64));
  FT_CHECK(!text::is_valid_label("\xFF", 64));
}

FT_TEST(text, quoting_round_trip) {
  const std::vector<std::string> samples = {"", "plain", "with \"quotes\"", "back\\slash", "both \\\" mixed"};
  for (const std::string& sample : samples) {
    const std::string escaped = text::escape_quoted(sample);
    auto restored = text::unescape_quoted(escaped, 4096);
    FT_REQUIRE(restored.has_value());
    FT_CHECK_EQ(restored.value(), sample);
  }
  FT_CHECK_ERROR(text::unescape_quoted("trailing\\", 4096), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(text::unescape_quoted("bad\\nescape", 4096), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(text::unescape_quoted("toolong", 3), ErrorCode::TextTooLong);
}

FT_TEST(text, decimal_parsing_is_strict) {
  FT_CHECK_EQ(text::parse_u64("0").value(), 0U);
  FT_CHECK_EQ(text::parse_u64("1").value(), 1U);
  FT_CHECK_EQ(text::parse_u64("18446744073709551615").value(), UINT64_MAX);
  FT_CHECK(!text::parse_u64("").has_value());
  FT_CHECK(!text::parse_u64("00").has_value());
  FT_CHECK(!text::parse_u64("01").has_value());
  FT_CHECK(!text::parse_u64("+1").has_value());
  FT_CHECK(!text::parse_u64("-1").has_value());
  FT_CHECK(!text::parse_u64(" 1").has_value());
  FT_CHECK(!text::parse_u64("1 ").has_value());
  FT_CHECK(!text::parse_u64("1a").has_value());
  FT_CHECK(!text::parse_u64("18446744073709551616").has_value());
  FT_CHECK(!text::parse_u64("99999999999999999999999").has_value());

  for (std::uint64_t value : {0ULL, 1ULL, 9ULL, 10ULL, 4294967296ULL, UINT64_MAX}) {
    FT_CHECK_EQ(text::parse_u64(text::format_u64(value)).value(), value);
  }
}

FT_TEST(text, hexadecimal_round_trip) {
  const std::string bytes = std::string("\x00\x01\x7F\x80\xFF", 5);
  const std::string encoded = text::to_hex(bytes);
  FT_CHECK_EQ(encoded, std::string("00017f80ff"));
  auto decoded = text::from_hex(encoded, 16);
  FT_REQUIRE(decoded.has_value());
  FT_CHECK_EQ(decoded->size(), std::size_t{5});
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    FT_CHECK_EQ((*decoded)[index], static_cast<std::uint8_t>(bytes[index]));
  }
  FT_CHECK_ERROR(text::from_hex("0", 16), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(text::from_hex("zz", 16), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(text::from_hex("0011223344", 2), ErrorCode::LimitExceeded);
  FT_CHECK_EQ(text::to_hex(std::string()).size(), std::size_t{0});
}

FT_TEST(text, tokenizing_and_trimming) {
  const std::vector<std::string_view> tokens = text::split_tokens("  alpha\tbeta  gamma ");
  FT_REQUIRE(tokens.size() == 3);
  FT_CHECK_EQ(tokens[0], std::string_view("alpha"));
  FT_CHECK_EQ(tokens[1], std::string_view("beta"));
  FT_CHECK_EQ(tokens[2], std::string_view("gamma"));
  FT_CHECK_EQ(text::split_tokens("   ").size(), std::size_t{0});
  const std::string trimmed_x = std::string(text::trim("  x  "));
  FT_CHECK_EQ(trimmed_x, std::string("x"));
  const std::string trimmed_crlf = std::string(text::trim("\r\n"));
  FT_CHECK_EQ(trimmed_crlf, std::string("\n"));
  FT_CHECK(text::byte_less("a", "b"));
  FT_CHECK(!text::byte_less("b", "a"));
  FT_CHECK(text::to_lower_ascii("AbC\xC3\x89") == std::string("abc\xC3\x89"));
}

FT_TEST(text, identifier_syntax) {
  const std::vector<std::string> valid = {"a", "A1", "rack-01", "fac.1:zone-a", std::string(128, 'a')};
  for (const std::string& value : valid) {
    FT_CHECK(is_valid_identifier_syntax(value));
    auto parsed = NodeId::parse(value);
    FT_REQUIRE(parsed.has_value());
    FT_CHECK_EQ(parsed->str(), value);
  }
  const std::vector<std::string> invalid = {"",
                                            "-lead",
                                            "trail-",
                                            ".lead",
                                            "a b",
                                            "a/b",
                                            "a\\b",
                                            "a\nb",
                                            "a\"b",
                                            std::string(129, 'a'),
                                            "caf\xC3\xA9"};
  for (const std::string& value : invalid) {
    FT_CHECK(!is_valid_identifier_syntax(value));
    FT_CHECK_ERROR(NodeId::parse(value), ErrorCode::MalformedIdentifier);
  }
  FT_CHECK(!identifier_syntax_help().empty());
}

FT_TEST(text, identity_types_are_distinct_and_ordered) {
  const NodeId first = node_id("a");
  const NodeId second = node_id("b");
  FT_CHECK(first < second);
  FT_CHECK(second > first);
  FT_CHECK(first == node_id("a"));
  FT_CHECK(first != second);
  FT_CHECK(NodeId().empty());
  FT_CHECK_EQ(NodeId().value(), std::string_view(""));

  const DomainId domain = DomainId::parse("pwr-a").value();
  FT_CHECK_EQ(domain.str(), std::string("pwr-a"));
  FT_CHECK(std::hash<NodeId>{}(first) != std::hash<NodeId>{}(second));
}
