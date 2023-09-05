#pragma once
#include <async_simple/Executor.h>
#include <async_simple/coro/SyncAwait.h>

#include <asio/buffer.hpp>

#include "asio/streambuf.hpp"
#include "define.h"
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
      auto ec = co_await async_read_until(head_buf_, TWO_CRCF);

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

 private:
  async_simple::Executor *executor_;
  asio::ip::tcp::socket socket_;
  asio::streambuf head_buf_;
  std::string body_;
};
}  // namespace cinatra