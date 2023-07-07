#pragma once

#include <chrono>
#include <string_view>

#include "define.h"

namespace cinatra {
namespace time_util {
inline constexpr bool is_leap(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline constexpr int get_day_index(std::string_view str) {
  return week_table[((str[0] & ~0x20) ^ (str[2] & ~0x20)) % week_table.size()];
}

inline constexpr int get_month_index(std::string_view str) {
  return month_table[((str[1] & ~0x20) + (str[2] & ~0x20)) %
                     month_table.size()];
}

inline constexpr int days_in(int m, int year) {
  constexpr int index = get_month_index("Feb");
  if (m == index && is_leap(year)) {
    return 29;
  }
  return int(days_before[m + 1] - days_before[m]);
}

inline constexpr int get_digit(std::string_view sv, int width) {
  int num = 0;
  for (int i = 0; i < width; i++) {
    if ('0' <= sv[i] && sv[i] <= '9') {
      num = num * 10 + (sv[i] - '0');
    }
    else {
      return -1;
    }
  }
  return num;
}

inline constexpr std::uint64_t days_since_epoch(int year) {
  auto y = std::uint64_t(std::int64_t(year) - absolute_zero_year);

  auto n = y / 400;
  y -= 400 * n;
  auto d = days_per_400_years * n;
  n = y / 100;
  y -= 100 * n;
  d += days_per_100_years * n;
  n = y / 4;
  y -= 4 * n;
  d += days_per_4_years * n;
  n = y;
  d += 365 * n;
  return d;
}

inline std::pair<bool, std::time_t> faster_mktime(int year, int month, int day,
                                                  int hour, int min, int sec,
                                                  int day_of_week) {
  auto d = days_since_epoch(year);
  d += std::uint64_t(days_before[month]);
  constexpr int index = get_month_index("Mar");
  if (is_leap(year) && month >= index) {
    d++;  // February 29
  }
  d += std::uint64_t(day - 1);
  auto abs = d * seconds_per_day;
  abs +=
      std::uint64_t(hour * seconds_per_hour + min * seconds_per_minute + sec);
  constexpr int day_index = get_day_index("Mon");
  if (day_of_week != -1) {
    std::int64_t wday = ((abs + std::uint64_t(day_index) * seconds_per_day) %
                         seconds_per_week) /
                        seconds_per_day;
    if (wday != day_of_week) {
      return {false, 0};
    }
  }
  return {true, std::int64_t(abs) + (absolute_to_internal + internal_to_unix)};
}

template <time_format Format>
inline constexpr std::array<component_of_time_format, 32> get_format() {
  if constexpr (Format == time_format::http_format) {
    return http_time_format;
  }
  else if constexpr (Format == time_format::utc_format) {
    return utc_time_format;
  }
  else {
    return utc_time_without_punctuation_format;
  }
}
}  // namespace time_util

template <time_format Format = time_format::http_format>
inline std::pair<bool, std::time_t> get_timestamp(
    const std::string &gmt_time_str) {
  using namespace time_util;
  std::string_view sv(gmt_time_str);
  int year, month, day, hour, min, sec, day_of_week;
  int len_of_gmt_time_str = (int)gmt_time_str.length();
  int len_of_processed_part = 0;
  int len_of_ignored_part = 0;  // second_decimal_part is ignored
  char c;
  constexpr std::array<component_of_time_format, 32> real_format =
      time_util::get_format<Format>();
  if constexpr (Format == time_format::utc_format) {
    day_of_week = -1;
  }
  else if (Format == time_format::utc_without_punctuation_format) {
    day_of_week = -1;
  }

  for (auto &comp : real_format) {
    switch (comp) {
      case component_of_time_format::ending:
        goto travel_done;
        break;
      case component_of_time_format::colon:
      case component_of_time_format::comma:
      case component_of_time_format::SP:
      case component_of_time_format::hyphen:
      case component_of_time_format::dot:
      case component_of_time_format::T:
      case component_of_time_format::Z:
        if (len_of_gmt_time_str - len_of_processed_part < 1) {
          return {false, 0};
        }
        c = sv[len_of_processed_part];
        if ((comp == component_of_time_format::Z && c != 'Z') ||
            (comp == component_of_time_format::T && c != 'T') ||
            (comp == component_of_time_format::dot && c != '.') ||
            (comp == component_of_time_format::hyphen && c != '-') ||
            (comp == component_of_time_format::SP && c != ' ') ||
            (comp == component_of_time_format::colon && c != ':') ||
            (comp == component_of_time_format::comma && c != ',')) {
          return {false, 0};
        }
        len_of_processed_part += 1;
        break;
      case component_of_time_format::year:
        if (len_of_gmt_time_str - len_of_processed_part < 4) {
          return {false, 0};
        }
        if ((year = get_digit(sv.substr(len_of_processed_part, 4), 4)) == -1) {
          return {false, 0};
        }
        len_of_processed_part += 4;
        break;
      case component_of_time_format::month_name:
        if (len_of_gmt_time_str - len_of_processed_part < 3) {
          return {false, 0};
        }
        if ((month = get_month_index(sv.substr(len_of_processed_part, 3))) ==
            -1) {
          return {false, 0};
        }
        len_of_processed_part += 3;
        break;
      case component_of_time_format::hour:
      case component_of_time_format::minute:
      case component_of_time_format::second:
      case component_of_time_format::month:
      case component_of_time_format::day:
        if (len_of_gmt_time_str - len_of_processed_part < 2) {
          return {false, 0};
        }
        int digit;
        if ((digit = get_digit(sv.substr(len_of_processed_part, 2), 2)) == -1) {
          return {false, 0};
        }
        if (comp == component_of_time_format::hour) {
          hour = digit;
          if (hour < 0 || hour >= 24) {
            return {false, 0};
          }
        }
        else if (comp == component_of_time_format::minute) {
          min = digit;
          if (min < 0 || min >= 60) {
            return {false, 0};
          }
        }
        else if (comp == component_of_time_format::month) {
          month = digit;
          if (month < 1 || month > 12) {
            return {false, 0};
          }
          month--;
        }
        else if (comp == component_of_time_format::second) {
          sec = digit;
          if (sec < 0 || sec >= 60) {
            return {false, 0};
          }
        }
        else {
          day = digit;
        }
        len_of_processed_part += 2;
        break;
      case component_of_time_format::day_name:
        if (len_of_gmt_time_str - len_of_processed_part < 3) {
          return {false, 0};
        }
        if ((day_of_week = get_day_index(sv.substr(len_of_processed_part, 3))) <
            0) {
          return {false, 0};
        }
        len_of_processed_part += 3;
        break;
      case component_of_time_format::GMT:
        if (len_of_gmt_time_str - len_of_processed_part < 3) {
          return {false, 0};
        }
        if (sv.substr(len_of_processed_part, 3) != "GMT") {
          return {false, 0};
        }
        len_of_processed_part += 3;
        break;
      case component_of_time_format::second_decimal_part:
        int cur = len_of_processed_part;
        while (cur < len_of_gmt_time_str &&
               (sv[cur] >= '0' && sv[cur] <= '9')) {
          len_of_ignored_part++;
          cur++;
        }
        if (cur == len_of_processed_part) {
          return {false, 0};
        }
        len_of_processed_part = cur;
        break;
    }
  }
travel_done:
  if ((len_of_processed_part != len_of_gmt_time_str) ||
      (len_of_processed_part != len_of_http_time_format &&
       (len_of_processed_part - len_of_ignored_part) !=
           len_of_utc_time_format) &&
          (len_of_processed_part - len_of_ignored_part) !=
              len_of_utc_time_without_punctuation_format) {
    return {false, 0};
  }
  if (day < 1 || day > days_in(month, year)) {
    return {false, 0};
  }
  return faster_mktime(year, month, day, hour, min, sec, day_of_week);
}

inline std::string get_time_str(std::time_t t) {
  std::stringstream ss;
  ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

inline std::string get_gmt_time_str(std::time_t t) {
  struct tm *GMTime = gmtime(&t);
  char buff[512] = {0};
  strftime(buff, sizeof(buff), "%a, %d %b %Y %H:%M:%S GMT", GMTime);
  return buff;
}

inline std::string get_cur_time_str() {
  return get_time_str(std::time(nullptr));
}
}  // namespace cinatra