// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test framework.
//
// The framework deliberately has no external dependencies and no timing logic:
// every test runs to completion. Property tests take an explicit seed so that a
// failing case can be reproduced exactly.

#ifndef FACILITY_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP
#define FACILITY_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "dccp/facility_topology/result.hpp"

namespace ftest {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

std::vector<TestCase>& registry();
int register_test(const char* suite, const char* name, void (*function)());

/// Thrown by FT_REQUIRE to abandon the remainder of a test body.
struct TestAborted {};

void fail(const char* file, int line, const std::string& message);
void note(const std::string& message);

/// Runs the whole suite. Returns the process exit status.
int run_all(int argc, char** argv);

/// Seed used by property tests in the current run.
std::uint64_t current_seed();

/// Prints a reproducible seed marker on failure.
void set_current_case_context(const std::string& context);

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) { register_test(suite, name, function); }
};

}  // namespace ftest

#define FT_TEST(suite_name, case_name)                                                       \
  static void suite_name##_##case_name##_body();                                             \
  static const ::ftest::Registrar suite_name##_##case_name##_registrar(#suite_name,          \
                                                                      #case_name,            \
                                                                      &suite_name##_##case_name##_body); \
  static void suite_name##_##case_name##_body()

#define FT_FAIL(message) ::ftest::fail(__FILE__, __LINE__, (message))

#define FT_CHECK(condition)                                                    \
  do {                                                                         \
    const bool ftest_ok = static_cast<bool>(condition);                        \
    if (!ftest_ok) {                                                           \
      FT_FAIL(std::string("CHECK failed: ") + #condition);                      \
    }                                                                          \
  } while (false)

#define FT_REQUIRE(condition)                                                  \
  do {                                                                         \
    const bool ftest_ok = static_cast<bool>(condition);                        \
    if (!ftest_ok) {                                                           \
      FT_FAIL(std::string("REQUIRE failed: ") + #condition);                    \
      throw ::ftest::TestAborted{};                                            \
    }                                                                          \
  } while (false)

#define FT_CHECK_EQ(actual, expected)                                                              \
  do {                                                                                             \
    const auto ftest_actual = (actual);                                                            \
    const auto ftest_expected = (expected);                                                        \
    const bool ftest_equal = (ftest_actual == ftest_expected);                                     \
    if (!ftest_equal) {                                                                            \
      std::ostringstream ftest_stream;                                                             \
      ftest_stream << "CHECK_EQ failed: " #actual " == " #expected " (actual=" << ::ftest::render(ftest_actual) \
                   << ", expected=" << ::ftest::render(ftest_expected) << ")";                     \
      FT_FAIL(ftest_stream.str());                                                                 \
    }                                                                                              \
  } while (false)

#define FT_CHECK_NE(actual, expected)                                                              \
  do {                                                                                             \
    const auto ftest_actual = (actual);                                                            \
    const auto ftest_expected = (expected);                                                        \
    const bool ftest_equal = (ftest_actual == ftest_expected);                                     \
    if (ftest_equal) {                                                                             \
      std::ostringstream ftest_stream;                                                             \
      ftest_stream << "CHECK_NE failed: " #actual " != " #expected;                                \
      FT_FAIL(ftest_stream.str());                                                                 \
    }                                                                                              \
  } while (false)

/// Asserts that a Result failed with a specific stable error code.
#define FT_CHECK_ERROR(expression, expected_code)                                                  \
  do {                                                                                             \
    const auto& ftest_result = (expression);                                                       \
    if (ftest_result.has_value()) {                                                                \
      FT_FAIL(std::string("expected failure ") + #expected_code + " but the call succeeded: " #expression); \
    } else if (ftest_result.error().code() != (expected_code)) {                                   \
      std::ostringstream ftest_stream;                                                             \
      ftest_stream << "expected " << ::dccp::facility_topology::error_code_name(expected_code)      \
                   << " but got " << ftest_result.error().to_string();                             \
      FT_FAIL(ftest_stream.str());                                                                 \
    }                                                                                              \
  } while (false)

/// Asserts that a Result succeeded.
#define FT_CHECK_OK(expression)                                                                    \
  do {                                                                                             \
    const auto& ftest_result = (expression);                                                       \
    if (!ftest_result.has_value()) {                                                               \
      FT_FAIL(std::string("expected success but got ") + ftest_result.error().to_string());        \
    }                                                                                              \
  } while (false)

/// Asserts that a Result succeeded, reporting the error and abandoning the
/// remainder of the test body when it did not.
#define FT_REQUIRE_OK(expression)                                                                  \
  do {                                                                                             \
    const auto& ftest_result = (expression);                                                       \
    if (!ftest_result.has_value()) {                                                               \
      FT_FAIL(std::string("REQUIRE_OK failed: ") + #expression + " -> " + ftest_result.error().to_string()); \
      throw ::ftest::TestAborted{};                                                                \
    }                                                                                              \
  } while (false)

namespace ftest {

std::string render(const std::string& value);
std::string render(std::string_view value);
std::string render(const char* value);
std::string render(bool value);
std::string render(const std::error_code& value);

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// Renders any value for a failure message: streamable values directly,
/// iterable values element by element, and anything else as a placeholder.
template <class T>
std::string render(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (requires { std::begin(value); std::end(value); }) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : value) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append(render(item));
    }
    out.push_back(']');
    return out;
  } else {
    return "<value>";
  }
}

}  // namespace ftest

#endif  // FACILITY_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP
