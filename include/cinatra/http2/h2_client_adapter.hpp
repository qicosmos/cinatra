#pragma once

#ifdef CINATRA_ENABLE_SSL

#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cinatra/define.h"
#include "cinatra/uri.hpp"
#include "cinatra/utils.hpp"
#include "h2_client.hpp"

namespace cinatra::http2 {

struct h2_client_adapter_response {
  bool fallback_to_http1 = false;
  std::error_code net_err;
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

class h2_client_adapter {
 public:
  explicit h2_client_adapter(coro_io::ExecutorWrapper<> *executor)
      : client_(executor) {}

  void close() {
    client_.close();
    connected_ = false;
    host_.clear();
    port_.clear();
    authority_.clear();
  }

  bool connected() const noexcept { return connected_; }

  async_simple::coro::Lazy<std::error_code> async_connect(const uri_t &u) {
    co_return co_await ensure_connected(u);
  }

  async_simple::coro::Lazy<h2_client_adapter_response> async_request(
      const uri_t &u, http_method method, req_content_type content_type,
      std::string_view body,
      const std::unordered_map<std::string, std::string> &headers) {
    h2_client_adapter_response result;
    auto ec = co_await ensure_connected(u);
    if (ec) {
      if (ec == std::make_error_code(std::errc::protocol_error)) {
        result.fallback_to_http1 = true;
      }
      else {
        result.net_err = ec;
        result.status = 404;
      }
      co_return result;
    }

    h2_client_request req;
    req.method = std::string(method_name(method));
    req.scheme = "https";
    req.authority = authority_;
    req.path = path_with_query(u);
    req.body.assign(body.data(), body.size());

    if (!append_headers(req, content_type, headers)) {
      result.fallback_to_http1 = true;
      co_return result;
    }

    auto h2_resp = co_await client_.async_request(std::move(req));
    result.net_err = h2_resp.net_err;
    result.status = h2_resp.status_code;
    result.body = std::move(h2_resp.body);
    result.headers.reserve(h2_resp.headers.size());
    for (auto &header : h2_resp.headers) {
      result.headers.emplace_back(std::move(header.name),
                                  std::move(header.value));
    }
    co_return result;
  }

  async_simple::coro::Lazy<h2_client_adapter_response> async_request(
      std::string_view path, http_method method, req_content_type content_type,
      std::string_view body,
      const std::unordered_map<std::string, std::string> &headers) {
    h2_client_adapter_response result;
    if (!connected_) {
      result.fallback_to_http1 = true;
      co_return result;
    }

    h2_client_request req;
    req.method = std::string(method_name(method));
    req.scheme = "https";
    req.authority = authority_;
    req.path.assign(path.data(), path.size());
    req.body.assign(body.data(), body.size());

    if (!append_headers(req, content_type, headers)) {
      result.fallback_to_http1 = true;
      co_return result;
    }

    auto h2_resp = co_await client_.async_request(std::move(req));
    result.net_err = h2_resp.net_err;
    result.status = h2_resp.status_code;
    result.body = std::move(h2_resp.body);
    result.headers.reserve(h2_resp.headers.size());
    for (auto &header : h2_resp.headers) {
      result.headers.emplace_back(std::move(header.name),
                                  std::move(header.value));
    }
    co_return result;
  }

 private:
  async_simple::coro::Lazy<std::error_code> ensure_connected(const uri_t &u) {
    auto host = u.get_host();
    auto port = u.get_port();
    auto authority = make_authority(host, port);
    if (connected_ && host == host_ && port == port_) {
      co_return std::error_code{};
    }

    client_.close();
    connected_ = false;
    auto ec = co_await client_.connect(host, port, "https", authority);
    if (!ec) {
      connected_ = true;
      host_ = std::move(host);
      port_ = std::move(port);
      authority_ = std::move(authority);
    }
    co_return ec;
  }

  static std::string path_with_query(const uri_t &u) {
    auto path = u.get_path();
    if (!u.query.empty()) {
      path.append("?").append(u.query);
    }
    return path;
  }

  static std::string make_authority(std::string_view host,
                                    std::string_view port) {
    std::string authority(host);
    if (!port.empty() && port != "443") {
      authority.append(":").append(port);
    }
    return authority;
  }

  static bool connection_specific_header(std::string_view name) {
    return name == "connection" || name == "keep-alive" ||
           name == "proxy-connection" || name == "transfer-encoding" ||
           name == "upgrade" || name == "http2-settings";
  }

  static bool has_header(const h2_client_request &req, std::string_view name) {
    for (auto &header : req.headers) {
      if (iequal0(header.name, name)) {
        return true;
      }
    }
    return false;
  }

  static bool append_headers(
      h2_client_request &req, req_content_type content_type,
      const std::unordered_map<std::string, std::string> &headers) {
    for (auto &header : headers) {
      auto name = ascii_lower_copy(header.first);
      if (name == "host") {
        req.authority = header.second;
        continue;
      }
      if (connection_specific_header(name)) {
        return false;
      }
      req.headers.push_back({std::move(name), header.second});
    }

    auto type = get_content_type_str(content_type);
    if (!type.empty() && !has_header(req, "content-type")) {
      req.headers.push_back({"content-type", std::move(type)});
    }
    return true;
  }

  coro_http2_client client_;
  bool connected_ = false;
  std::string host_;
  std::string port_;
  std::string authority_;
};

}  // namespace cinatra::http2

#endif
