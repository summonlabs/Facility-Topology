// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/clock.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace dccp::facility_topology {
namespace {

constexpr bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

constexpr unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
  switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    case 2:
      return is_leap_year(year) ? 29U : 28U;
    default:
      return 0;
  }
}

/// Days since 1970-01-01 for a proleptic Gregorian date.
constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= (month <= 2) ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const int shifted_month = static_cast<int>(month) + ((month > 2) ? -3 : 9);
  const unsigned day_of_year = static_cast<unsigned>((153 * shifted_month + 2) / 5) + day - 1U;
  const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

/// Inverse of days_from_civil().
constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
  const std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
  const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
  const unsigned day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
  const unsigned month = static_cast<unsigned>(static_cast<int>(month_prime) + ((month_prime < 10U) ? 3 : -9));
  return CivilDate{year + ((month <= 2) ? 1 : 0), month, day};
}

constexpr std::int64_t kMinUnixSeconds = 0;               // 1970-01-01T00:00:00Z
constexpr std::int64_t kMaxUnixSeconds = 253402300799LL;  // 9999-12-31T23:59:59Z

std::string two_digits(unsigned value) {
  std::string out;
  out.push_back(static_cast<char>('0' + (value / 10U) % 10U));
  out.push_back(static_cast<char>('0' + value % 10U));
  return out;
}

std::string four_digits(std::uint64_t value) {
  std::string out(4, '0');
  for (std::size_t i = 0; i < 4; ++i) {
    out[3 - i] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  }
  return out;
}

}  // namespace

Clock::~Clock() = default;

std::int64_t SystemClock::unix_seconds() const noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

Result<std::string> format_utc(std::int64_t unix_seconds) {
  if (unix_seconds < kMinUnixSeconds || unix_seconds > kMaxUnixSeconds) {
    return Error(ErrorCode::InvalidArgument,
                 "timestamp is outside the representable canonical UTC range 1970-01-01..9999-12-31")
        .with_subject(std::to_string(unix_seconds));
  }
  const std::int64_t days = unix_seconds / 86400;
  std::int64_t remainder = unix_seconds % 86400;
  const CivilDate date = civil_from_days(days);
  const auto hour = static_cast<unsigned>(remainder / 3600);
  remainder %= 3600;
  const auto minute = static_cast<unsigned>(remainder / 60);
  const auto second = static_cast<unsigned>(remainder % 60);

  std::string out = four_digits(static_cast<std::uint64_t>(date.year));
  out.push_back('-');
  out.append(two_digits(date.month));
  out.push_back('-');
  out.append(two_digits(date.day));
  out.push_back('T');
  out.append(two_digits(hour));
  out.push_back(':');
  out.append(two_digits(minute));
  out.push_back(':');
  out.append(two_digits(second));
  out.push_back('Z');
  return out;
}

Result<std::int64_t> parse_utc(std::string_view text) {
  if (text.size() != 20) {
    return Error(ErrorCode::MalformedRecord, "timestamp must be exactly \"YYYY-MM-DDTHH:MM:SSZ\"")
        .with_subject(std::string(text.substr(0, 40)));
  }
  const auto digit = [&text](std::size_t index) noexcept { return text[index] >= '0' && text[index] <= '9'; };
  constexpr std::size_t kDigitPositions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const std::size_t position : kDigitPositions) {
    if (!digit(position)) {
      return Error(ErrorCode::MalformedRecord, "timestamp contains a non-digit where a digit is required")
          .with_subject(std::string(text));
    }
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
    return Error(ErrorCode::MalformedRecord, "timestamp must be exactly \"YYYY-MM-DDTHH:MM:SSZ\"")
        .with_subject(std::string(text));
  }
  const auto field = [&text](std::size_t offset) noexcept {
    return static_cast<unsigned>((text[offset] - '0') * 10 + (text[offset + 1] - '0'));
  };
  const auto year = static_cast<std::int64_t>((text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 +
                                              (text[3] - '0'));
  const unsigned month = field(5);
  const unsigned day = field(8);
  const unsigned hour = field(11);
  const unsigned minute = field(14);
  const unsigned second = field(17);

  if (month < 1 || month > 12) {
    return Error(ErrorCode::MalformedRecord, "timestamp month must be 01..12").with_subject(std::string(text));
  }
  const unsigned month_length = days_in_month(year, month);
  if (day < 1 || day > month_length) {
    return Error(ErrorCode::MalformedRecord, "timestamp day is not a valid day of that month")
        .with_subject(std::string(text));
  }
  if (hour > 23 || minute > 59 || second > 59) {
    return Error(ErrorCode::MalformedRecord, "timestamp time fields are out of range").with_subject(std::string(text));
  }

  const std::int64_t days = days_from_civil(year, month, day);
  const std::int64_t seconds =
      days * 86400 + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
      static_cast<std::int64_t>(second);
  if (seconds < kMinUnixSeconds || seconds > kMaxUnixSeconds) {
    return Error(ErrorCode::InvalidArgument, "timestamp is outside the representable canonical UTC range")
        .with_subject(std::string(text));
  }
  return seconds;
}

}  // namespace dccp::facility_topology
