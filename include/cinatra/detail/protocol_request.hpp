#pragma once

#include <span>
#include <string_view>
#include <unordered_map>

#include "cinatra/picohttpparser.h"

namespace cinatra::detail {

class protocol_request {
 public:
  virtual ~protocol_request() = default;

  virtual std::string_view get_query_value(std::string_view key) const = 0;
  virtual std::span<http_header> get_headers() = 0;
  virtual std::span<http_header> get_trailers() = 0;
  virtual const std::unordered_map<std::string_view, std::string_view>&
  get_queries() const = 0;
  virtual std::string_view full_url() const = 0;
  virtual std::string_view body() const = 0;
  virtual bool is_chunked() const = 0;
  virtual std::string_view url() const = 0;
  virtual std::string_view method() const = 0;
  virtual std::string_view scheme() const = 0;
  virtual std::string_view authority() const = 0;
  virtual std::string_view protocol() const = 0;
};

}  // namespace cinatra::detail
