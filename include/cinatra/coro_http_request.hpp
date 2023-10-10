#pragma once
#include "http_parser.hpp"

namespace cinatra {
class coro_http_request {
 public:
  coro_http_request(http_parser& parser) : parser_(parser) {}

  std::string_view get_header_value(std::string_view key) {
    auto headers = parser_.get_headers();
    for (auto& header : headers) {
      if (iequal0(header.name, key)) {
        return header.value;
      }
    }

    return {};
  }

  std::string_view get_query_value(std::string_view key) {
    return parser_.get_query_value(key);
  }

  std::span<http_header> get_headers() const { return parser_.get_headers(); }

  const auto& get_queries() const { return parser_.queries(); }

 private:
  http_parser& parser_;
};
}  // namespace cinatra