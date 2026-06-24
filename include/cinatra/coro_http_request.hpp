#pragma once

#include <algorithm>
#include <any>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "async_simple/coro/Lazy.h"
#include "define.h"
#ifdef CINATRA_ENABLE_SSL
#include "detail/protocol_request.hpp"
#endif
#include "http_parser.hpp"
#include "session.hpp"
#include "session_manager.hpp"
#include "utils.hpp"
#include "ws_define.h"

namespace cinatra {
#ifdef CINATRA_ENABLE_SSL
namespace http2 {
class coro_http2_connection;
}
#endif

inline std::vector<std::pair<int, int>> parse_ranges(std::string_view range_str,
                                                     size_t file_size,
                                                     bool &is_valid) {
  if (file_size == 0) {
    is_valid = false;
    return {};
  }

  auto file_last = static_cast<int>((std::min)(
      file_size - 1, static_cast<size_t>(std::numeric_limits<int>::max())));
  range_str = trim_sv(range_str);
  if (range_str.empty()) {
    return {{0, file_last}};
  }

  if (range_str.find("--") != std::string_view::npos) {
    is_valid = false;
    return {};
  }

  if (range_str == "-") {
    return {{0, file_last}};
  }

  std::vector<std::pair<int, int>> vec;
  auto ranges = split_sv(range_str, ",");
  for (auto range : ranges) {
    auto sub_range = split_sv(range, "-");
    auto fist_range = trim_sv(sub_range[0]);

    int start = 0;
    if (fist_range.empty()) {
      start = -1;
    }
    else {
      auto [ptr, ec] = std::from_chars(
          fist_range.data(), fist_range.data() + fist_range.size(), start);
      if (ec != std::errc{}) {
        is_valid = false;
        return {};
      }
    }

    int end = 0;
    if (sub_range.size() == 1) {
      end = file_last;
    }
    else {
      auto second_range = trim_sv(sub_range[1]);
      if (second_range.empty()) {
        end = file_last;
      }
      else {
        auto [ptr, ec] =
            std::from_chars(second_range.data(),
                            second_range.data() + second_range.size(), end);
        if (ec != std::errc{}) {
          is_valid = false;
          return {};
        }
      }
    }

    if (start > 0 &&
        (static_cast<size_t>(start) >= file_size || start == end)) {
      // out of range
      is_valid = false;
      return {};
    }

    if (end > 0 && static_cast<size_t>(end) >= file_size) {
      end = file_last;
    }

    if (start == -1) {
      start = static_cast<int>(
          (std::min)(file_size - static_cast<size_t>(end),
                     static_cast<size_t>(std::numeric_limits<int>::max())));
      end = file_last;
    }

    vec.push_back({start, end});
  }
  return vec;
}

class coro_http_connection;
class coro_http_request {
 public:
#ifdef CINATRA_ENABLE_SSL
  coro_http_request() = default;
#endif
  coro_http_request(http_parser &parser, coro_http_connection *conn)
#ifdef CINATRA_ENABLE_SSL
      : parser_(&parser),
        conn_(conn){}
#else
      : parser_(parser), conn_(conn) {
  }
#endif

        std::string_view get_header_value(std::string_view key) {
#ifndef CINATRA_ENABLE_SSL
    auto headers = parser_.get_headers();
#else
    auto headers = get_headers();
#endif
    for (auto &header : headers) {
      if (iequal0(header.name, key)) {
        return header.value;
      }
    }

    return {};
  }

  std::string_view get_query_value(std::string_view key) {
#ifndef CINATRA_ENABLE_SSL
    return parser_.get_query_value(key);
#else
    if (has_parser()) {
      return parser().get_query_value(key);
    }

    if (protocol_request_) {
      return protocol_request_->get_query_value(key);
    }
    return {};
#endif
  }

  std::string get_decode_query_value(std::string_view key) {
#ifndef CINATRA_ENABLE_SSL
    auto value = parser_.get_query_value(key);
#else
    auto value = get_query_value(key);
#endif
    if (value.empty()) {
      return "";
    }

    return code_utils::get_string_by_urldecode(value);
  }

