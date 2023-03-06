#pragma once
#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>

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
#include "websocket.hpp"

namespace cinatra {
struct resp_data {
  std::error_code net_err;
  status_type status;
  std::string_view resp_body;
  std::vector<std::pair<std::string, std::string>> resp_headers;
  bool eof;
};

template <typename Stream>
struct req_context {
  req_content_type content_type = req_content_type::none;
  std::string req_str;
  std::string content;
  Stream stream;
};

struct multipart_t {
  std::string filename;
  std::string content;
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

  bool has_closed() { return has_closed_; }

  void add_header(std::string key, std::string val) {
    if (key.empty())
      return;

    if (key == "Host")
      return;

    req_headers_.emplace_back(std::move(key), std::move(val));
  }

  void set_ws_sec_key(std::string sec_key) { ws_sec_key_ = std::move(sec_key); }

  async_simple::coro::Lazy<bool> async_connect(std::string uri) {
    resp_data data{};
    auto [r, u] = handle_uri(data, uri);
    if (!r) {
      std::cout << "url error";
      co_return false;
    }

    req_context<std::string> ctx{};
    if (u.is_websocket()) {
      // build websocket http header
      add_header("Upgrade", "websocket");
      add_header("Connection", "Upgrade");
      if (ws_sec_key_.empty()) {
        ws_sec_key_ = "s//GYHa/XO7Hd2F2eOGfyA==";  // provide a random string.
      }
      add_header("Sec-WebSocket-Key", ws_sec_key_);
      add_header("Sec-WebSocket-Version", "13");
    }

    data = co_await async_request(std::move(uri), http_method::GET,
                                  std::move(ctx));
    async_read_ws().start([](auto &&) {
    });
    co_return !data.net_err;
  }

  async_simple::coro::Lazy<resp_data> async_send_ws(std::string msg,
                                                    bool need_mask = true,
                                                    opcode op = opcode::text) {
    resp_data data{};

    websocket ws{};
    if (op == opcode::close) {
      msg = ws.format_close_payload(close_code::normal, msg.data(), msg.size());
    }

    std::string encode_header = ws.encode_frame(msg, op, need_mask);
    std::vector<asio::const_buffer> buffers{
        asio::buffer(encode_header.data(), encode_header.size()),
        asio::buffer(msg.data(), msg.size())};

    auto [ec, _] = co_await asio_util::async_write(socket_, buffers);
    if (ec) {
      data.net_err = ec;
      data.status = status_type::not_found;
    }

    co_return data;
  }

  void on_ws_msg(std::function<void(resp_data)> on_ws_msg) {
    on_ws_msg_ = std::move(on_ws_msg);
  }
  void on_ws_close(std::function<void(std::string_view)> on_ws_close) {
    on_ws_close_ = std::move(on_ws_close);
  }

  async_simple::coro::Lazy<resp_data> async_get(std::string uri) {
    req_context<std::string> ctx{};
    return async_request(std::move(uri), http_method::GET, std::move(ctx));
  }

  resp_data get(std::string uri) {
    return async_simple::coro::syncAwait(async_get(std::move(uri)));
  }

  async_simple::coro::Lazy<resp_data> async_post(
      std::string uri, std::string content, req_content_type content_type) {
    req_context<std::string> ctx{content_type, "", std::move(content)};
    return async_request(std::move(uri), http_method::POST, std::move(ctx));
  }

  resp_data post(std::string uri, std::string content,
                 req_content_type content_type) {
    return async_simple::coro::syncAwait(
        async_post(std::move(uri), std::move(content), content_type));
  }

  async_simple::coro::Lazy<resp_data> async_redirect(std::string uri) {
    req_context<std::string> ctx{};
    resp_data data{};
    data = co_await async_request(std::move(uri), http_method::GET,
                                  std::move(ctx));

    if (!redirect_uri_.empty() && is_redirect_resp()) {
      data = co_await async_request(std::move(redirect_uri_), http_method::GET,
                                    std::move(ctx));
      co_return data;
    }
    co_return data;
  }

  resp_data redirect(std::string uri) {
    return async_simple::coro::syncAwait(async_redirect(std::move(uri)));
  }

  bool add_str_part(std::string name, std::string content) {
    return form_data_
        .emplace(std::move(name), multipart_t{"", std::move(content)})
        .second;
  }

