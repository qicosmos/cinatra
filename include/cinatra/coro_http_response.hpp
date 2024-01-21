#pragma once
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "async_simple/coro/Lazy.h"
#include "async_simple/coro/SyncAwait.h"
#include "define.h"
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
    has_set_content_ = true;
  }
  void set_status_and_content(status_type status, std::string content = "") {
    status_ = status;
    content_ = std::move(content);
    has_set_content_ = true;
  }
  void set_delay(bool r) { delay_ = r; }
  bool get_delay() const { return delay_; }
  void set_format_type(format_type type) { fmt_type_ = type; }

  void add_header(auto k, auto v) {
    resp_headers_.emplace_back(resp_header{std::move(k), std::move(v)});
  }

  void set_keepalive(bool r) { keepalive_ = r; }

  void set_boundary(std::string_view boundary) { boundary_ = boundary; }

  std::string_view get_boundary() { return boundary_; }

  void to_buffers(std::vector<asio::const_buffer> &buffers) {
    buffers.push_back(asio::buffer(to_http_status_string(status_)));
    build_resp_head(buffers);
    if (!content_.empty()) {
      if (fmt_type_ == format_type::chunked) {
        to_chunked_buffers(buffers, content_, true);
      }
      else {
        buffers.push_back(asio::buffer(content_));
      }
    }
  }

  void build_resp_head(std::vector<asio::const_buffer> &buffers) {
    bool has_len = false;
    bool has_host = false;
    for (auto &[k, v] : resp_headers_) {
      if (k == "Host") {
        has_host = true;
      }
      if (k == "Content-Length") {
        has_len = true;
      }
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
      if (!content_.empty()) {
        auto [ptr, ec] = std::to_chars(buf_, buf_ + 32, content_.size());
        buffers.emplace_back(asio::buffer(CONTENT_LENGTH_SV));
        buffers.emplace_back(
            asio::buffer(std::string_view(buf_, std::distance(buf_, ptr))));
        buffers.emplace_back(asio::buffer(CRCF));
      }
      else {
        if (!has_len && boundary_.empty())
          buffers.emplace_back(asio::buffer(ZERO_LENGTH_SV));
      }
    }

    buffers.emplace_back(asio::buffer(DATE_SV));
    buffers.emplace_back(asio::buffer(get_gmt_time_str()));
    buffers.emplace_back(asio::buffer(CRCF));

    if (keepalive_.has_value()) {
      bool keepalive = keepalive_.value();
      keepalive ? buffers.emplace_back(asio::buffer(CONN_KEEP_SV))
                : buffers.emplace_back(asio::buffer(CONN_CLOSE_SV));
    }

    for (auto &[k, v] : resp_headers_) {
      buffers.emplace_back(asio::buffer(k));
      buffers.emplace_back(asio::buffer(COLON_SV));
      buffers.emplace_back(asio::buffer(v));
      buffers.emplace_back(asio::buffer(CRCF));
    }

    buffers.emplace_back(asio::buffer(CRCF));
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
  }

  void set_shrink_to_fit(bool r) { need_shrink_every_time_ = r; }

 private:
  status_type status_;
  format_type fmt_type_;
  std::string content_;
  std::optional<bool> keepalive_;
  bool delay_;
  char buf_[32];
  std::vector<resp_header> resp_headers_;
  coro_http_connection *conn_;
  std::string boundary_;
  bool has_set_content_ = false;
  bool need_shrink_every_time_ = false;
};
}  // namespace cinatra