  std::span<http_header> get_headers() const {
#ifndef CINATRA_ENABLE_SSL
    return parser_.get_headers();
#else
    if (has_parser()) {
      return parser().get_headers();
    }
    if (protocol_request_) {
      return protocol_request_->get_headers();
    }
    return {};
#endif
  }

#ifdef CINATRA_ENABLE_SSL
  std::span<http_header> get_trailers() const {
    if (protocol_request_) {
      return protocol_request_->get_trailers();
    }
    return {};
  }
#endif

  const auto &get_queries() const {
#ifndef CINATRA_ENABLE_SSL
    return parser_.queries();
#else
    if (has_parser()) {
      return parser().queries();
    }
    if (protocol_request_) {
      return protocol_request_->get_queries();
    }
    static const std::unordered_map<std::string_view, std::string_view>
        empty_queries;
    return empty_queries;
#endif
  }

  std::string_view full_url() {
#ifndef CINATRA_ENABLE_SSL
    return parser_.full_url();
#else
    if (has_parser()) {
      return parser().full_url();
    }
    if (protocol_request_) {
      return protocol_request_->full_url();
    }
    return {};
#endif
  }

  void set_body(std::string &body) {
    body_ = body;
    auto type = get_content_type();
#ifndef CINATRA_ENABLE_SSL
    if (type == content_type::urlencoded) {
      parser_.parse_query(body_);
    }
#else
    if (type == content_type::urlencoded && has_parser()) {
      parser().parse_query(body_);
    }
#endif
  }

  std::string_view get_body() const {
#ifdef CINATRA_ENABLE_SSL
    if (protocol_request_) {
      return protocol_request_->body();
    }
#endif
    return body_;
  }

  bool is_chunked() {
#ifndef CINATRA_ENABLE_SSL
    return parser_.is_chunked();
#else
    if (has_parser()) {
      return parser().is_chunked();
    }
    return protocol_request_ && protocol_request_->is_chunked();
#endif
  }

  std::string_view get_accept_encoding() {
    return get_header_value("Accept-Encoding");
  }

  content_encoding get_encoding_type() {
    auto encoding_type = get_header_value("Content-Encoding");
    if (!encoding_type.empty()) {
      if (encoding_type.find("gzip") != std::string_view::npos)
        return content_encoding::gzip;
      else if (encoding_type.find("deflate") != std::string_view::npos)
        return content_encoding::deflate;
      else if (encoding_type.find("br") != std::string_view::npos)
        return content_encoding::br;
      else
        return content_encoding::none;
    }
    else {
      return content_encoding::none;
    }
  }

  content_type get_content_type() {
    if (is_chunked())
      return content_type::chunked;

    auto content_type = get_header_value("content-type");
    if (!content_type.empty()) {
      if (content_type.find("application/x-www-form-urlencoded") !=
          std::string_view::npos) {
        return content_type::urlencoded;
      }
      else if (content_type.find("multipart/form-data") !=
               std::string_view::npos) {
        return content_type::multipart;
      }
      else if (content_type.find("application/octet-stream") !=
               std::string_view::npos) {
        return content_type::octet_stream;
      }
      else {
        return content_type::string;
      }
    }

    if (is_websocket_) {
      return content_type::websocket;
    }

    return content_type::unknown;
  }

  std::string_view get_url() {
#ifndef CINATRA_ENABLE_SSL
    return parser_.url();
#else
    if (has_parser()) {
      return parser().url();
    }
    if (protocol_request_) {
      return protocol_request_->url();
    }
    return {};
#endif
  }

  std::string_view get_method() {
#ifndef CINATRA_ENABLE_SSL
    return parser_.method();
#else
    if (has_parser()) {
      return parser().method();
    }
    if (protocol_request_) {
      return protocol_request_->method();
    }
    return {};
#endif
  }

#ifdef CINATRA_ENABLE_SSL
  std::string_view get_scheme() const {
    return protocol_request_ ? protocol_request_->scheme() : std::string_view{};
  }

  std::string_view get_authority() const {
    return protocol_request_ ? protocol_request_->authority()
                             : std::string_view{};
  }

  std::string_view get_protocol() const {
    return protocol_request_ ? protocol_request_->protocol()
                             : std::string_view{};
  }
#endif

