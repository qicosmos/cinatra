#pragma once
#include <async_simple/Executor.h>
#include <async_simple/coro/SyncAwait.h>

#include <asio/buffer.hpp>

#include "asio/streambuf.hpp"
#include "define.h"
#include "http_parser.hpp"
#include "ylt/coro_io/coro_io.hpp"

namespace cinatra {
class coro_connection {
 public:
  template <typename executor_t>
  coro_connection(executor_t *executor, asio::ip::tcp::socket socket)
      : executor_(executor), socket_(std::move(socket)) {}

  ~coro_connection() {}

  async_simple::coro::Lazy<void> start() {
    while (true) {
      auto [ec, size] = co_await async_read_until(head_buf_, TWO_CRCF);
      if (ec) {
        // handle error
      }

      const char *data_ptr = asio::buffer_cast<const char *>(head_buf_.data());
      parser_.parse_request(data_ptr, size, 0);
      auto headers = parser_.get_headers();
      for (auto [k, v] : headers) {
        std::cout << k << ": " << v << "\n";
      }
      //   ec = co_await read_body();
      //   route();
      //   prepare_response();
      //   send_response();
    }
    co_return;
  }

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_until(
      AsioBuffer &buffer, asio::string_view delim) noexcept {
    return coro_io::async_read_until(socket_, buffer, delim);
  }

  auto &get_executor() { return *executor_; }

 private:
  async_simple::Executor *executor_;
  asio::ip::tcp::socket socket_;
  asio::streambuf head_buf_;
  std::string body_;
  http_parser parser_;
};
}  // namespace cinatra