// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "child_process.hpp"

namespace ftest {
namespace {

std::string g_current_case;
std::uint64_t g_seed = 20260201ULL;
int g_failures_in_case = 0;
bool g_case_failed = false;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void fail(const char* file, int line, const std::string& message) {
  ++g_failures_in_case;
  g_case_failed = true;
  std::cerr << "FAIL " << g_current_case << "\n  " << file << ":" << line << ": " << message << "\n"
            << "  seed=" << g_seed << "\n";
}

void note(const std::string& message) { std::cerr << "NOTE " << g_current_case << ": " << message << "\n"; }

std::uint64_t current_seed() { return g_seed; }

void set_current_case_context(const std::string& context) { g_current_case = context; }

std::string render(const std::string& value) { return "\"" + value + "\""; }
std::string render(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string render(const char* value) { return value == nullptr ? "(null)" : std::string("\"") + value + "\""; }
std::string render(bool value) { return value ? "true" : "false"; }
std::string render(const std::error_code& value) { return value.message(); }

int run_all(int argc, char** argv) {
  std::vector<std::string> filters;
  std::vector<std::string> arguments(argv + 1, argv + argc);
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& argument = arguments[index];
    if (argument == "--list") {
      for (const TestCase& test : registry()) {
        std::cout << test.suite << "." << test.name << "\n";
      }
      return 0;
    }
    if (argument.rfind("--filter=", 0) == 0) {
      filters.push_back(argument.substr(9));
      continue;
    }
    if (argument == "--filter" && index + 1 < arguments.size()) {
      filters.push_back(arguments[++index]);
      continue;
    }
    if (argument.rfind("--seed=", 0) == 0) {
      g_seed = std::strtoull(argument.substr(7).c_str(), nullptr, 10);
      continue;
    }
    if (argument == "--seed" && index + 1 < arguments.size()) {
      g_seed = std::strtoull(arguments[++index].c_str(), nullptr, 10);
      continue;
    }
    if (!argument.empty() && argument[0] != '-') {
      filters.push_back(argument);
      continue;
    }
    std::cerr << "unknown argument: " << argument << "\n";
    return 2;
  }

  std::vector<TestCase> selected;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (filters.empty()) {
      selected.push_back(test);
      continue;
    }
    for (const std::string& filter : filters) {
      if (full.find(filter) != std::string::npos) {
        selected.push_back(test);
        break;
      }
    }
  }
  if (selected.empty()) {
    std::cerr << "no tests matched the requested filters\n";
    return 2;
  }

  std::cout << "facility_topology test suite: " << selected.size() << " of " << registry().size()
            << " tests selected, seed=" << g_seed << "\n";
  std::size_t passed = 0;
  std::vector<std::string> failed;
  for (const TestCase& test : selected) {
    g_current_case = test.suite + "." + test.name;
    g_case_failed = false;
    const int before = g_failures_in_case;
    try {
      test.function();
    } catch (const TestAborted&) {
      // FT_REQUIRE already reported the failure.
    } catch (const std::exception& error) {
      fail(__FILE__, __LINE__, std::string("uncaught exception: ") + error.what());
    } catch (...) {
      fail(__FILE__, __LINE__, "uncaught non-standard exception");
    }
    if (g_case_failed || g_failures_in_case != before) {
      failed.push_back(g_current_case);
    } else {
      ++passed;
      std::cout << "ok   " << g_current_case << "\n";
    }
  }

  std::cout << "\n" << passed << " passed, " << failed.size() << " failed, seed=" << g_seed << "\n";
  for (const std::string& name : failed) {
    std::cout << "  failed: " << name << "\n";
  }
  return failed.empty() ? 0 : 1;
}

}  // namespace ftest

int main(int argc, char** argv) {
  int exit_code = 0;
  if (ftest::dispatch_child_scenario(argc, argv, exit_code)) {
    return exit_code;
  }
  return ftest::run_all(argc, argv);
}
