#pragma once
#include <charconv>
#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "async_simple/coro/Lazy.h"
#include "async_simple/coro/SyncAwait.h"
#include "cookie.hpp"
#include "define.h"
#ifdef CINATRA_ENABLE_GZIP
#include "gzip.hpp"
#endif
#ifdef CINATRA_ENABLE_BROTLI
#include "brzip.hpp"
#endif
#include "picohttpparser.h"
#include "response_cv.hpp"
#include "time_util.hpp"
#include "utils.hpp"

namespace cinatra {
struct resp_header {
  std::string key;
  std::string value;
};

struct resp_header_sv {
  std::string_view key;
  std::string_view value;
};

enum class format_type {
  normal,
  chunked,
};

namespace detail {
template <typename Content>
concept response_content =
    std::convertible_to<Content, std::string_view> ||
    (std::invocable<Content &, std::string &>
#ifdef CINATRA_ENABLE_GZIP
     && std::invocable<Content &, gzip_codec::compress_stream &>
#endif
    );

// The empty argument keeps the response API's default (no client preference).
// Explicit coding tokens override '*'; a zero weight wins over duplicates.
inline bool accepts_content_encoding(std::string_view header,
                                     std::string_view encoding) {
  if (header.empty()) {
    return true;
  }
  auto trim = [](std::string_view value) {
    auto start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) {
      return std::string_view{};
    }
    return value.substr(start, value.find_last_not_of(" \t") - start + 1);
  };
  auto equal = [](std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
      auto lower = [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
      };
      if (lower(left[i]) != lower(right[i])) {
        return false;
      }
    }
    return true;
  };
  int quality = -1;
  int wildcard = -1;
  while (!header.empty()) {
    auto comma = header.find(',');
    auto entry = trim(header.substr(0, comma));
    header = comma == std::string_view::npos ? std::string_view{}
                                             : header.substr(comma + 1);
    auto semicolon = entry.find(';');
    auto coding = trim(entry.substr(0, semicolon));
    bool matches = equal(coding, encoding) ||
                   (encoding == "gzip" && equal(coding, "x-gzip"));
    if (!matches && coding != "*") {
      continue;
    }
    int weight = 1000;
    if (semicolon != std::string_view::npos) {
      auto parameter = trim(entry.substr(semicolon + 1));
      auto equals = parameter.find('=');
      if (equals == std::string_view::npos ||
          !equal(trim(parameter.substr(0, equals)), "q")) {
        weight = 0;
      }
      else {
        auto value = trim(parameter.substr(equals + 1));
        if (value.empty() || (value[0] != '0' && value[0] != '1') ||
            value.size() > 5 || (value.size() > 1 && value[1] != '.')) {
          weight = 0;
        }
        else {
          weight = value[0] == '1' ? 1000 : 0;
          int place = 100;
          for (size_t i = 2; i < value.size(); ++i, place /= 10) {
            if (value[i] < '0' || value[i] > '9' ||
                (value[0] == '1' && value[i] != '0')) {
              weight = 0;
              break;
            }
            weight += (value[i] - '0') * place;
          }
        }
      }
    }
    auto &selected = matches ? quality : wildcard;
    if (selected == -1 || weight < selected) {
      selected = weight;
    }
  }
  return (quality == -1 ? wildcard : quality) > 0;
}
}  // namespace detail

class coro_http_connection;
class coro_http_response {
 public:
  coro_http_response(coro_http_connection *conn)
      : status_(status_type::not_implemented),
        fmt_type_(format_type::normal),
        delay_(false),
        conn_(conn) {}

  void set_status(cinatra::status_type status) { status_ = status; }
  void set_content(std::string content) {
    content_ = std::move(content);
    content_view_ = {};
    has_set_content_ = true;
  }
  // Content can also be a synchronous writer, e.g.
  // [&](auto &stream) { iguana::to_json(value, stream); }.
  // With gzip enabled, writers must accept both std::string & and
  // gzip_codec::compress_stream & so that uncompressed fallbacks work.
  // Exceptions propagate without replacing the previous response.
  template <detail::response_content Content = std::string>
  void set_status_and_content(
      status_type status, Content content,
      content_encoding encoding = content_encoding::none,
      std::string_view client_encoding_type = "") {
    // Keep ordinary owned responses independent of codec dispatch/inlining.
    if constexpr (!std::is_invocable_v<Content &, std::string &>) {
      if (encoding == content_encoding::none) {
        content_ = std::move(content);
        content_view_ = {};
        status_ = status;
        has_set_content_ = true;
        return;
      }
    }
    set_status_and_content_view(status, std::move(content), encoding, false,
                                client_encoding_type);
  }