  bool add_file_part(std::string name, std::string filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
      std::cout << "open file failed\n";
      return false;
    }

    if (form_data_.find(name) != form_data_.end()) {
      std::cout << "name already exist\n";
      return false;
    }

    std::string short_name =
        std::filesystem::path(filename).filename().string();

    size_t file_size = std::filesystem::file_size(filename);

    size_t size_to_read = 1024 * 1024;
    std::string file_data;
    file_data.resize(file_size);
    file.read(file_data.data(), size_to_read);
    form_data_.emplace(std::move(name), multipart_t{std::move(short_name),
                                                    std::move(file_data)});
    return true;
  }

  async_simple::coro::Lazy<resp_data> async_upload(std::string uri) {
    if (form_data_.empty()) {
      std::cout << "no multipart\n";
      co_return resp_data{{}, status_type::not_found};
    }

    std::string content = build_multipart_content();

    co_return co_await async_post(std::move(uri), std::move(content),
                                  req_content_type::multipart);
  }

  async_simple::coro::Lazy<resp_data> async_upload(std::string uri,
                                                   std::string name,
                                                   std::string filename) {
    add_file_part(std::move(name), std::move(filename));
    return async_upload(std::move(uri));
  }

  async_simple::coro::Lazy<resp_data> async_download(std::string uri,
                                                     std::string filename,
                                                     std::string range = "") {
    resp_data data{};
    std::ofstream file(filename, std::ios::binary | std::ios::app);
    if (!file) {
      data.net_err = std::make_error_code(std::errc::no_such_file_or_directory);
      data.status = status_type::not_found;
      co_return data;
    }

    req_context<std::ofstream> ctx{};
    if (range.empty()) {
      ctx = {req_content_type::none, "", "", std::move(file)};
    }
    else {
      std::string req_str = "Range: bytes=";
      req_str.append(range).append(CRCF);
      ctx = {req_content_type::none, std::move(req_str), {}, std::move(file)};
    }

    data = co_await async_request(std::move(uri), http_method::GET,
                                  std::move(ctx));

    co_return data;
  }

  resp_data download(std::string uri, std::string filename,
                     std::string range = "") {
    return async_simple::coro::syncAwait(
        async_download(std::move(uri), std::move(filename), std::move(range)));
  }

  async_simple::coro::Lazy<resp_data> async_request(std::string uri,
                                                    http_method method,
                                                    auto ctx) {
    resp_data data{};

    std::error_code ec{};
    size_t size = 0;
    http_parser parser;
    bool is_keep_alive = false;

    do {
      if (auto [ok, u] = handle_uri(data, uri); !ok) {
        break;
      }
      else {
        if (has_closed_) {
          std::string host = proxy_host_.empty() ? u.get_host() : proxy_host_;
          std::string port = proxy_port_.empty() ? u.get_port() : proxy_port_;
          if (ec = co_await asio_util::async_connect(io_ctx_, socket_, host,
                                                     port);
              ec) {
            break;
          }
          has_closed_ = false;
        }

        std::string write_msg = prepare_request_str(u, method, ctx);

        if (std::tie(ec, size) = co_await asio_util::async_write(
                socket_, asio::buffer(write_msg));
            ec) {
          break;
        }
      }

      if (std::tie(ec, size) = co_await asio_util::async_read_until(
              socket_, read_buf_, TWO_CRCF);
          ec) {
        break;
      }

      if (ec = handle_header(data, parser, size); ec) {
        break;
      }

      is_keep_alive = parser.keep_alive();
      bool is_ranges = parser.is_ranges();
      if (parser.is_chunked()) {
        is_keep_alive = true;
        ec = co_await handle_chunked(data, std::move(ctx));
        break;
      }

      bool is_redirect = parser.is_location();
      if (is_redirect)
        redirect_uri_ = parser.get_header_value("Location");

      size_t content_len = (size_t)parser.body_len();

      if ((size_t)parser.body_len() <= read_buf_.size()) {
        // Now get entire content, additional data will discard.
        handle_entire_content(data, content_len, is_ranges, ctx);
        break;
      }

      // read left part of content.
      size_t size_to_read = content_len - read_buf_.size();
      if (std::tie(ec, size) =
              co_await asio_util::async_read(socket_, read_buf_, size_to_read);
          ec) {
        break;
      }

      // Now get entire content, additional data will discard.
      handle_entire_content(data, content_len, is_ranges, ctx);
    } while (0);

    handle_result(data, ec, is_keep_alive);

    co_return data;
  }

