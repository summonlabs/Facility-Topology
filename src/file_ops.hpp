// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Platform file primitives used by the durable store. Private to the library.
//
// Everything here is written so that authoritative state is only ever changed
// by an atomic replace, and so that a lock is never held across a callback.

#ifndef DCCP_FACILITY_TOPOLOGY_SRC_FILE_OPS_HPP
#define DCCP_FACILITY_TOPOLOGY_SRC_FILE_OPS_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology::file_ops {

enum class LockMode {
  Shared,
  Exclusive,
};

/// Advisory, inter-process file lock.
///
/// The lock is held for as long as the object lives and is released on
/// destruction. Acquisition never blocks: a lock held by another process is
/// reported as STORE_LOCKED so that the caller can decide what to do instead
/// of waiting inside the library.
class FileLock {
 public:
  FileLock() noexcept;
  ~FileLock();
  FileLock(FileLock&&) noexcept;
  FileLock& operator=(FileLock&&) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  static Result<FileLock> acquire(const std::filesystem::path& file, LockMode mode);

  bool held() const noexcept;
  void release() noexcept;

 private:
  struct Handle;
  explicit FileLock(std::unique_ptr<Handle> handle) noexcept;

  std::unique_ptr<Handle> handle_;
};

/// Reads a whole file, refusing anything larger than `max_bytes`.
///
/// The size check uses the file's own reported size before allocating, and the
/// content is re-checked while reading, so a file that grows between the two
/// cannot force an unbounded allocation.
Result<std::string> read_file(const std::filesystem::path& path, std::size_t max_bytes);

/// Atomically replaces `path` with `content`.
///
/// The bytes are written to a uniquely named temporary file in the same
/// directory, flushed to stable storage, and then renamed over the target. On
/// success the target is either the old content or the new content, never a
/// mixture.
Result<void> atomic_write_file(const std::filesystem::path& path, std::string_view content);

/// Flushes a directory entry so a completed rename is durable.
Result<void> flush_directory(const std::filesystem::path& directory);

/// Creates `directory` and any missing parents.
Result<void> create_directories(const std::filesystem::path& directory);

/// True when `path` names an existing regular file (symbolic links are not
/// followed: a link in a store directory is treated as untrusted input).
bool is_regular_file(const std::filesystem::path& path) noexcept;

/// True when `path` names an existing directory.
bool is_directory(const std::filesystem::path& path) noexcept;

bool exists(const std::filesystem::path& path) noexcept;

/// Removes a file, reporting whether it existed. Missing files are not an
/// error so that cleanup paths are idempotent.
Result<bool> remove_file(const std::filesystem::path& path);

/// Renames within the same filesystem, replacing an existing target.
Result<void> rename_replace(const std::filesystem::path& from, const std::filesystem::path& to);

/// Directory entry names, sorted byte-wise so that every caller observes a
/// deterministic order.
Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory);

/// Process identifier, used only to make temporary file names unique.
std::string process_id_token();

/// Monotonic counter making temporary file names unique within a process.
std::string next_sequence_token();

}  // namespace dccp::facility_topology::file_ops

#endif  // DCCP_FACILITY_TOPOLOGY_SRC_FILE_OPS_HPP
