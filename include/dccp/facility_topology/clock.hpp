// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_FACILITY_TOPOLOGY_CLOCK_HPP
#define DCCP_FACILITY_TOPOLOGY_CLOCK_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/facility_topology/result.hpp"

namespace dccp::facility_topology {

/// Time source for recorded provenance.
///
/// The library never calls the operating system clock directly. Durable
/// timestamps come from an injected clock, so tests and benchmarks are
/// reproducible and the default is the only place that reads real time.
class Clock {
 public:
  virtual ~Clock();

  /// Seconds since the Unix epoch, UTC.
  virtual std::int64_t unix_seconds() const noexcept = 0;
};

/// Real UTC wall clock.
class SystemClock final : public Clock {
 public:
  std::int64_t unix_seconds() const noexcept override;
};

/// Fixed, caller-controlled clock. Used by tests and by replay tooling that
/// must reproduce an exact historical timestamp.
class FixedClock final : public Clock {
 public:
  explicit FixedClock(std::int64_t unix_seconds) noexcept : seconds_(unix_seconds) {}

  std::int64_t unix_seconds() const noexcept override { return seconds_; }
  void set_unix_seconds(std::int64_t seconds) noexcept { seconds_ = seconds; }

 private:
  std::int64_t seconds_;
};

/// Formats a Unix timestamp as canonical UTC "YYYY-MM-DDTHH:MM:SSZ".
Result<std::string> format_utc(std::int64_t unix_seconds);

/// Parses canonical UTC "YYYY-MM-DDTHH:MM:SSZ" into a Unix timestamp.
/// Rejects anything that is not exactly that shape, including leap seconds,
/// out-of-range calendar fields and trailing content.
Result<std::int64_t> parse_utc(std::string_view text);

}  // namespace dccp::facility_topology

#endif  // DCCP_FACILITY_TOPOLOGY_CLOCK_HPP