  template <detail::response_content String = std::string_view>
  void set_status_and_content_view(
      status_type status, String &&content = "",
      content_encoding encoding = content_encoding::none, bool is_view = true,
      std::string_view client_encoding_type = "") {
#ifdef CINATRA_ENABLE_GZIP
    if (encoding == content_encoding::gzip &&
        detail::accepts_content_encoding(client_encoding_type, "gzip")) {
      std::string encode_str;
      bool r = [&] {
        if constexpr (std::is_invocable_v<String &, std::string &>) {
          return gzip_codec::compress(content, encode_str);
        }
        else {
          return gzip_codec::compress(std::string_view(content), encode_str);
        }
      }();
      if (!r) {
        set_status_and_content(status_type::internal_server_error,
                               "gzip compress error");
      }
      else {
        add_header("Content-Encoding", "gzip");
        set_content(std::move(encode_str));
        status_ = status;
      }
      has_set_content_ = true;
      return;
    }
#endif

    decltype(auto) body = [&]() -> decltype(auto) {
      if constexpr (std::is_invocable_v<String &, std::string &>) {
        std::string serialized;
        content(serialized);
        is_view = false;
        return serialized;
      }
      else {
        return std::forward<String>(content);
      }
    }();
    // Borrow lvalue strings, but retain temporary strings in the response.
    if constexpr (std::same_as<std::remove_cvref_t<String>, std::string> &&
                  !std::is_lvalue_reference_v<String>) {
      is_view = false;
    }
#ifdef CINATRA_ENABLE_GZIP
    if (encoding == content_encoding::deflate &&
        detail::accepts_content_encoding(client_encoding_type, "deflate")) {
      std::string deflate_str;
      bool r = gzip_codec::deflate(body, deflate_str);
      if (!r) {
        set_status_and_content(status_type::internal_server_error,
                               "deflate compress error");
      }
      else {
        add_header("Content-Encoding", "deflate");
        set_content(std::move(deflate_str));
        status_ = status;
      }
      has_set_content_ = true;
      return;
    }
#endif

#ifdef CINATRA_ENABLE_BROTLI
    if (encoding == content_encoding::br &&
        detail::accepts_content_encoding(client_encoding_type, "br")) {
      std::string br_str;
      bool r = br_codec::brotli_compress(body, br_str);
      if (!r) {
        set_status_and_content(status_type::internal_server_error,
                               "br compress error");
      }
      else {
        add_header("Content-Encoding", "br");
        set_content(std::move(br_str));
        status_ = status;
      }
      has_set_content_ = true;
      return;
    }
#endif

    if (is_view) {
      std::string_view view(body);
      status_ = status;
      content_view_ = view;
    }
    else {
      content_ = std::forward<decltype(body)>(body);
      content_view_ = {};
      status_ = status;
    }
    has_set_content_ = true;
  }

  // Only explicit template arguments select this compatibility overload.
  // Retain their former by-value semantics without borrowing a local string.
  template <detail::response_content String>
    requires(!std::is_reference_v<String>)
  void set_status_and_content_view(
      status_type status, const std::type_identity_t<String> &content,
      content_encoding encoding = content_encoding::none, bool is_view = true,
      std::string_view client_encoding_type = "") {
    set_status_and_content_view(status, String(content), encoding, is_view,
                                client_encoding_type);
  }
  void set_delay(bool r) { delay_ = r; }
  bool get_delay() const { return delay_; }
  void set_format_type(format_type type) { fmt_type_ = type; }
  template <size_t N>
  void set_content_type() {
    content_type_ = get_content_type<N>();
  }

  status_type status() { return status_; }
  std::string_view content() { return content_; }
  size_t content_size() { return content_.size(); }

