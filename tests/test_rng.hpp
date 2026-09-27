// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic pseudo-random source for property tests.
//
// This is a xoshiro256** generator seeded through splitmix64. It is used only
// to generate test inputs: it is not part of the library and makes no
// cryptographic claim. Same seed, same sequence, on every platform.

#ifndef FACILITY_TOPOLOGY_TESTS_TEST_RNG_HPP
#define FACILITY_TOPOLOGY_TESTS_TEST_RNG_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace ftest {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept { reseed(seed); }

  void reseed(std::uint64_t seed) noexcept {
    std::uint64_t state = seed;
    for (std::size_t index = 0; index < 4; ++index) {
      state += 0x9E3779B97F4A7C15ULL;
      std::uint64_t z = state;
      z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
      words_[index] = z ^ (z >> 31U);
    }
  }

  std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(words_[1] * 5U, 7U) * 9U;
    const std::uint64_t t = words_[1] << 17U;
    words_[2] ^= words_[0];
    words_[3] ^= words_[1];
    words_[1] ^= words_[2];
    words_[0] ^= words_[3];
    words_[2] ^= t;
    words_[3] = rotl(words_[3], 45U);
    return result;
  }

  /// Uniform value in [0, bound). Returns 0 when bound is 0.
  std::uint64_t below(std::uint64_t bound) noexcept {
    if (bound == 0) {
      return 0;
    }
    return next() % bound;
  }

  std::size_t index(std::size_t bound) noexcept { return static_cast<std::size_t>(below(bound)); }

  /// True with probability numerator/denominator.
  bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    if (denominator == 0) {
      return false;
    }
    return below(denominator) < numerator;
  }

  /// A short identifier-safe token.
  std::string token(std::size_t length) {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    out.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      out.push_back(kAlphabet[below(sizeof(kAlphabet) - 1)]);
    }
    return out;
  }

  /// Arbitrary bytes, including invalid UTF-8 and control characters.
  std::string bytes(std::size_t length) {
    std::string out;
    out.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      out.push_back(static_cast<char>(below(256)));
    }
    return out;
  }

 private:
  static std::uint64_t rotl(std::uint64_t value, unsigned count) noexcept {
    return (value << count) | (value >> (64U - count));
  }

  std::uint64_t words_[4] = {0, 0, 0, 0};
};

}  // namespace ftest

#endif  // FACILITY_TOPOLOGY_TESTS_TEST_RNG_HPP
