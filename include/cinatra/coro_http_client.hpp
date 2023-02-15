#pragma once

#include <atomic>
#include <memory>
#include <string_view>
#include <thread>

#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio_util/asio_coro_util.hpp"
#include "async_simple/coro/Lazy.h"
#include "cinatra/define.h"
#include "cinatra/utils.hpp"
#include "http_parser.hpp"
#include "modern_callback.h"
#include "response_cv.hpp"
#include "uri.hpp"

namespace cinatra {
struct resp_data {
  std::error_code ec;
  int status;
  std::string_view resp_body;
  std::vector<std::pair<std::string, std::string>> resp_headers;
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
    close();
    work_ = nullptr;
    if (io_thd_.joinable()) {
      io_thd_.join();
    }

    std::cout << "client quit\n";
  }

  void close() {
    if (has_closed_)
      return;

    io_ctx_.post([this] {
      close_socket();
    });
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

  async_simple::coro::Lazy<resp_data> async_get(std::string uri) {
    auto [r, u] = get_uri(uri);
    auto ec = co_await asio_util::async_connect(io_ctx_, socket_, u.get_host(),
                                                u.get_port());

    resp_data data{ec};
    if (ec) {
      std::cout << ec.message() << "\n";
      data.status = (int)status_type::not_found;
      co_return data;
    }

    std::string write_msg = prepare_request_str(u);

    co_await asio_util::async_write(socket_, asio::buffer(write_msg));

    auto [err, size] =
        co_await asio_util::async_read_until(socket_, read_buf_, TWO_CRCF);
    if (err) {
      data.ec = err;
      data.status = (int)status_type::not_found;
      std::cout << ec.message() << "\n";
      co_return data;
    }

    // parse header
    const char *data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
    size_t buf_size = read_buf_.size();
    int parse_ret = parser_.parse_response(data_ptr, size, 0);
    if (parse_ret < 0) {
      close_socket();
      data.status = (int)status_type::not_implemented;
      co_return data;
    }
    read_buf_.consume(size);  // header size

    size_t content_len = (size_t)parser_.body_len();
    if (content_len == 0) {
      close_socket();
      copy_headers();
      data.status = (int)status_type::ok;
      data.resp_headers = std::move(copy_headers_);
      co_return data;
    }

    if ((size_t)parser_.total_len() <= buf_size) {
      // get entire body
      std::string_view reply(data_ptr + parser_.header_len(), content_len);
      std::cout << reply << "\n";

      read_buf_.consume(content_len);  // body size

      close_socket();

      copy_headers();
      data.status = (int)status_type::ok;
      data.resp_headers = std::move(copy_headers_);
      data.resp_body = reply;
      co_return data;
    }

    size_t size_to_read = content_len - read_buf_.size();
    copy_headers();

    co_await asio_util::async_read(socket_, read_buf_, size_to_read);

    // get entire body
    size_t data_size = read_buf_.size();
    const char *data_ptr1 = asio::buffer_cast<const char *>(read_buf_.data());

    std::string_view reply(data_ptr1, data_size);
    std::cout << reply << "\n";

    read_buf_.consume(content_len);
    close_socket();

    data.status = (int)status_type::ok;
    data.resp_headers = std::move(copy_headers_);
    data.resp_body = reply;
    co_return data;
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

  std::string prepare_request_str(const uri_t &u) {
    std::string req_str(method_name(http_method::GET));
    req_str.append(" ").append(u.get_path());
    req_str.append(" HTTP/1.1\r\nHost:").append(u.host).append("\r\n");
    req_str.append("\r\n");

    return req_str;
  }

  void copy_headers() {
    if (!copy_headers_.empty()) {
      copy_headers_.clear();
    }
    auto [headers, num_headers] = parser_.get_headers();
    for (size_t i = 0; i < num_headers; i++) {
      copy_headers_.emplace_back(
          std::string(headers[i].name, headers[i].name_len),
          std::string(headers[i].value, headers[i].value_len));
    }
  }

  void close_socket() {
    std::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    has_closed_ = true;
  }

  asio::io_context io_ctx_;
  asio::ip::tcp::socket socket_;
  std::unique_ptr<asio::io_context::work> work_;
  std::thread io_thd_;

  std::atomic<bool> has_closed_;
  asio::streambuf read_buf_;

  http_parser parser_;
  std::vector<std::pair<std::string, std::string>> copy_headers_;
};
}  // namespace cinatra