  void add_header(auto k, auto v) {
    resp_headers_.emplace_back(resp_header{std::move(k), std::move(v)});
  }

  void add_header_span(std::span<http_header> resp_headers) {
    resp_header_span_ = resp_headers;
  }

  void set_keepalive(bool r) { keepalive_ = r; }

  void need_date_head(bool r) { need_date_ = r; }
  bool need_date() { return need_date_; }
  void need_server_head(bool r) { need_server_ = r; }

  void set_boundary(std::string_view boundary) { boundary_ = boundary; }

  std::string_view get_boundary() { return boundary_; }

  void to_buffers(std::vector<asio::const_buffer> &buffers,
                  std::string &size_str) {
    buffers.push_back(asio::buffer(to_http_status_string(status_)));
    build_resp_head(buffers);
    if (!content_.empty()) {
      handle_content(buffers, size_str, content_);
    }
    else if (!content_view_.empty()) {
      handle_content(buffers, size_str, content_view_);
    }
  }

  void build_resp_str(std::string &resp_str) {
    resp_str.append(to_http_status_string(status_));
    bool has_len = false;
    bool has_host = false;
    check_header(resp_headers_, has_len, has_host);
    if (!resp_header_span_.empty()) {
      check_header(resp_header_span_, has_len, has_host);
    }

    if (need_server_ && !has_host) {
      resp_str.append(CINATRA_HOST_SV);
    }

    if (content_.empty() && !has_set_content_ &&
        fmt_type_ != format_type::chunked) {
      content_.append(default_status_content(status_));
    }

    if (fmt_type_ == format_type::chunked) {
      resp_str.append(TRANSFER_ENCODING_SV);
    }
    else {
      if (!has_len && (!content_.empty() || !content_view_.empty())) {
        size_t content_size =
            content_.empty() ? content_view_.size() : content_.size();
        auto [ptr, ec] = std::to_chars(buf_, buf_ + 32, content_size);
        resp_str.append(CONTENT_LENGTH_SV);
        resp_str.append(std::string_view(buf_, std::distance(buf_, ptr)));
        resp_str.append(CRCF);
      }
      else {
        if (!has_len && boundary_.empty())
          resp_str.append(ZERO_LENGTH_SV);
      }
    }

    if (need_date_) {
      resp_str.append(DATE_SV);
      resp_str.append(get_gmt_time_str());
      resp_str.append(CRCF);
    }

    if (keepalive_.has_value()) {
      bool keepalive = keepalive_.value();
      keepalive ? resp_str.append(CONN_KEEP_SV)
                : resp_str.append(CONN_CLOSE_SV);
    }

    append_header_str(resp_str, resp_headers_);

    if (!resp_header_span_.empty()) {
      append_header_str(resp_str, resp_header_span_);
    }

    resp_str.append(CRCF);
    if (content_view_.empty()) {
      resp_str.append(content_);
    }
    else {
      resp_str.append(content_view_);
    }
  }

  void append_header_str(auto &resp_str, auto &resp_headers) {
    for (auto &[k, v] : resp_headers) {
      resp_str.append(k);
      resp_str.append(COLON_SV);
      resp_str.append(v);
      resp_str.append(CRCF);
    }
  }

  void check_header(auto &resp_headers, bool &has_len, bool &has_host) {
    for (auto &[k, v] : resp_headers) {
      if (k == "Server") {
        has_host = true;
      }
      else if (k == "Content-Length") {
        has_len = true;
      }
      else if (k == "Date") {
        need_date_ = false;
      }
    }
  }

