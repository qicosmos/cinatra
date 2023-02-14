#pragma once

#include <memory>
#include <thread>

#include "asio/io_context.hpp"
#include "asio_util/asio_coro_util.hpp"
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
  coro_http_client() {
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

 private:
  asio::io_context io_ctx_;
  std::unique_ptr<asio::io_context::work> work_;
  std::thread io_thd_;
};
}  // namespace cinatra