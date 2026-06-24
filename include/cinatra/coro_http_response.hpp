#pragma once
#include <charconv>
#include <functional>
#ifdef CINATRA_ENABLE_SSL
#include <memory>
#endif
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#ifdef CINATRA_ENABLE_SSL
#include <utility>
#endif
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

#ifdef CINATRA_ENABLE_SSL
namespace detail {
struct response_push {
  std::string method = "GET";
  std::string path = "/";
  std::string scheme;
  std::string authority;
  status_type status = status_type::ok;
  std::string body;
  std::vector<resp_header> request_headers;
  std::vector<resp_header> response_headers;
  std::vector<resp_header> response_trailers;

  void add_request_header(std::string name, std::string value) {
    request_headers.push_back({std::move(name), std::move(value)});
  }

  void add_response_header(std::string name, std::string value) {
    response_headers.push_back({std::move(name), std::move(value)});
  }

  void add_response_trailer(std::string name, std::string value) {
    response_trailers.push_back({std::move(name), std::move(value)});
  }
};

struct protocol_response {
  std::vector<response_push> pushes;
};
}  // namespace detail
#endif

enum class format_type {
  normal,
  chunked,
};

class coro_http_connection;
#ifdef CINATRA_ENABLE_SSL
class coro_http_response;
namespace http2 {
class coro_http2_connection;
using response_push = detail::response_push;
response_push &add_push(coro_http_response &resp, std::string path,
                        std::string body = {},
                        status_type status = status_type::ok);
}  // namespace http2
#endif

class coro_http_response {
 public:
#ifdef CINATRA_ENABLE_SSL
  coro_http_response()
      : status_(status_type::not_implemented),
        fmt_type_(format_type::normal),
        delay_(false),
        conn_(nullptr) {}
#endif
  coro_http_response(coro_http_connection *conn)
      : status_(status_type::not_implemented),
        fmt_type_(format_type::normal),
        delay_(false),
        conn_(conn) {}

  void set_status(cinatra::status_type status) { status_ = status; }
  void set_content(std::string content) {
    content_ = std::move(content);
    has_set_content_ = true;
  }
  void set_status_and_content(
      status_type status, std::string content,
      content_encoding encoding = content_encoding::none,
      std::string_view client_encoding_type = "") {
    set_status_and_content_view(status, std::move(content), encoding, false,
                                client_encoding_type);
  }