  void build_resp_head(std::vector<asio::const_buffer> &buffers) {
    bool has_len = false;
    bool has_host = false;
    check_header(resp_headers_, has_len, has_host);
    if (!resp_header_span_.empty()) {
      check_header(resp_header_span_, has_len, has_host);
    }

    if (need_server_ && !has_host) {
      buffers.emplace_back(asio::buffer(CINATRA_HOST_SV));
    }

    if (content_.empty() && !has_set_content_ &&
        fmt_type_ != format_type::chunked) {
      content_.append(default_status_content(status_));
    }

    if (fmt_type_ == format_type::chunked) {
      buffers.emplace_back(asio::buffer(TRANSFER_ENCODING_SV));
    }
    else {
      if (!cookies_.empty()) {
        for (auto &[_, cookie] : cookies_) {
          resp_headers_.emplace_back(
              resp_header{"Set-Cookie", cookie.to_string()});
        }
      }

      if (!content_.empty()) {
        if (!has_len)
          handle_content_len(buffers, content_);
      }
      else if (!content_view_.empty()) {
        if (!has_len)
          handle_content_len(buffers, content_view_);
      }
      else {
        if (!has_len && boundary_.empty())
          buffers.emplace_back(asio::buffer(ZERO_LENGTH_SV));
      }
    }

    if (need_date_) {
      buffers.emplace_back(asio::buffer(DATE_SV));
      buffers.emplace_back(asio::buffer(get_gmt_time_str()));
      buffers.emplace_back(asio::buffer(CRCF));
    }

    if (keepalive_.has_value()) {
      bool keepalive = keepalive_.value();
      keepalive ? buffers.emplace_back(asio::buffer(CONN_KEEP_SV))
                : buffers.emplace_back(asio::buffer(CONN_CLOSE_SV));
    }

    if (!content_type_.empty()) {
      buffers.emplace_back(asio::buffer(content_type_));
    }

    append_header(buffers, resp_headers_);

    if (!resp_header_span_.empty()) {
      append_header(buffers, resp_header_span_);
    }

    buffers.emplace_back(asio::buffer(CRCF));
  }

  void append_header(auto &buffers, auto &resp_headers) {
    for (auto &[k, v] : resp_headers) {
      buffers.emplace_back(asio::buffer(k));
      buffers.emplace_back(asio::buffer(COLON_SV));
      buffers.emplace_back(asio::buffer(v));
      buffers.emplace_back(asio::buffer(CRCF));
    }
  }

  coro_http_connection *get_conn() { return conn_; }

  void clear() {
    content_.clear();
    if (need_shrink_every_time_) {
      content_.shrink_to_fit();
    }

    resp_headers_.clear();
    keepalive_ = {};
    delay_ = false;
    status_ = status_type::init;
    fmt_type_ = format_type::normal;
    boundary_.clear();
    has_set_content_ = false;
    cookies_.clear();
    need_date_ = true;
    need_server_ = true;
    content_type_ = {};
    content_view_ = {};
  }

  void set_shrink_to_fit(bool r) { need_shrink_every_time_ = r; }

  void add_cookie(const cookie &cookie) {
    cookies_[cookie.get_name()] = cookie;
  }

  void redirect(const std::string &url, bool is_forever = false) {
    add_header("Location", url);
    is_forever == false
        ? set_status_and_content(status_type::moved_temporarily, "")
        : set_status_and_content(status_type::moved_permanently, "");
  }

 private:
  void handle_content(std::vector<asio::const_buffer> &buffers,
                      std::string &size_str, std::string_view content) {
    if (fmt_type_ == format_type::chunked) {
      to_chunked_buffers(buffers, size_str, content, true);
    }
    else {
      buffers.push_back(asio::buffer(content));
    }
  }

  void handle_content_len(std::vector<asio::const_buffer> &buffers,
                          std::string_view content) {
    auto [ptr, ec] = std::to_chars(buf_, buf_ + 32, content.size());
    buffers.emplace_back(asio::buffer(CONTENT_LENGTH_SV));
    buffers.emplace_back(
        asio::buffer(std::string_view(buf_, std::distance(buf_, ptr))));
    buffers.emplace_back(asio::buffer(CRCF));
  }

  status_type status_;
  format_type fmt_type_;
  std::string content_;
  std::optional<bool> keepalive_;
  bool delay_;
  char buf_[32];
  std::vector<resp_header> resp_headers_;
  std::span<http_header> resp_header_span_;
  coro_http_connection *conn_;
  std::string boundary_;
  bool has_set_content_ = false;
  bool need_shrink_every_time_ = false;
  bool need_date_ = true;
  bool need_server_ = true;
  std::unordered_map<std::string, cookie> cookies_;
  std::string_view content_type_;
  std::string_view content_view_;
};
}  // namespace cinatra
