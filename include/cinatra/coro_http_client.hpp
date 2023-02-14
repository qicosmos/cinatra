#pragma once

#include <memory>
#include <thread>

#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio_util/asio_coro_util.hpp"
#include "async_simple/coro/Lazy.h"
#include "cinatra/utils.hpp"
#include "http_parser.hpp"
#include "modern_callback.h"
#include "uri.hpp"

namespace cinatra {
struct resp_data {
  std::error_code ec;
  int status;
  std::string_view resp_body;
  std::pair<phr_header *, size_t> resp_headers;
};

class coro_http_client {
 public:
  coro_http_client() : socket_(io_ctx_) {
    work_ = std::make_unique<asio::io_context::work>(io_ctx_);
    io_thd_ = std::thread([this] {
      io_ctx_.run();
    });
  }

  ~coro_http_client() {
    work_ = nullptr;
    if (io_thd_.joinable()) {
      io_thd_.join();
    }

    std::cout << "client quit\n";
  }

  async_simple::coro::Lazy<bool> async_connect(std::string uri) {
    auto [r, u] = get_uri(uri);
    if (!r) {
      std::cout << "url error";
      co_return false;
    }

    auto ec = co_await asio_util::async_connect(io_ctx_, socket_, u.get_host(),
                                                u.get_port());

    if (ec) {
      std::cout << ec.message() << "\n";
    }

    co_return !ec;
  }

 private:
  std::pair<bool, uri_t> get_uri(const std::string &uri) {
    uri_t u;
    if (!u.parse_from(uri.data())) {
      if (u.schema.empty())
        return {false, {}};

      auto new_uri = url_encode(uri);

      if (!u.parse_from(new_uri.data())) {
        return {false, {}};
      }
    }

    if (u.schema == "https"sv) {
#ifdef CINATRA_ENABLE_SSL
      upgrade_to_ssl();
#else
      // please open CINATRA_ENABLE_SSL before request https!
      assert(false);
#endif
    }

    return {true, u};
  }

  asio::io_context io_ctx_;
  asio::ip::tcp::socket socket_;
  std::unique_ptr<asio::io_context::work> work_;
  std::thread io_thd_;
};
}  // namespace cinatra