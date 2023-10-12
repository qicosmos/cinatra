#pragma once
#include <async_simple/Executor.h>
#include <async_simple/coro/SyncAwait.h>

#include <asio/buffer.hpp>
#include <system_error>
#include <thread>

#include "asio/dispatch.hpp"
#include "asio/streambuf.hpp"
#include "async_simple/coro/Lazy.h"
#include "cinatra/cinatra_log_wrapper.hpp"
#include "coro_http_request.hpp"
#include "coro_http_router.hpp"
#include "define.h"
#include "http_parser.hpp"
#include "string_resize.hpp"
#include "ylt/coro_io/coro_io.hpp"

namespace cinatra {
struct chunked_result {
  std::error_code ec;
  bool eof = false;
  std::string_view data;
};

class coro_http_connection {
 public:
  template <typename executor_t>
  coro_http_connection(executor_t *executor, asio::ip::tcp::socket socket)
      : executor_(executor),
        socket_(std::move(socket)),
        request_(parser_, this) {
    buffers_.reserve(3);
    response_.set_response_cb([this]() -> async_simple::coro::Lazy<void> {
      co_await reply();
    });
  }

  ~coro_http_connection() { close(); }

  async_simple::coro::Lazy<void> start() {
    while (true) {
      auto [ec, size] = co_await async_read_until(head_buf_, TWO_CRCF);
      if (ec) {
        CINATRA_LOG_ERROR << "read http header error: " << ec.message();
        close();
        break;
      }

      const char *data_ptr = asio::buffer_cast<const char *>(head_buf_.data());
      int head_len = parser_.parse_request(data_ptr, size, 0);
      if (head_len <= 0) {
        CINATRA_LOG_ERROR << "parse http header error";
        close();
        break;
      }

      head_buf_.consume(size);
      keep_alive_ = check_keep_alive();

      if (!parser_.is_chunked()) {
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
              asio::buffer(body_.data() + part_size, size_to_read),
              size_to_read);
          if (ec) {
            CINATRA_LOG_ERROR << "async_read error: " << ec.message();
            close();
            break;
          }
        }
      }

      std::string_view key = {
          parser_.method().data(),
          parser_.method().length() + 1 + parser_.url().length()};

      if (!body_.empty()) {
        request_.set_body(body_);
      }

      auto &router = coro_http_router::instance();
      if (auto handler = router.get_handler(key); handler) {
        router.route(handler, request_, response_);
      }
      else {
        if (auto coro_handler = router.get_coro_handler(key); coro_handler) {
          co_await router.route_coro(coro_handler, request_, response_);
        }
        else {
          // not found
          response_.set_status(status_type::not_found);
        }
      }

      if (!response_.get_delay()) {
        co_await reply();
      }

      response_.clear();
      buffers_.clear();
      body_.clear();
    }
  }

  async_simple::coro::Lazy<void> reply() {
    // avoid duplicate reply
    response_.to_buffers(buffers_);
    auto [ec, _] = co_await async_write(buffers_);
    if (ec) {
      CINATRA_LOG_ERROR << "async_write error: " << ec.message();
      close();
      co_return;
    }

    if (!keep_alive_) {
      // now in io thread, so can close socket immediately.
      close();
    }
  }

  async_simple::coro::Lazy<chunked_result> read_chunked() {
    if (head_buf_.size() > 0) {
      const char *data_ptr = asio::buffer_cast<const char *>(head_buf_.data());
      chunked_buf_.sputn(data_ptr, head_buf_.size());
      head_buf_.consume(head_buf_.size());
    }

    chunked_result result{};
    std::error_code ec{};
    size_t size = 0;

    if (std::tie(ec, size) = co_await async_read_until(chunked_buf_, CRCF);
        ec) {
      result.ec = ec;
      close();
      co_return result;
    }

    size_t buf_size = chunked_buf_.size();
    size_t additional_size = buf_size - size;
    const char *data_ptr = asio::buffer_cast<const char *>(chunked_buf_.data());
    std::string_view size_str(data_ptr, size - CRCF.size());
    size_t chunk_size;
    auto [ptr, err] = std::from_chars(
        size_str.data(), size_str.data() + size_str.size(), chunk_size, 16);
    if (err != std::errc{}) {
      CINATRA_LOG_ERROR << "bad chunked size";
      result.ec = std::make_error_code(std::errc::invalid_argument);
      co_return result;
    }

    chunked_buf_.consume(size);

    if (chunk_size == 0) {
      // all finished, no more data
      chunked_buf_.consume(CRCF.size());
      result.eof = true;
      co_return result;
    }

    if (additional_size < size_t(chunk_size + 2)) {
      // not a complete chunk, read left chunk data.
      size_t size_to_read = chunk_size + 2 - additional_size;
      if (std::tie(ec, size) = co_await async_read(chunked_buf_, size_to_read);
          ec) {
        result.ec = ec;
        close();
        co_return result;
      }
    }

    data_ptr = asio::buffer_cast<const char *>(chunked_buf_.data());
    result.data = std::string_view{data_ptr, (size_t)chunk_size};
    chunked_buf_.consume(chunk_size + CRCF.size());

    co_return result;
  }

  auto &socket() { return socket_; }

  void set_quit_callback(std::function<void(const uint64_t &conn_id)> callback,
                         uint64_t conn_id) {
    quit_cb_ = std::move(callback);
    conn_id_ = conn_id;
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

  void close() {
    if (has_closed_) {
      return;
    }

    asio::dispatch(socket_.get_executor(), [this] {
      std::error_code ec;
      socket_.shutdown(asio::socket_base::shutdown_both, ec);
      socket_.close(ec);
      if (quit_cb_) {
        quit_cb_(conn_id_);
      }
      has_closed_ = true;
    });
  }

 private:
  bool check_keep_alive() {
    bool keep_alive = true;
    auto val = request_.get_header_value("connection");
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
  asio::streambuf chunked_buf_;
  http_parser parser_;
  bool keep_alive_;
  coro_http_response response_;
  coro_http_request request_;
  std::vector<asio::const_buffer> buffers_;
  std::atomic<bool> has_closed_{false};
  uint64_t conn_id_{0};
  std::function<void(const uint64_t &conn_id)> quit_cb_ = nullptr;
};
}  // namespace cinatra