  std::string_view get_boundary() {
    auto content_type = get_header_value("content-type");
    if (content_type.empty()) {
      return {};
    }

    auto pos = content_type.rfind("=");
    if (pos == std::string_view::npos) {
      return "";
    }

    return content_type.substr(pos + 1);
  }

  coro_http_connection *get_conn() { return conn_; }

  bool is_upgrade() {
#ifndef CINATRA_ENABLE_SSL
    if (!parser_.has_upgrade())
      return false;
#else
    if (!has_parser()) {
      return false;
    }
    if (!parser().has_upgrade())
      return false;
#endif

    auto u = get_header_value("Upgrade");
    if (u.empty())
      return false;

    if (u != WEBSOCKET)
      return false;

    auto sec_ws_key = get_header_value("sec-websocket-key");
    if (sec_ws_key.empty() || sec_ws_key.size() != 24)
      return false;

    is_websocket_ = true;
    return true;
  }

  bool is_support_compressed() {
    auto extension_str = get_header_value("Sec-WebSocket-Extensions");
    if (extension_str.find("permessage-deflate") != std::string::npos) {
      return true;
    }
    return false;
  }

  void set_aspect_data(std::vector<std::string> data) {
    aspect_data_ = std::move(data);
  }

  template <typename... Args>
  void set_aspect_data(Args... args) {
    (aspect_data_.push_back(std::move(args)), ...);
  }

  void set_user_data(std::any data) { user_data_ = std::move(data); }

  std::any get_user_data() { return user_data_; }

  std::vector<std::string> &get_aspect_data() { return aspect_data_; }

  std::unordered_map<std::string_view, std::string_view> get_cookies(
      std::string_view cookie_str) const {
    auto cookies = get_cookies_map(cookie_str);
    return cookies;
  }

  std::shared_ptr<session> get_session(bool create = true) {
    auto &session_manager = session_manager::get();

    auto cookies = get_cookies(get_header_value("Cookie"));
    std::string session_id;
    auto iter = cookies.find(CSESSIONID);
    if (iter == cookies.end() && !create) {
      return nullptr;
    }
    else if (iter == cookies.end()) {
      session_id = session_manager.generate_session_id();
    }
    else {
      session_id = iter->second;
    }

    cached_session_id_ = session_id;
    return session_manager.get_session(session_id);
  }

  std::string get_cached_session_id() {
    std::string temp_session_id = "";
    cached_session_id_.swap(temp_session_id);
    return temp_session_id;
  }

  bool has_session() { return !cached_session_id_.empty(); }
  void clear() {
    body_ = {};
#ifdef CINATRA_ENABLE_SSL
    protocol_request_.reset();
#endif
    if (!aspect_data_.empty()) {
      aspect_data_.clear();
    }
    if (user_data_.has_value()) {
      user_data_.reset();
    }
  }

  std::unordered_map<std::string, std::string> params_;
  std::smatch matches_;

 private:
#ifdef CINATRA_ENABLE_SSL
  friend class http2::coro_http2_connection;

  template <typename ProtocolRequest, typename... Args>
  ProtocolRequest &reset_protocol_request(coro_http_connection *conn,
                                          Args &&...args) {
    clear();
    parser_ = nullptr;
    conn_ = conn;
    is_websocket_ = false;
    auto request =
        std::make_shared<ProtocolRequest>(std::forward<Args>(args)...);
    auto &ref = *request;
    protocol_request_ = std::move(request);
    return ref;
  }
#endif

#ifdef CINATRA_ENABLE_SSL
  bool has_parser() const { return parser_ != nullptr; }

  http_parser &parser() const { return *parser_; }
#endif

#ifdef CINATRA_ENABLE_SSL
  http_parser *parser_ = nullptr;
#else
  http_parser &parser_;
#endif
  std::string_view body_;
#ifdef CINATRA_ENABLE_SSL
  coro_http_connection *conn_ = nullptr;
#else
  coro_http_connection *conn_;
#endif
  bool is_websocket_ = false;
#ifdef CINATRA_ENABLE_SSL
  std::shared_ptr<detail::protocol_request> protocol_request_;
#endif
  std::vector<std::string> aspect_data_;
  std::string cached_session_id_;
  std::any user_data_;
};
}  // namespace cinatra