  inline void set_proxy(const std::string &host, const std::string &port) {
    proxy_host_ = host;
    proxy_port_ = port;
  }

  inline void set_proxy_basic_auth(const std::string &username,
                                   const std::string &password) {
    proxy_basic_auth_username_ = username;
    proxy_basic_auth_password_ = password;
  }

  inline void set_proxy_bearer_token_auth(const std::string &token) {
    proxy_bearer_token_auth_token_ = token;
  }

 private:
  std::pair<bool, uri_t> handle_uri(resp_data &data, const std::string &uri) {
    uri_t u;
    if (!u.parse_from(uri.data())) {
      if (!u.schema.empty()) {
        auto new_uri = url_encode(uri);

        if (!u.parse_from(new_uri.data())) {
          data.net_err = std::make_error_code(std::errc::protocol_error);
          data.status = status_type::not_found;
          return {false, {}};
        }
      }
    }

    if (u.schema == "https"sv || u.schema == "wss"sv) {
#ifdef CINATRA_ENABLE_SSL
      // upgrade_to_ssl();
#else
      // please open CINATRA_ENABLE_SSL before request https!
      assert(false);
#endif
    }
    // construct proxy request uri
    construct_proxy_uri(u);

    return {true, u};
  }

  void construct_proxy_uri(uri_t &u) {
    if (!proxy_host_.empty() && !proxy_port_.empty()) {
      if (!proxy_request_uri_.empty())
        proxy_request_uri_.clear();
      if (u.get_port() == "http") {
        proxy_request_uri_ += "http://" + u.get_host() + ":";
        proxy_request_uri_ += "80";
      }
      else if (u.get_port() == "https") {
        proxy_request_uri_ += "https://" + u.get_host() + ":";
        proxy_request_uri_ += "443";
      }
      else {
        // all be http
        proxy_request_uri_ += " http://" + u.get_host() + ":";
        proxy_request_uri_ += u.get_port();
      }
      proxy_request_uri_ += u.get_path();
      u.path = std::string_view(proxy_request_uri_);
    }
  }

  std::string prepare_request_str(const uri_t &u, http_method method,
                                  const auto &ctx) {
    std::string req_str(method_name(method));

    req_str.append(" ").append(u.get_path());
    if (!u.query.empty()) {
      req_str.append("?").append(u.query);
    }

    req_str.append(" HTTP/1.1\r\nHost:").append(u.host).append("\r\n");
    auto type_str = get_content_type_str(ctx.content_type);
    if (!type_str.empty()) {
      if (ctx.content_type == req_content_type::multipart) {
        type_str.append(BOUNDARY);
      }
      req_headers_.emplace_back("Content-Type", std::move(type_str));
    }

    bool has_connection = false;
    // add user headers
    if (!req_headers_.empty()) {
      for (auto &pair : req_headers_) {
        if (pair.first == "Connection") {
          has_connection = true;
        }
        req_str.append(pair.first)
            .append(": ")
            .append(pair.second)
            .append("\r\n");
      }
    }

    if (!has_connection) {
      req_str.append("Connection: keep-alive\r\n");
    }

    if (!proxy_basic_auth_username_.empty() &&
        !proxy_basic_auth_password_.empty()) {
      std::string basic_auth_str = "Proxy-Authorization: Basic ";
      std::string basic_base64_str = base64_encode(
          proxy_basic_auth_username_ + ":" + proxy_basic_auth_password_);
      req_str.append(basic_auth_str).append(basic_base64_str).append(CRCF);
    }

    if (!proxy_bearer_token_auth_token_.empty()) {
      std::string bearer_token_str = "Proxy-Authorization: Bearer ";
      req_str.append(bearer_token_str)
          .append(proxy_bearer_token_auth_token_)
          .append(CRCF);
    }

    if (!ctx.req_str.empty())
      req_str.append(ctx.req_str);

    // add content
    size_t content_len = ctx.content.size();
    bool should_add = false;
    if (content_len > 0) {
      should_add = true;
    }
    else {
      if (method == http_method::POST)
        should_add = true;
    }

    if (should_add) {
      char buf[32];
      auto [ptr, ec] = std::to_chars(buf, buf + 32, content_len);
      req_str.append("Content-Length: ")
          .append(std::string_view(buf, ptr - buf))
          .append("\r\n");
    }

    req_str.append("\r\n");

    if (content_len > 0)
      req_str.append(std::move(ctx.content));

    return req_str;
  }

