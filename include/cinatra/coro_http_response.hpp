#pragma once
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "define.h"
#include "response_cv.hpp"
#include "time_util.hpp"

namespace cinatra {
class coro_http_response {
 public:
  void set_status(cinatra::status_type status) { status_ = status; }
  void set_content(std::string content) { content_ = std::move(content); }
  void set_status_and_content(status_type status, std::string content) {
    status_ = status;
    content_ = std::move(content);
  }
  std::string_view get_content() const { return content_; }
  cinatra::status_type get_status() const { return status_; }

  void add_header(auto k, auto v) {
    resp_headers_.emplace(std::move(k), std::move(v));
  }

  void set_keepalive(bool r) { keepalive_ = r; }

  std::vector<asio::const_buffer> to_buffers() {
    build_resp_head();

    std::vector<asio::const_buffer> buffers;
    buffers.reserve(3);
    buffers.push_back(asio::buffer(to_rep_string(status_)));
    buffers.push_back(asio::buffer(head_));
    if (!content_.empty()) {
      buffers.push_back(asio::buffer(content_));
    }

    return buffers;
  }

  void build_resp_head() {
    if (resp_headers_.find("Host") == resp_headers_.end()) {
      resp_headers_.emplace("Host", "cinatra");
    }

    if (status_ >= status_type::not_found) {
      auto sv = to_string(status_);
      content_.reserve(sv.size());
      content_.append(sv);
    }

    if (!content_.empty()) {
      char buf[32];
      auto [ptr, ec] = std::to_chars(buf, buf + 32, content_.size());
      resp_headers_.emplace("Content-Length",
                            std::string_view(buf, std::distance(buf, ptr)));
    }
    else {
      resp_headers_.emplace("Content-Length", "0");
    }

    resp_headers_.emplace("Date", get_gmt_time_str());

    if (keepalive_.has_value()) {
      bool keepalive = keepalive_.value();
      resp_headers_.emplace("Connection", keepalive ? "keep-alive" : "close");
    }

    size_t head_size = get_head_size();
    head_.reserve(head_size);
    append_head();
    assert(head_.size() == head_size);
  }

  void clear() {
    head_.clear();
    content_.clear();
    resp_headers_.clear();
    keepalive_ = {};
  }

  size_t get_head_size() {
    size_t size = 0;
    for (auto& [k, v] : resp_headers_) {
      size += k.size() + 3 + v.size();
    }
    return size + 2;
  }

  void append_head() {
    for (auto& [k, v] : resp_headers_) {
      head_.append(k);
      head_.append(":");
      head_.append(v);
      head_.append(CRCF);
    }
    head_.append(CRCF);
  }

 private:
  status_type status_;
  std::string head_;
  std::string content_;
  std::optional<bool> keepalive_;
  std::unordered_map<std::string, std::string> resp_headers_;
};
}  // namespace cinatra