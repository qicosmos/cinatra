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
  coro_http_response() { head_.reserve(128); }
  void set_status(cinatra::status_type status) { status_ = status; }
  void set_content(std::string content) { content_ = std::move(content); }
  void set_status_and_content(status_type status, std::string content) {
    status_ = status;
    content_ = std::move(content);
  }
  std::string_view get_content() const { return content_; }
  cinatra::status_type get_status() const { return status_; }

  void add_header(auto k, auto v) {
    resp_headers_.emplace_back(std::move(k), std::move(v));
  }

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
      resp_headers_sv_.emplace_back("Host", "cinatra");
    }

    if (status_ >= status_type::not_found) {
      auto sv = to_string(status_);
      content_.reserve(sv.size());
      content_.append(sv);
    }

    if (!content_.empty()) {
      auto [ptr, ec] = std::to_chars(buf_, buf_ + 32, content_.size());
      resp_headers_sv_.emplace_back(
          "Content-Length", std::string_view(buf_, std::distance(buf_, ptr)));
    }
    else {
      resp_headers_sv_.emplace_back("Content-Length", "0");
    }

    resp_headers_sv_.emplace_back("Date", get_gmt_time_str());

    if (keepalive_.has_value()) {
      bool keepalive = keepalive_.value();
      resp_headers_sv_.emplace_back("Connection",
                                    keepalive ? "keep-alive" : "close");
    }

    append_head(resp_headers_);
    append_head(resp_headers_sv_);
    head_.append(CRCF);
  }

  void clear() {
    head_.clear();
    content_.clear();
    resp_headers_.clear();
    resp_headers_sv_.clear();
    keepalive_ = {};
  }

  size_t get_head_size() {
    size_t size = 0;
    for (auto& [k, v] : resp_headers_) {
      size += k.size() + 3 + v.size();
    }
    return size + 2;
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
  char buf_[32];
  std::vector<resp_header> resp_headers_;
  std::vector<resp_header_sv> resp_headers_sv_;
};
}  // namespace cinatra