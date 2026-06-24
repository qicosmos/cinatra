#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cinatra/detail/protocol_request.hpp"
#include "cinatra/utils.hpp"

namespace cinatra::http2 {

struct h2_request_context : detail::protocol_request {
  h2_request_context() = default;
  h2_request_context(const h2_request_context&) = delete;
  h2_request_context& operator=(const h2_request_context&) = delete;

  void clear() {
    chunked = false;
    method_value.clear();
    scheme_value.clear();
    authority_value.clear();
    protocol_value.clear();
    url_value.clear();
    full_url_value.clear();
    body_holder.clear();
    body_view = {};
    headers_storage.clear();
    headers.clear();
    trailers_storage.clear();
    trailers.clear();
    queries_storage.clear();
    queries.clear();
  }

  void set_url(std::string value) {
    full_url_value = std::move(value);
    rebuild_url_and_query_views();
  }

  void add_header(std::string name, std::string value) {
    headers_storage.push_back({std::move(name), std::move(value)});
    auto& header = headers_storage.back();
    headers.push_back({header.first, header.second});
  }

  void add_trailer(std::string name, std::string value) {
    trailers_storage.push_back({std::move(name), std::move(value)});
    auto& trailer = trailers_storage.back();
    trailers.push_back({trailer.first, trailer.second});
  }

  void set_body(std::string value, bool parse_urlencoded) {
    body_holder = std::move(value);
    body_view = body_holder;
    if (parse_urlencoded) {
      parse_query_values(body_view);
    }
  }

  std::string_view get_query_value(std::string_view key) const override {
    if (auto it = queries.find(key); it != queries.end()) {
      return it->second;
    }
    return {};
  }

  std::span<http_header> get_headers() override {
    return {headers.data(), headers.size()};
  }

  std::span<http_header> get_trailers() override {
    return {trailers.data(), trailers.size()};
  }

  const std::unordered_map<std::string_view, std::string_view>& get_queries()
      const override {
    return queries;
  }

  std::string_view full_url() const override { return full_url_value; }

  std::string_view body() const override { return body_view; }

  bool is_chunked() const override { return chunked; }

  std::string_view url() const override { return url_value; }

  std::string_view method() const override { return method_value; }

  std::string_view scheme() const override { return scheme_value; }

  std::string_view authority() const override { return authority_value; }

  std::string_view protocol() const override { return protocol_value; }

  void rebuild_url_and_query_views() {
    queries_storage.clear();
    queries.clear();

    auto pos = full_url_value.find('?');
    if (pos == std::string::npos) {
      url_value = full_url_value;
      return;
    }

    url_value = full_url_value.substr(0, pos);
    parse_query_values(std::string_view(full_url_value).substr(pos + 1));
  }

  void parse_query_values(std::string_view str) {
    auto vec = split_sv(str, "&");
    for (auto s : vec) {
      if (s.empty()) {
        continue;
      }

      std::string_view key;
      std::string_view val;
      size_t pos = s.find('=');
      if (pos != std::string_view::npos) {
        key = s.substr(0, pos);
        if (key.empty()) {
          continue;
        }
        val = s.substr(pos + 1, s.length() - pos);
      }
      else {
        key = s;
        val = "";
      }

      queries_storage.emplace_back(std::string(key), std::string(val));
      auto& entry = queries_storage.back();
      queries.emplace(entry.first, entry.second);
    }
  }

  bool chunked = false;
  std::string method_value;
  std::string scheme_value;
  std::string authority_value;
  std::string protocol_value;
  std::string url_value;
  std::string full_url_value;
  std::string body_holder;
  std::string_view body_view;
  std::deque<std::pair<std::string, std::string>> headers_storage;
  std::vector<http_header> headers;
  std::deque<std::pair<std::string, std::string>> trailers_storage;
  std::vector<http_header> trailers;
  std::deque<std::pair<std::string, std::string>> queries_storage;
  std::unordered_map<std::string_view, std::string_view> queries;
};

}  // namespace cinatra::http2
