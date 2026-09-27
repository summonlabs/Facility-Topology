// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "child_process.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ftest {
namespace {

std::filesystem::path output_path() {
  static std::atomic<std::uint64_t> counter{0};
  std::error_code error;
  std::filesystem::path directory = std::filesystem::temp_directory_path(error);
  if (error) {
    directory = std::filesystem::current_path();
  }
  return directory / ("facility_topology_child_output_" + std::to_string(counter.fetch_add(1)) + ".txt");
}

std::string read_and_remove(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  stream.close();
  std::error_code error;
  std::filesystem::remove(path, error);
  return buffer.str();
}

#if defined(_WIN32)

std::wstring to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

/// Quotes one argument for a CreateProcess command line.
std::string quote_argument(const std::string& value) {
  if (!value.empty() && value.find_first_of(" \t\n\v\"") == std::string::npos) {
    return value;
  }
  std::string out;
  out.push_back('"');
  for (std::size_t index = 0; index < value.size(); ++index) {
    std::size_t backslashes = 0;
    while (index < value.size() && value[index] == '\\') {
      ++backslashes;
      ++index;
    }
    if (index == value.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (value[index] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      continue;
    }
    out.append(backslashes, '\\');
    out.push_back(value[index]);
  }
  out.push_back('"');
  return out;
}

HANDLE open_capture(const std::filesystem::path& path) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE open_null_input() {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

#else

void write_all(int descriptor, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t chunk = ::write(descriptor, text.data() + written, text.size() - written);
    if (chunk <= 0) {
      return;
    }
    written += static_cast<std::size_t>(chunk);
  }
}

#endif

}  // namespace

std::filesystem::path executable_path() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(4096);
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
#else
  std::vector<char> buffer(4096);
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return {};
  }
  return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
}

ChildOutcome run_program(const std::filesystem::path& program, const std::vector<std::string>& arguments) {
  ChildOutcome outcome;
  if (program.empty()) {
    outcome.output = "no program path was supplied";
    return outcome;
  }
  const std::filesystem::path capture = output_path();

#if defined(_WIN32)
  std::string command = quote_argument(program.string());
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  std::wstring wide_command = to_wide(command);
  if (wide_command.empty()) {
    outcome.output = "cannot encode the command line";
    return outcome;
  }

  const HANDLE output = open_capture(capture);
  if (output == INVALID_HANDLE_VALUE) {
    outcome.output = "cannot create the capture file";
    return outcome;
  }
  const HANDLE input = open_null_input();

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = (input == INVALID_HANDLE_VALUE) ? nullptr : input;

  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
  if (input != INVALID_HANDLE_VALUE) {
    ::CloseHandle(input);
  }
  if (created == 0) {
    ::CloseHandle(output);
    outcome.output = "cannot start the child process: " + std::to_string(::GetLastError());
    std::error_code error;
    std::filesystem::remove(capture, error);
    return outcome;
  }
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  ::CloseHandle(output);
  outcome.exit_code = static_cast<int>(exit_code);
  outcome.output = read_and_remove(capture);
  return outcome;
#else
  std::vector<std::string> storage;
  storage.push_back(program.string());
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    outcome.output = "cannot fork";
    return outcome;
  }
  if (pid == 0) {
    const int descriptor = ::open(capture.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor >= 0) {
      ::dup2(descriptor, STDOUT_FILENO);
      ::dup2(descriptor, STDERR_FILENO);
      ::close(descriptor);
    }
    ::execv(program.c_str(), argv.data());
    const std::string failure = "cannot execute the child process\n";
    write_all(STDERR_FILENO, failure);
    ::_exit(127);
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    outcome.output = "cannot wait for the child process";
    return outcome;
  }
  if (WIFEXITED(status)) {
    outcome.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    outcome.exit_code = 128 + WTERMSIG(status);
  }
  outcome.output = read_and_remove(capture);
  return outcome;
#endif
}

ChildOutcome run_child(const std::vector<std::string>& arguments) { return run_program(executable_path(), arguments); }

bool dispatch_child_scenario(int argc, char** argv, int& exit_code) {
  if (argc < 2) {
    return false;
  }
  if (std::string(argv[1]) != "--ftop-child") {
    return false;
  }
  exit_code = child_scenarios_main(argc, argv);
  return true;
}

}  // namespace ftest