  std::error_code handle_header(resp_data &data, http_parser &parser,
                                size_t header_size) {
    // parse header
    const char *data_ptr = asio::buffer_cast<const char *>(read_buf_.data());

    char status[4] = {0};
    if (data_ptr != nullptr) {
      memcpy(status, data_ptr + 9, 3);
      response_code_.assign(status);
    }

    int parse_ret = parser.parse_response(data_ptr, header_size, 0);
    if (parse_ret < 0) {
      return std::make_error_code(std::errc::protocol_error);
    }
    read_buf_.consume(header_size);  // header size
    data.resp_headers = get_headers(parser);
    return {};
  }

  void handle_entire_content(resp_data &data, size_t content_len,
                             bool is_ranges, auto &ctx) {
    if (content_len > 0) {
      if (is_ranges) {
        auto data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
        if constexpr (std::is_same_v<
                          std::ofstream,
                          std::remove_cvref_t<decltype(ctx.stream)>>) {
          ctx.stream.write(data_ptr, content_len);
        }
        else {
          ctx.stream.append(data_ptr, content_len);
        }
      }
      else {
        assert(content_len == read_buf_.size());
        auto data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
        std::string_view reply(data_ptr, content_len);
        data.resp_body = reply;
      }
      read_buf_.consume(content_len);
    }
    data.eof = (read_buf_.size() == 0);

    data.status = status_type::ok;
  }

  void handle_result(resp_data &data, std::error_code ec, bool is_keep_alive) {
    if (ec) {
      close_socket();
      data.net_err = ec;
      data.status = status_type::not_found;
      std::cout << ec.message() << "\n";
    }
    else {
      if (!is_keep_alive) {
        close_socket();
      }
    }
  }

  template <typename Stream>
  async_simple::coro::Lazy<std::error_code> handle_chunked(
      resp_data &data, req_context<Stream> ctx) {
    std::error_code ec{};
    size_t size = 0;
    while (true) {
      if (std::tie(ec, size) =
              co_await asio_util::async_read_until(socket_, read_buf_, CRCF);
          ec) {
        break;
      }

      size_t buf_size = read_buf_.size();
      size_t additional_size = buf_size - size;
      const char *data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
      std::string_view size_str(data_ptr, size - CRCF.size());
      auto chunk_size = hex_to_int(size_str);
      read_buf_.consume(size);
      if (chunk_size < 0) {
        std::cout << "bad chunked size\n";
        ec = asio::error::make_error_code(
            asio::error::basic_errors::invalid_argument);
        break;
      }

      if (chunk_size == 0) {
        // all finished, no more data
        read_buf_.consume(CRCF.size());
        data.status = status_type::ok;
        data.eof = true;
        break;
      }

      if (additional_size < size_t(chunk_size + 2)) {
        // not a complete chunk, read left chunk data.
        size_t size_to_read = chunk_size + 2 - additional_size;
        if (std::tie(ec, size) = co_await asio_util::async_read(
                socket_, read_buf_, size_to_read);
            ec) {
          break;
        }
      }

      data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
      if constexpr (std::is_same_v<std::ofstream,
                                   std::remove_cvref_t<Stream>>) {
        ctx.stream.write(data_ptr, chunk_size);
      }

      read_buf_.consume(chunk_size + CRCF.size());
    }
    co_return ec;
  }

  std::string build_multipart_content() {
    std::string content;

    for (auto &[key, part] : form_data_) {
      std::string part_content;
      part_content.append("--").append(BOUNDARY).append(CRCF);
      part_content.append("Content-Disposition: form-data; name=\"");
      part_content.append(key).append("\"");
      if (!part.filename.empty()) {
        part_content.append("; filename=\"")
            .append(part.filename)
            .append("\"")
            .append(CRCF);
        auto ext = std::filesystem::path(part.filename).extension().string();
        if (auto it = g_content_type_map.find(ext);
            it != g_content_type_map.end()) {
          part_content.append("Content-Type: ").append(it->second);
        }
      }
      part_content.append(TWO_CRCF);

      part_content.append(part.content).append(CRCF);
      content.append(part_content);
    }
    content.append("--").append(BOUNDARY).append("--").append(CRCF);
    return content;
  }