  template <typename String>
  void set_status_and_content_view(
      status_type status, String content = "",
      content_encoding encoding = content_encoding::none, bool is_view = true,
      std::string_view client_encoding_type = "") {
    status_ = status;
#ifdef CINATRA_ENABLE_GZIP
    if (encoding == content_encoding::gzip) {
      if (client_encoding_type.empty() ||
          client_encoding_type.find("gzip") != std::string_view::npos) {
        std::string encode_str;
        bool r = gzip_codec::compress(content, encode_str);
        if (!r) {
          set_status_and_content(status_type::internal_server_error,
                                 "gzip compress error");
        }
        else {
          add_header("Content-Encoding", "gzip");
          set_content(std::move(encode_str));
        }
      }
      else {
        if (is_view) {
          content_view_ = content;
        }
        else {
          content_ = std::move(content);
        }
      }
      has_set_content_ = true;
      return;
    }

    if (encoding == content_encoding::deflate) {
      if (client_encoding_type.empty() ||
          client_encoding_type.find("deflate") != std::string_view::npos) {
        std::string deflate_str;
        bool r = gzip_codec::deflate(content, deflate_str);
        if (!r) {
          set_status_and_content(status_type::internal_server_error,
                                 "deflate compress error");
        }
        else {
          add_header("Content-Encoding", "deflate");
          set_content(std::move(deflate_str));
        }
      }
      else {
        if (is_view) {
          content_view_ = content;
        }
        else {
          content_ = std::move(content);
        }
      }
      has_set_content_ = true;
      return;
    }
#endif

#ifdef CINATRA_ENABLE_BROTLI
    if (encoding == content_encoding::br) {
      if (client_encoding_type.empty() ||
          client_encoding_type.find("br") != std::string_view::npos) {
        std::string br_str;
        bool r = br_codec::brotli_compress(content, br_str);
        if (!r) {
          set_status_and_content(status_type::internal_server_error,
                                 "br compress error");
        }
        else {
          add_header("Content-Encoding", "br");
          set_content(std::move(br_str));
        }
      }
      else {
        if (is_view) {
          content_view_ = content;
        }
        else {
          content_ = std::move(content);
        }
      }
      has_set_content_ = true;
      return;
    }
#endif

    if (is_view) {
      content_view_ = content;
    }
    else {
      content_ = std::move(content);
    }
    has_set_content_ = true;
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
#ifdef CINATRA_ENABLE_SSL
  std::string_view content() const { return content_; }
#endif
  size_t content_size() { return content_.size(); }
#ifdef CINATRA_ENABLE_SSL
  size_t content_size() const { return content_.size(); }
#endif

  void add_header(auto k, auto v) {
    resp_headers_.emplace_back(resp_header{std::move(k), std::move(v)});
  }

#ifdef CINATRA_ENABLE_SSL
  void add_trailer(auto k, auto v) {
    trailers_.emplace_back(resp_header{std::move(k), std::move(v)});
  }
#endif

  void add_header_span(std::span<http_header> resp_headers) {
    resp_header_span_ = resp_headers;
  }

  void set_keepalive(bool r) { keepalive_ = r; }

  void need_date_head(bool r) { need_date_ = r; }
  bool need_date() { return need_date_; }

  void set_boundary(std::string_view boundary) { boundary_ = boundary; }

  std::string_view get_boundary() { return boundary_; }
#ifdef CINATRA_ENABLE_SSL
  std::string_view get_boundary() const { return boundary_; }

  void bind_connection(coro_http_connection *conn) { conn_ = conn; }
#endif

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

    if (!has_host) {
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
      if (!content_.empty() || !content_view_.empty()) {
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

    if (!has_host) {
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
#ifdef CINATRA_ENABLE_SSL
  coro_http_connection *get_conn() const { return conn_; }
#endif

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
#ifdef CINATRA_ENABLE_SSL
    trailers_.clear();
    protocol_response_.reset();
#endif
    need_date_ = true;
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
#ifdef CINATRA_ENABLE_SSL
  friend class http2::coro_http2_connection;
  friend http2::response_push &http2::add_push(coro_http_response &resp,
                                               std::string path,
                                               std::string body,
                                               status_type status);

  detail::protocol_response &mutable_protocol_response() {
    if (!protocol_response_) {
      protocol_response_ = std::make_unique<detail::protocol_response>();
    }
    return *protocol_response_;
  }

  const detail::protocol_response *protocol_response() const {
    return protocol_response_.get();
  }

  const std::vector<resp_header> &headers() const { return resp_headers_; }
  std::span<const http_header> header_span() const { return resp_header_span_; }
  const std::vector<resp_header> &trailers() const { return trailers_; }
  const std::unordered_map<std::string, cookie> &cookies() const {
    return cookies_;
  }
  bool has_explicit_content() const { return has_set_content_; }
  std::string_view content_view() const { return content_view_; }
  std::string_view body_view() const {
    return content_.empty() ? content_view_ : std::string_view(content_);
  }
  std::string_view content_type_value() const {
    if (content_type_.empty()) {
      return {};
    }

    constexpr std::string_view prefix = "Content-Type: ";
    constexpr std::string_view suffix = "\r\n";
    auto value = content_type_;
    if (value.starts_with(prefix)) {
      value.remove_prefix(prefix.size());
    }
    if (value.ends_with(suffix)) {
      value.remove_suffix(suffix.size());
    }
    return value;
  }
#endif

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
  std::unordered_map<std::string, cookie> cookies_;
#ifdef CINATRA_ENABLE_SSL
  std::vector<resp_header> trailers_;
  std::unique_ptr<detail::protocol_response> protocol_response_;
#endif
  std::string_view content_type_;
  std::string_view content_view_;
};

#ifdef CINATRA_ENABLE_SSL
namespace http2 {
inline response_push &add_push(coro_http_response &resp, std::string path,
                               std::string body, status_type status) {
  auto &push = resp.mutable_protocol_response().pushes.emplace_back();
  push.path = std::move(path);
  push.body = std::move(body);
  push.status = status;
  return push;
}
}  // namespace http2
#endif
}  // namespace cinatra
