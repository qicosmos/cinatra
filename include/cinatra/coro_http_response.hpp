#pragma once
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "async_simple/coro/Lazy.h"
#include "async_simple/coro/SyncAwait.h"
#include "define.h"
#include "response_cv.hpp"
#include "time_util.hpp"

namespace cinatra {
struct resp_header {
  std::string key;
  std::string value;
};

struct resp_header_sv {
  std::string_view key;
  std::string_view value;
};

class coro_http_response {
 public:
  coro_http_response() : status_(status_type::not_implemented), delay_(false) {
    head_.reserve(128);
  }
  void set_status(cinatra::status_type status) { status_ = status; }
  void set_content(std::string content) { content_ = std::move(content); }
  void set_status_and_content(status_type status, std::string content) {
    status_ = status;
    content_ = std::move(content);
  }
  void set_delay(bool r) { delay_ = r; }
  bool get_delay() const { return delay_; }

  void add_header(auto k, auto v) {
    resp_headers_.emplace_back(resp_header{std::move(k), std::move(v)});
  }

  void set_response_cb(
      std::function<async_simple::coro::Lazy<void>()> response_cb) {
    response_cb_ = std::move(response_cb);
  }

  async_simple::coro::Lazy<void> reply() { co_await response_cb_(); }

  void sync_reply() { async_simple::coro::syncAwait(reply()); }

  void set_keepalive(bool r) { keepalive_ = r; }

  void to_buffers(std::vector<asio::const_buffer>& buffers) {
    build_resp_head();

    buffers.push_back(asio::buffer(to_rep_string(status_)));
    buffers.push_back(asio::buffer(head_));
    if (!content_.empty()) {
      buffers.push_back(asio::buffer(content_));
    }
  }

  void build_resp_head() {
    if (std::find_if(resp_headers_.begin(), resp_headers_.end(),
                     [](resp_header& header) {
                       return header.key == "Host";
                     }) == resp_headers_.end()) {
      resp_headers_sv_.emplace_back(resp_header_sv{"Host", "cinatra"});
    }

    if (status_ >= status_type::not_found) {
      content_.append(to_string(status_));
    }

    if (!content_.empty()) {
      auto [ptr, ec] = std::to_chars(buf_, buf_ + 32, content_.size());
      resp_headers_sv_.emplace_back(resp_header_sv{
          "Content-Length", std::string_view(buf_, std::distance(buf_, ptr))});
    }
    else {
      resp_headers_sv_.emplace_back(resp_header_sv{"Content-Length", "0"});
    }

    resp_headers_sv_.emplace_back(resp_header_sv{"Date", get_gmt_time_str()});

    if (keepalive_.has_value()) {
      bool keepalive = keepalive_.value();
      resp_headers_sv_.emplace_back(
          resp_header_sv{"Connection", keepalive ? "keep-alive" : "close"});
    }

    append_head(resp_headers_);
    append_head(resp_headers_sv_);
    head_.append(CRCF);
  }

  void clear() {
    head_.clear();
    content_.clear();
    head_.shrink_to_fit();
    content_.shrink_to_fit();

    resp_headers_.clear();
    resp_headers_sv_.clear();
    keepalive_ = {};
    delay_ = false;
  }

  void append_head(auto& headers) {
    for (auto& [k, v] : headers) {
      head_.append(k);
      head_.append(":");
      head_.append(v);
      head_.append(CRCF);
    }
  }

 private:
  status_type status_;
  std::string head_;
  std::string content_;
  std::optional<bool> keepalive_;
  bool delay_;
  char buf_[32];
  std::vector<resp_header> resp_headers_;
  std::vector<resp_header_sv> resp_headers_sv_;
  std::function<async_simple::coro::Lazy<void>()> response_cb_;
};
}  // namespace cinatra