  std::vector<std::pair<std::string, std::string>> get_headers(
      http_parser &parser) {
    std::vector<std::pair<std::string, std::string>> resp_headers;

    auto [headers, num_headers] = parser.get_headers();
    for (size_t i = 0; i < num_headers; i++) {
      resp_headers.emplace_back(
          std::string(headers[i].name, headers[i].name_len),
          std::string(headers[i].value, headers[i].value_len));
    }

    return resp_headers;
  }

  async_simple::coro::Lazy<void> async_read_ws() {
    resp_data data{};

    read_buf_.consume(read_buf_.size());
    size_t header_size = 2;

    websocket ws{};
    while (true) {
      if (auto [ec, _] =
              co_await asio_util::async_read(socket_, read_buf_, header_size);
          ec) {
        data.net_err = ec;
        data.status = status_type::not_found;
        if (on_ws_msg_)
          on_ws_msg_(data);
        co_return;
      }

      const char *data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
      auto ret = ws.parse_header(data_ptr, header_size, false);
      if (ret == -2) {
        header_size += ws.left_header_len();
        continue;
      }
      frame_header *header = (frame_header *)data_ptr;
      bool is_close_frame = header->opcode == opcode::close;

      read_buf_.consume(header_size);

      size_t payload_len = ws.payload_length();
      if (payload_len > read_buf_.size()) {
        size_t size_to_read = payload_len - read_buf_.size();
        if (auto [ec, size] = co_await asio_util::async_read(socket_, read_buf_,
                                                             size_to_read);
            ec) {
          data.net_err = ec;
          data.status = status_type::not_found;
          if (on_ws_msg_)
            on_ws_msg_(data);
          co_return;
        }
      }

      data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
      if (is_close_frame) {
        payload_len -= 2;
        if (payload_len > 0) {
          data_ptr += sizeof(uint16_t);
          std::string out;
          if (header->mask) {
            std::string out;
            ws.parse_payload(data_ptr, payload_len, out);
            data_ptr = out.data();
          }
        }
      }

      data.status = status_type::ok;
      data.resp_body = {data_ptr, payload_len};

      read_buf_.consume(read_buf_.size());
      header_size = 2;

      if (is_close_frame) {
        if (on_ws_close_)
          on_ws_close_(data.resp_body);
        co_await async_send_ws("close", false, opcode::close);
        close();
        co_return;
      }
      if (on_ws_msg_)
        on_ws_msg_(data);
    }
  }

  void close_socket() {
    std::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    has_closed_ = true;
  }

  template <typename E>
  constexpr auto to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
  }

  bool is_redirect_resp() {
    int resp_code = std::stoi(response_code_);
    if (resp_code == to_underlying(status_type::moved_temporarily) ||
        resp_code == to_underlying(status_type::moved_permanently) ||
        resp_code == to_underlying(status_type::not_modified) ||
        resp_code == to_underlying(status_type::multiple_choices) ||
        resp_code == to_underlying(status_type::temporary_redirect))
      return true;
    return false;
  }

  asio::io_context io_ctx_;
  asio::ip::tcp::socket socket_;
  std::unique_ptr<asio::io_context::work> work_;
  std::thread io_thd_;

  std::atomic<bool> has_closed_ = true;
  asio::streambuf read_buf_;

  std::vector<std::pair<std::string, std::string>> req_headers_;

  std::string proxy_request_uri_ = "";
  std::string proxy_host_;
  std::string proxy_port_;

  std::string proxy_basic_auth_username_;
  std::string proxy_basic_auth_password_;

  std::string proxy_bearer_token_auth_token_;

  std::map<std::string, multipart_t> form_data_;

  std::function<void(resp_data)> on_ws_msg_;
  std::function<void(std::string_view)> on_ws_close_;
  std::string ws_sec_key_;

  std::string redirect_uri_;

  std::string response_code_;
};
}  // namespace cinatra
