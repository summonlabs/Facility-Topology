// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent-process helper for the multiprocess proof obligations.
//
// The test executable re-executes itself to obtain genuine operating-system
// processes: no threads pretending to be processes, no in-process simulation of
// a crash. A child scenario communicates its outcome through its exit status
// and through captured standard output.

#ifndef FACILITY_TOPOLOGY_TESTS_CHILD_PROCESS_HPP
#define FACILITY_TOPOLOGY_TESTS_CHILD_PROCESS_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace ftest {

/// Absolute path of the running test executable.
std::filesystem::path executable_path();

struct ChildOutcome {
  /// Process exit status, or -1 when the process could not be started.
  int exit_code = -1;
  /// Combined standard output and standard error.
  std::string output;
};

/// Runs this executable as a new operating-system process.
///
/// Every argument is passed verbatim; the working directory is not changed so
/// scenarios must use absolute paths.
ChildOutcome run_child(const std::vector<std::string>& arguments);

/// Runs an arbitrary program with its output captured.
ChildOutcome run_program(const std::filesystem::path& program, const std::vector<std::string>& arguments);

/// Entry point implemented by the scenario translation unit.
int child_scenarios_main(int argc, char** argv);

/// When the process was started as a child scenario, runs it and stores its
/// exit status. Returns false for a normal test run.
bool dispatch_child_scenario(int argc, char** argv, int& exit_code);

}  // namespace ftest

#endif  // FACILITY_TOPOLOGY_TESTS_CHILD_PROCESS_HPP
