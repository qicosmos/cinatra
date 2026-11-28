#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cinatra/utils.hpp"
#include "cinatra_log_wrapper.hpp"
#include "define.h"
#include "picohttpparser.h"
#include "secure_string_hash.hpp"
#include "url_encode_decode.hpp"

using namespace std::string_view_literals;

#ifndef CINATRA_MAX_HTTP_HEADER_FIELD_SIZE
#define CINATRA_MAX_HTTP_HEADER_FIELD_SIZE 100
#endif

#ifndef CINATRA_MAX_QUERY_FIELD_COUNT
#define CINATRA_MAX_QUERY_FIELD_COUNT 1024
#endif

namespace cinatra {
inline bool iequal0(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char a, char b) {
    return tolower(a) == tolower(b);
  });
}

namespace detail {
inline bool http_header_name_equal(std::string_view lhs,
                                   std::string_view rhs) noexcept {
  const auto lower = [](unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
  };
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                    [&](char a, char b) {
                      return lower(a) == lower(b);
                    });
}

inline double parse_http_quality(std::string_view value) noexcept {
  if (value.empty() || (value.front() != '0' && value.front() != '1')) {
    return 0;
  }
  const bool full_weight = value.front() == '1';
  if (value.size() == 1) {
    return full_weight ? 1 : 0;
  }
  if (value[1] != '.' || value.size() > 5) {
    return 0;
  }

  unsigned fraction = 0;
  unsigned place = 100;
  for (const char digit : value.substr(2)) {
    if (digit < '0' || digit > '9') {
      return 0;
    }
    fraction += (digit - '0') * place;
    place /= 10;
  }
  if (full_weight) {
    return fraction == 0 ? 1 : 0;
  }
  return fraction / 1000.0;
}
}  // namespace detail

// Return the explicit weight, the wildcard weight, or zero for no match.
// The caller chooses its policy for an absent header and identity fallback.
inline double accept_encoding_quality(std::string_view header,
                                      std::string_view encoding) noexcept {
  const auto trim = [](std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      return std::string_view{};
    }
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
  };

  double wildcard = 0;
  while (!header.empty()) {
    const auto comma = header.find(',');
    const auto entry = header.substr(0, comma);
    const auto semicolon = entry.find(';');
    const auto name = trim(entry.substr(0, semicolon));
    const bool matches = detail::http_header_name_equal(name, encoding);
    if (matches || name == "*") {
      double quality = 1;
      if (semicolon != std::string_view::npos) {
        const auto parameter = trim(entry.substr(semicolon + 1));
        if (detail::http_header_name_equal(parameter.substr(0, 2), "q=")) {
          quality = detail::parse_http_quality(parameter.substr(2));
        }
      }
      if (matches) {
        return quality;
      }
      wildcard = quality;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    header.remove_prefix(comma + 1);
  }
  return wildcard;
}

class http_parser {
 public:
  void parse_body_len() {
    auto header_value = this->get_header_value("content-length"sv);
    if (header_value.empty()) {
      body_len_ = 0;
    }
    else {
      auto [ptr, ec] = std::from_chars(
          header_value.data(), header_value.data() + header_value.size(),
          body_len_, 10);
      if (ec != std::errc{}) {
        body_len_ = -1;
      }
    }
  }

  int parse_response(const char *data, size_t size, int last_len) {
    int minor_version;

    num_headers_ = CINATRA_MAX_HTTP_HEADER_FIELD_SIZE;
    const char *msg;
    size_t msg_len;
    header_len_ = cinatra::detail::phr_parse_response(
        data, size, &minor_version, &status_, &msg, &msg_len, headers_.data(),
        &num_headers_, last_len);
    msg_ = {msg, msg_len};
    parse_body_len();
    if (header_len_ < 0) [[unlikely]] {
      CINATRA_LOG_WARNING << "parse http head failed";
      if (num_headers_ == CINATRA_MAX_HTTP_HEADER_FIELD_SIZE) {
        output_error();
      }
    }
    return header_len_;
  }

  int parse_request(const char *data, size_t size, int last_len) {
    int minor_version;

    num_headers_ = CINATRA_MAX_HTTP_HEADER_FIELD_SIZE;

    const char *method;
    size_t method_len;
    const char *url;
    size_t url_len;

    bool has_query{};
    header_len_ = detail::phr_parse_request(
        data, size, &method, &method_len, &url, &url_len, &minor_version,
        headers_.data(), &num_headers_, last_len, has_connection_, has_close_,
        has_upgrade_, has_query);

    if (header_len_ < 0) [[unlikely]] {
      CINATRA_LOG_WARNING << "parse http head failed";
      if (num_headers_ == CINATRA_MAX_HTTP_HEADER_FIELD_SIZE) {
        output_error();
      }
      return header_len_;
    }

    method_ = {method, method_len};
    url_ = {url, url_len};

    auto methd_type = method_type(method_);
    if (methd_type == http_method::GET || methd_type == http_method::HEAD) {
      body_len_ = 0;
    }
    else {
      parse_body_len();
    }

    full_url_ = url_;
    if (parameter_count_ != 0 || parameter_limit_exceeded_) {
      queries_.clear();
      parameter_count_ = 0;
      parameter_limit_exceeded_ = false;
    }
    if (has_query) {
      size_t pos = url_.find('?');
      parse_query(url_.substr(pos + 1, url_len - pos - 1));
      if (parameter_limit_exceeded_) [[unlikely]] {
        CINATRA_LOG_WARNING << "too many query fields";
        return -1;
      }
      url_ = {url, pos};
    }

    return header_len_;
  }

