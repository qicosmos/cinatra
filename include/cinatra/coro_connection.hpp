#pragma once
#include <async_simple/Executor.h>
#include <async_simple/coro/SyncAwait.h>

#include <asio/buffer.hpp>
#include <thread>

#include "asio/streambuf.hpp"
#include "coro_http_router.hpp"
#include "define.h"
#include "http_parser.hpp"
#include "string_resize.hpp"
#include "ylt/coro_io/coro_io.hpp"

inline std::string g_resp_str =
    "HTTP/1.1 200 OK\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 11\r\n"
    "Content-Type: text/html\r\n"
    "Host: cinatra\r\n\r\n"
    "hello world";

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
        CINATRA_LOG_ERROR << "read http header error: " << ec.message();
        co_return;
      }

      const char *data_ptr = asio::buffer_cast<const char *>(head_buf_.data());
      int head_len = parser_.parse_request(data_ptr, size, 0);
      if (head_len <= 0) {
        CINATRA_LOG_ERROR << "parse http header error";
        co_return;
      }

      head_buf_.consume(size);
      keep_alive_ = check_keep_alive();

      size_t body_len = parser_.body_len();
      if (body_len <= head_buf_.size()) {
        if (body_len > 0) {
          detail::resize(body_, body_len);
          auto data_ptr = asio::buffer_cast<const char *>(head_buf_.data());
          memcpy(body_.data(), data_ptr, body_len);
          head_buf_.consume(head_buf_.size());
        }
      }
      else {
        size_t part_size = head_buf_.size();
        size_t size_to_read = body_len - part_size;
        auto data_ptr = asio::buffer_cast<const char *>(head_buf_.data());
        detail::resize(body_, body_len);
        memcpy(body_.data(), data_ptr, part_size);
        head_buf_.consume(part_size);

        auto [ec, size] = co_await async_read(
            asio::buffer(body_.data() + part_size, size_to_read), size_to_read);
      }

      std::string_view key = {
          parser_.method().data(),
          parser_.method().length() + 1 + parser_.url().length()};

      auto &router = coro_http_router::instance();
      if (auto handler = router.get_handler(key); handler) {
        (*handler)(response_);
      }
      else {
        if (auto coro_handler = router.get_coro_handler(key); coro_handler) {
          co_await (*coro_handler)(response_);
        }
        else {
          // not found
        }
      }

      co_await async_write(asio::buffer(g_resp_str));

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

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
      AsioBuffer &&buffer, size_t size_to_read) noexcept {
    return coro_io::async_read(socket_, buffer, size_to_read);
  }

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_write(
      AsioBuffer &&buffer) {
    return coro_io::async_write(socket_, buffer);
  }

  auto &get_executor() { return *executor_; }

 private:
  void build_response() {}
  inline bool iequal(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char a, char b) {
                        return tolower(a) == tolower(b);
                      });
  }

  std::string_view get_header_value(std::string_view key) {
    auto headers = parser_.get_headers();
    for (auto header : headers) {
      if (iequal(header.name, key)) {
        return header.value;
      }
    }

    return {};
  }

  bool check_keep_alive() {
    bool keep_alive = true;
    auto val = get_header_value("connection");
    if (!val.empty() && iequal(val, "close")) {
      keep_alive = false;
    }

    return keep_alive;
  }

 private:
  async_simple::Executor *executor_;
  asio::ip::tcp::socket socket_;
  asio::streambuf head_buf_;
  std::string body_;
  http_parser parser_;
  bool keep_alive_;
  coro_http_response response_;
};
}  // namespace cinatra