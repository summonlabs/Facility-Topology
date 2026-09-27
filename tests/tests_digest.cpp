// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dccp/facility_topology/digest.hpp"
#include "test_support.hpp"

using namespace dccp::facility_topology;
using namespace ftest;

namespace {

std::string hex_string(const Digest& digest) { return digest_hex(digest); }

std::string repeat(std::string_view unit, std::size_t count) {
  std::string out;
  out.reserve(unit.size() * count);
  for (std::size_t index = 0; index < count; ++index) {
    out.append(unit);
  }
  return out;
}

}  // namespace

// Published FIPS 180-4 / NIST example vectors.
FT_TEST(digest, known_answer_vectors) {
  FT_CHECK_EQ(hex_string(digest_of("")),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FT_CHECK_EQ(hex_string(digest_of("abc")),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FT_CHECK_EQ(hex_string(digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  FT_CHECK_EQ(
      hex_string(digest_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
      std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
  FT_CHECK_EQ(hex_string(digest_of(repeat("a", 1000000))),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

FT_TEST(digest, block_boundaries) {
  // Byte counts around the 64-byte block and the 56-byte padding boundary must
  // all agree between the one-shot and streaming paths.
  const std::vector<std::size_t> sizes = {0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 1000};
  for (const std::size_t size : sizes) {
    const std::string input = repeat("x", size);
    Sha256 streaming;
    for (std::size_t index = 0; index < input.size(); ++index) {
      streaming.update(input.data() + index, 1);
    }
    FT_CHECK_EQ(hex_string(streaming.finish()), hex_string(digest_of(input)));

    Sha256 chunked;
    std::size_t offset = 0;
    while (offset < input.size()) {
      const std::size_t chunk = std::min<std::size_t>(7, input.size() - offset);
      chunked.update(input.data() + offset, chunk);
      offset += chunk;
    }
    FT_CHECK_EQ(hex_string(chunked.finish()), hex_string(digest_of(input)));
  }
}

FT_TEST(digest, text_and_parse_round_trip) {
  const Digest digest = digest_of("facility");
  const std::string hex = digest_hex(digest);
  FT_CHECK_EQ(hex.size(), std::size_t{64});
  auto parsed = digest_parse(hex);
  FT_REQUIRE(parsed.has_value());
  FT_CHECK(digest_equal(*parsed, digest));

  auto upper = digest_parse("B858BB1E13D3B3AF31AB1B07E7D3D6EA4D6DBD1D9FBF1D0BC1B9BC4AFE0D0D8B");
  FT_CHECK(upper.has_value());

  FT_CHECK_ERROR(digest_parse(""), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(digest_parse(std::string(63, 'a')), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(digest_parse(std::string(66, 'a')), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(digest_parse(std::string(64, 'z')), ErrorCode::MalformedRecord);

  const std::string tagged = digest_tagged_hex(digest);
  FT_CHECK_EQ(tagged.substr(0, 7), std::string("sha256:"));
  auto parsed_tagged = digest_parse_tagged(tagged);
  FT_REQUIRE(parsed_tagged.has_value());
  FT_CHECK(digest_equal(*parsed_tagged, digest));
  FT_CHECK_ERROR(digest_parse_tagged(hex), ErrorCode::MalformedRecord);
  FT_CHECK_ERROR(digest_parse_tagged("sha512:" + hex), ErrorCode::MalformedRecord);
}

FT_TEST(digest, zero_and_equality) {
  const Digest zero{};
  FT_CHECK(digest_is_zero(zero));
  FT_CHECK(!digest_is_zero(digest_of("")));
  FT_CHECK(digest_equal(zero, Digest{}));
  FT_CHECK(!digest_equal(zero, digest_of("")));

  Sha256 reused;
  reused.update("abc");
  const Digest first = reused.finish();
  const Digest second = reused.finish();
  FT_CHECK(digest_is_zero(second));
  FT_CHECK(!digest_is_zero(first));
  reused.update("abc");
  FT_CHECK(digest_is_zero(reused.finish()));
}