  bool has_connection() { return has_connection_; }

  bool has_close() { return has_close_; }

  bool has_upgrade() { return has_upgrade_; }

  std::string_view get_header_value(std::string_view key) const {
    for (size_t i = 0; i < num_headers_; i++) {
      if (detail::http_header_name_equal(headers_[i].name, key))
        return headers_[i].value;
    }
    return {};
  }

  const auto &queries() const { return queries_; }

  bool parameter_limit_exceeded() const { return parameter_limit_exceeded_; }

  std::string_view full_url() { return full_url_; }

  std::string_view get_query_value(std::string_view key) {
    if (auto it = queries_.find(key); it != queries_.end()) {
      return it->second;
    }
    else {
      return "";
    }
  }

  bool is_chunked() const {
    auto transfer_encoding = this->get_header_value("transfer-encoding"sv);
    if (transfer_encoding == "chunked"sv) {
      return true;
    }

    return false;
  }

  bool is_multipart() {
    auto content_type = get_header_value("Content-Type");
    if (content_type.empty()) {
      return false;
    }

    if (content_type.find("multipart") == std::string_view::npos) {
      return false;
    }

    return true;
  }

  std::string_view get_boundary() {
    auto content_type = get_header_value("Content-Type");
    size_t pos = content_type.find("=--");
    if (pos == std::string_view::npos) {
      return "";
    }

    return content_type.substr(pos + 1);
  }

  bool is_resp_ranges() const {
    auto value = this->get_header_value("Accept-Ranges"sv);
    return !value.empty();
  }

  bool is_websocket() const {
    auto upgrade = this->get_header_value("Upgrade"sv);
    return upgrade == "WebSocket"sv || upgrade == "websocket"sv;
  }

  bool keep_alive() const {
    if (is_websocket()) {
      return true;
    }
    auto val = this->get_header_value("connection"sv);
    if (val.empty() || iequal0(val, "keep-alive"sv)) {
      return true;
    }

    return false;
  }

  int status() const { return status_; }

  int header_len() const { return header_len_; }

  int64_t body_len() const { return body_len_; }

  int64_t total_len() const { return header_len_ + body_len_; }

  bool is_location() {
    auto location = this->get_header_value("Location"sv);
    return !location.empty();
  }

  std::string_view msg() const { return msg_; }

  std::string_view method() const { return method_; }

  std::string_view url() const { return url_; }

  std::span<http_header> get_headers() {
    return {headers_.data(), num_headers_};
  }

  void parse_query(std::string_view str) {
    size_t start = 0;
    while (start <= str.size()) {
      size_t end = str.find('&', start);
      auto s = str.substr(start, end - start);
      if (s.empty()) {
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
        continue;
      }

      size_t pos = s.find('=');
      std::string_view key;
      std::string_view val;
      if (pos != std::string_view::npos) {
        key = s.substr(0, pos);
        if (key.empty()) {
          if (end == std::string_view::npos) {
            break;
          }
          start = end + 1;
          continue;
        }
        val = s.substr(pos + 1, s.length() - pos - 1);
      }
      else {
        key = s;
        val = "";
      }

      if (parameter_count_ >= CINATRA_MAX_QUERY_FIELD_COUNT) [[unlikely]] {
        parameter_limit_exceeded_ = true;
        return;
      }
      ++parameter_count_;
      queries_.emplace(key, val);

      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
  }

 private:
  void output_error() {
    CINATRA_LOG_ERROR << "the field of http head is out of max limit "
                      << CINATRA_MAX_HTTP_HEADER_FIELD_SIZE
                      << ", you can define macro "
                         "CINATRA_MAX_HTTP_HEADER_FIELD_SIZE to expand it.";
  }

 private:
  int status_ = 0;
  std::string_view msg_;
  size_t num_headers_ = 0;
  int header_len_ = 0;
  int64_t body_len_ = 0;
  bool has_connection_{};
  bool has_close_{};
  bool has_upgrade_{};
  std::array<http_header, CINATRA_MAX_HTTP_HEADER_FIELD_SIZE> headers_;
  std::string_view method_;
  std::string_view url_;
  std::string_view full_url_;
  std::unordered_map<std::string_view, std::string_view, secure_string_hash>
      queries_;
  size_t parameter_count_ = 0;
  bool parameter_limit_exceeded_ = false;
};
}  // namespace cinatra
