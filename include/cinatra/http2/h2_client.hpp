#pragma once

#include <async_simple/Promise.h>
#include <async_simple/coro/ConditionVariable.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/Mutex.h>

#include <array>
#include <asio/ip/tcp.hpp>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cinatra/ylt/coro_io/coro_io.hpp"
#include "frame.hpp"
#include "h2_connection.hpp"
#include "hpack.hpp"

namespace cinatra::http2 {

struct h2_client_request {
  std::string method = "GET";
  std::string path = "/";
  std::string scheme = "http";
  std::string authority;
  std::string protocol;
  std::string body;
  std::vector<header_field> headers;
  std::vector<header_field> trailers;

  void add_header(std::string name, std::string value) {
    headers.push_back({std::move(name), std::move(value)});
  }

  void add_trailer(std::string name, std::string value) {
    trailers.push_back({std::move(name), std::move(value)});
  }
};

struct h2_client_response {
  std::error_code net_err;
  int status_code = 0;
  std::string body;
  std::vector<header_field> headers;
  std::vector<header_field> trailers;
};

struct h2_pushed_response {
  h2_client_request request;
  h2_client_response response;
};

class coro_http2_client {
 public:
  static constexpr uint32_t MAX_FRAME_SIZE = 16384;
  static constexpr uint32_t DEFAULT_WINDOW_SIZE = 65535;
  static constexpr uint32_t MAX_WINDOW_SIZE = 0x7fffffff;
  static constexpr uint32_t MAX_ALLOWED_FRAME_SIZE = 16777215;

  explicit coro_http2_client(
      coro_io::ExecutorWrapper<>* executor = coro_io::get_global_executor())
      : executor_(executor), socket_(executor->get_asio_executor()) {}

  ~coro_http2_client() { close(); }

  coro_io::ExecutorWrapper<>& get_executor() { return *executor_; }

  void set_enable_push(bool enabled) { enable_push_ = enabled; }

  void set_use_h2c_upgrade(bool enabled) { use_h2c_upgrade_ = enabled; }

  void set_push_handler(std::function<void(h2_pushed_response)> handler) {
    push_handler_ = std::move(handler);
  }

#ifdef CINATRA_ENABLE_SSL
  bool init_ssl(int verify_mode = asio::ssl::verify_none,
                const std::string& base_path = {},
                const std::string& cert_file = {},
                const std::string& sni_hostname = {}) {
    if (has_init_ssl_)
      return true;

    try {
      ssl_ctx_ =
          std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
      ssl_ctx_->set_options(
          asio::ssl::context::default_workarounds |
          asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
          asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
      auto full_cert_file = std::filesystem::path(base_path).append(cert_file);
      if (std::filesystem::exists(full_cert_file)) {
        ssl_ctx_->load_verify_file(full_cert_file.string());
      }
      else if (!base_path.empty() || !cert_file.empty()) {
        return false;
      }
      else {
        ssl_ctx_->set_default_verify_paths();
      }

      ssl_ctx_->set_verify_mode(verify_mode);
      ssl_sni_hostname_ = sni_hostname;
      has_init_ssl_ = true;
    } catch (...) {
      return false;
    }
    return true;
  }
#endif

  async_simple::coro::Lazy<std::error_code> connect(
      std::string host, std::string port, std::string scheme = "http",
      std::string authority = "") {
    bool want_ssl = false;
    {
      std::scoped_lock lock(state_mutex_);
      if (connected_)
        co_return std::error_code{};
      host_ = std::move(host);
      port_ = std::move(port);
      scheme_ = std::move(scheme);
      authority_ = authority.empty() ? host_ : std::move(authority);
#ifdef CINATRA_ENABLE_SSL
      want_ssl = has_init_ssl_ || scheme_ == "https";
      if (want_ssl && scheme_ == "http")
        scheme_ = "https";
      use_ssl_ = false;
#else
      want_ssl = scheme_ == "https";
#endif
      going_away_ = false;
      connection_closed_ = false;
      next_stream_id_ = 1;
      connection_send_window_ = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
      connection_recv_window_ = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
      connection_recv_pending_ = 0;
      peer_initial_window_size_ = DEFAULT_WINDOW_SIZE;
      peer_max_frame_size_ = MAX_FRAME_SIZE;
      peer_max_header_list_size_ = std::numeric_limits<uint32_t>::max();
      peer_max_concurrent_streams_ = std::numeric_limits<uint32_t>::max();
      peer_enable_connect_protocol_ = false;
      peer_goaway_last_stream_id_ = std::numeric_limits<uint32_t>::max();
      peer_settings_ready_ = false;
      request_start_next_ticket_ = 0;
      request_start_turn_ = 0;
      last_promised_stream_id_ = 0;
      pending_continuation_ = {};
      active_stream_count_ = 0;
      connected_ = true;
    }

#ifndef CINATRA_ENABLE_SSL
    if (want_ssl) {
      std::scoped_lock lock(state_mutex_);
      connected_ = false;
      stream_limit_cv_.notifyAll();
      co_return std::make_error_code(std::errc::protocol_error);
    }
#else
    if (want_ssl && !has_init_ssl_ &&
        !init_ssl(asio::ssl::verify_none, {}, {}, host_)) {
      std::scoped_lock lock(state_mutex_);
      connected_ = false;
      stream_limit_cv_.notifyAll();
      co_return std::make_error_code(std::errc::invalid_argument);
    }
#endif

    if (auto ec =
            co_await coro_io::async_connect(executor_, socket_, host_, port_);
        ec) {
      std::scoped_lock lock(state_mutex_);
      connected_ = false;
      stream_limit_cv_.notifyAll();
      co_return ec;
    }

#ifdef CINATRA_ENABLE_SSL
    if (want_ssl) {
      if (!recreate_ssl_stream()) {
        close();
        co_return std::make_error_code(std::errc::not_a_stream);
      }
      auto ec = co_await coro_io::async_handshake(
          ssl_stream_, asio::ssl::stream_base::client);
      if (ec || !selected_alpn_is_h2(ssl_stream_->native_handle())) {
        close();
        co_return ec ? ec : std::make_error_code(std::errc::protocol_error);
      }
      use_ssl_ = true;
    }
    else {
      use_ssl_ = false;
    }
#endif

    auto cli_settings = client_settings_entries();
    if (use_h2c_upgrade_ && !want_ssl) {
      co_return std::error_code{};
    }
    if (!co_await write_bytes(CLIENT_PREFACE) ||
        !co_await write_frame(make_settings_frame(cli_settings))) {
      close();
      co_return std::make_error_code(std::errc::connection_aborted);
    }

    start_read_loop();
    co_return std::error_code{};
  }

  async_simple::coro::Lazy<h2_client_response> async_get(
      std::string path, std::vector<header_field> headers = {}) {
    h2_client_request req;
    req.method = "GET";
    req.path = std::move(path);
    req.headers = std::move(headers);
    co_return co_await async_request(std::move(req));
  }

  async_simple::coro::Lazy<h2_client_response> async_post(
      std::string path, std::string body,
      std::vector<header_field> headers = {}) {
    h2_client_request req;
    req.method = "POST";
    req.path = std::move(path);
    req.body = std::move(body);
    req.headers = std::move(headers);
    co_return co_await async_request(std::move(req));
  }

  async_simple::coro::Lazy<h2_client_response> async_request(
      h2_client_request req) {
    auto st = std::make_shared<stream_state>();
    uint32_t stream_id = 0;
    uint64_t request_ticket = request_start_next_ticket_.fetch_add(1);
    bool used_h2c_upgrade = false;
    if (use_h2c_upgrade_ && request_ticket == 0) {
      {
        std::scoped_lock lock(state_mutex_);
        if (!connected_ || connection_closed_ || going_away_) {
          h2_client_response resp;
          resp.net_err = std::make_error_code(std::errc::not_connected);
          co_return resp;
        }
        if (req.scheme.empty())
          req.scheme = scheme_;
        if (req.authority.empty())
          req.authority = authority_;

        stream_id = 1;
        next_stream_id_ = 3;
        st->req = req;
        st->stream_id = stream_id;
        st->send_window = static_cast<int32_t>(peer_initial_window_size_);
        st->recv_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
        streams_.emplace(stream_id, st);
        active_stream_count_.fetch_add(1);
        used_h2c_upgrade = true;
      }
    }
    while (true) {
      if (used_h2c_upgrade)
        break;
      {
        auto stream_limit_lock = co_await stream_limit_mutex_.coScopedLock();
        co_await stream_limit_cv_.wait(
            stream_limit_mutex_, [this, request_ticket] {
              if (!connected_.load() || connection_closed_.load() ||
                  going_away_.load()) {
                return true;
              }
              if (request_start_turn_.load() != request_ticket)
                return false;
              if (!peer_settings_ready_.load())
                return false;
              return active_stream_count_.load() <
                     peer_max_concurrent_streams_.load();
            });
        {
          std::scoped_lock lock(state_mutex_);
          if (!connected_ || connection_closed_ || going_away_) {
            h2_client_response resp;
            resp.net_err = std::make_error_code(std::errc::not_connected);
            co_return resp;
          }
          if (!peer_settings_ready_.load()) {
            continue;
          }
          if (request_start_turn_.load() != request_ticket) {
            continue;
          }
          if (active_stream_count_.load() >=
              peer_max_concurrent_streams_.load()) {
            continue;
          }
          if (req.scheme.empty())
            req.scheme = scheme_;
          if (req.authority.empty())
            req.authority = authority_;

          stream_id = next_stream_id_;
          next_stream_id_ += 2;
          st->req = req;
          st->stream_id = stream_id;
          st->send_window = static_cast<int32_t>(peer_initial_window_size_);
          st->recv_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
          streams_.emplace(stream_id, st);
          active_stream_count_.fetch_add(1);
          break;
        }
      }
    }

    auto cleanup = [this, stream_id]() -> async_simple::coro::Lazy<void> {
      bool erased = false;
      {
        std::scoped_lock lock(state_mutex_);
        erased = streams_.erase(stream_id) > 0;
      }
      if (erased) {
        auto stream_limit_lock = co_await stream_limit_mutex_.coScopedLock();
        active_stream_count_.fetch_sub(1);
        stream_limit_cv_.notifyAll();
      }
      co_return;
    };

    if (used_h2c_upgrade) {
      if (!co_await send_h2c_upgrade_request(stream_id, *st, req)) {
        if (!st->done_signaled.load()) {
          if (!st->resp.net_err) {
            st->resp.net_err =
                std::make_error_code(std::errc::connection_aborted);
          }
          finish_stream(st);
        }
        auto resp = co_await std::move(st->future);
        co_await cleanup();
        co_return resp;
      }

      auto resp = co_await std::move(st->future);
      co_await cleanup();
      co_return resp;
    }

    if (!co_await send_request(stream_id, *st, req)) {
      if (!st->done_signaled.load()) {
        if (!st->resp.net_err) {
          st->resp.net_err =
              std::make_error_code(std::errc::connection_aborted);
        }
        finish_stream(st);
      }
      auto resp = co_await std::move(st->future);
      co_await cleanup();
      co_return resp;
    }

    auto resp = co_await std::move(st->future);
    co_await cleanup();
    co_return resp;
  }

  void close() {
    bool was_connected = false;
    {
      std::scoped_lock lock(state_mutex_);
      was_connected = connected_;
      connected_ = false;
      going_away_ = true;
#ifdef CINATRA_ENABLE_SSL
      use_ssl_ = false;
#endif
    }
    connection_closed_ = true;
    send_window_cv_.notifyAll();
    stream_limit_cv_.notifyAll();

    if (was_connected || socket_.is_open()) {
      std::error_code ignored;
      socket_.cancel(ignored);
      socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      socket_.close(ignored);
    }

    std::unique_lock lock(read_loop_mutex_);
    read_loop_cv_.wait(lock, [this] {
      return !read_loop_running_;
    });
  }

 private:
  struct stream_state {
    stream_state() : future(promise.getFuture()) {}

    h2_client_request req;
    h2_client_response resp;
    std::vector<uint8_t> hdr_block_buf;
    uint32_t stream_id = 0;
    int32_t recv_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
    uint32_t recv_pending = 0;
    int32_t send_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
    std::atomic<bool> done_signaled = false;
    std::optional<uint64_t> content_length;
    bool final_response_received = false;
    bool trailing_headers_received = false;
    bool is_pushed = false;
    uint32_t associated_stream_id = 0;
    async_simple::Promise<h2_client_response> promise;
    async_simple::Future<h2_client_response> future;
  };

  struct payload_view_result {
    bool ok = false;
    std::span<const uint8_t> payload{};
  };

  enum class header_decode_result {
    ok,
    stream_error,
    connection_error,
  };

  struct continuation_state {
    uint32_t stream_id = 0;
    std::shared_ptr<stream_state> target;
    header_block_kind block_kind = header_block_kind::initial;
    bool is_push_promise = false;
  };

  void start_read_loop() {
    std::scoped_lock lock(read_loop_mutex_);
    if (read_loop_running_)
      return;
    read_loop_running_ = true;
    read_loop().via(executor_).start([this](auto&&) {
      {
        std::scoped_lock lock(read_loop_mutex_);
        read_loop_running_ = false;
      }
      read_loop_cv_.notify_all();
    });
  }

  async_simple::coro::Lazy<bool> write_bytes(std::string_view data) {
    auto lock = co_await write_mutex_.coScopedLock();
    auto [ec, n] = co_await write_to_peer(asio::buffer(data));
    co_return !ec && n == data.size();
  }

  std::array<settings_entry, 4> client_settings_entries() const {
    return {
        settings_entry{settings_param::header_table_size, 4096},
        settings_entry{settings_param::enable_push, enable_push_ ? 1u : 0u},
        settings_entry{settings_param::initial_window_size,
                       DEFAULT_WINDOW_SIZE},
        settings_entry{settings_param::max_frame_size, MAX_FRAME_SIZE},
    };
  }

  static std::string encode_http2_settings_header(
      std::span<const settings_entry> settings) {
    auto frame = make_settings_frame(settings);
    auto payload = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(frame.data()) + 9, frame.size() - 9);
    std::string raw(reinterpret_cast<const char*>(payload.data()),
                    payload.size());
    auto encoded = cinatra::base64_encode(raw);
    for (auto& ch : encoded) {
      if (ch == '+')
        ch = '-';
      else if (ch == '/')
        ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
  }

  async_simple::coro::Lazy<bool> read_h2c_upgrade_response(stream_state& st) {
    std::string response;
    response.reserve(256);
    std::array<char, 1> ch{};
    while (response.find("\r\n\r\n") == std::string::npos &&
           response.size() < 8192) {
      auto buf = asio::buffer(ch);
      auto [ec, n] = co_await read_from_peer(buf, 1);
      if (ec || n != 1) {
        st.resp.net_err =
            ec ? ec : std::make_error_code(std::errc::connection_aborted);
        co_return false;
      }
      response.push_back(ch[0]);
    }

    auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      co_return false;
    }

    auto status_end = response.find("\r\n");
    if (status_end == std::string::npos) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      co_return false;
    }
    auto status_line = std::string_view(response.data(), status_end);
    size_t sp1 = status_line.find(' ');
    size_t sp2 =
        status_line.find(' ', sp1 == std::string_view::npos ? 0 : sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
        status_line.substr(0, sp1) != "HTTP/1.1" ||
        status_line.substr(sp1 + 1, sp2 - sp1 - 1) != "101") {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      co_return false;
    }

    bool saw_connection_upgrade = false;
    bool saw_upgrade_h2c = false;
    size_t pos = status_end + 2;
    while (pos < header_end) {
      size_t next = response.find("\r\n", pos);
      if (next == std::string::npos || next > header_end)
        next = header_end;
      if (next == pos)
        break;

      auto line = std::string_view(response.data() + pos, next - pos);
      auto colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        st.resp.net_err = std::make_error_code(std::errc::protocol_error);
        co_return false;
      }

      auto name =
          ascii_lower_copy(trim_optional_whitespace(line.substr(0, colon)));
      auto value = trim_optional_whitespace(line.substr(colon + 1));
      if (name == "connection") {
        saw_connection_upgrade = token_list_contains(value, "upgrade");
      }
      else if (name == "upgrade") {
        saw_upgrade_h2c = ascii_iequals(value, "h2c");
      }
      pos = next + 2;
    }

    if (!saw_connection_upgrade || !saw_upgrade_h2c) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      co_return false;
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> send_h2c_upgrade_request(
      uint32_t stream_id, stream_state& st, const h2_client_request& req) {
    if (stream_id != 1 || req.scheme != "http" || req.method == "CONNECT" ||
        !req.protocol.empty() || !req.trailers.empty()) {
      st.resp.net_err = std::make_error_code(std::errc::invalid_argument);
      close();
      co_return false;
    }

    auto cli_settings = client_settings_entries();
    std::string request;
    request.reserve(512 + req.body.size());
    request.append(req.method)
        .append(" ")
        .append(req.path)
        .append(" HTTP/1.1\r\n");
    request.append("Host: ").append(req.authority).append("\r\n");
    request.append("Connection: Upgrade, HTTP2-Settings\r\n");
    request.append("Upgrade: h2c\r\n");
    request.append("HTTP2-Settings: ")
        .append(encode_http2_settings_header(cli_settings))
        .append("\r\n");

    bool saw_content_length = false;
    for (auto& hf : req.headers) {
      auto name = ascii_lower_copy(hf.name);
      if (name == "host" || name == "connection" || name == "upgrade" ||
          name == "http2-settings" || name == "transfer-encoding") {
        st.resp.net_err = std::make_error_code(std::errc::invalid_argument);
        close();
        co_return false;
      }
      if (name == "content-length")
        saw_content_length = true;
      request.append(hf.name).append(": ").append(hf.value).append("\r\n");
    }
    if (!req.body.empty() && !saw_content_length) {
      request.append("Content-Length: ")
          .append(integer_to_string(req.body.size()))
          .append("\r\n");
    }
    request.append("\r\n").append(req.body);

    if (!co_await write_bytes(request)) {
      st.resp.net_err = std::make_error_code(std::errc::connection_aborted);
      close();
      co_return false;
    }
    if (!co_await read_h2c_upgrade_response(st)) {
      close();
      co_return false;
    }
    if (!co_await write_bytes(CLIENT_PREFACE) ||
        !co_await write_frame(make_settings_frame(cli_settings))) {
      st.resp.net_err = std::make_error_code(std::errc::connection_aborted);
      close();
      co_return false;
    }

    co_await advance_request_start_turn();
    start_read_loop();
    co_return true;
  }

  async_simple::coro::Lazy<void> advance_request_start_turn() {
    auto stream_limit_lock = co_await stream_limit_mutex_.coScopedLock();
    request_start_turn_.fetch_add(1);
    stream_limit_cv_.notifyAll();
    co_return;
  }

  async_simple::coro::Lazy<bool> write_frame(std::string data) {
    auto lock = co_await write_mutex_.coScopedLock();
    auto [ec, n] = co_await write_to_peer(asio::buffer(data));
    co_return !ec && n == data.size();
  }

  async_simple::coro::Lazy<bool> write_frame_locked(std::string_view data) {
    auto [ec, n] = co_await write_to_peer(asio::buffer(data));
    co_return !ec && n == data.size();
  }

  async_simple::coro::Lazy<bool> write_data_frame(
      uint32_t stream_id, uint8_t flags, std::span<const uint8_t> data) {
    auto hdr =
        make_frame_header(frame_type::data, flags, stream_id, data.size());
    std::array<asio::const_buffer, 2> buffers{
        asio::buffer(hdr), asio::buffer(data.data(), data.size())};

    auto lock = co_await write_mutex_.coScopedLock();
    auto [ec, n] = co_await write_to_peer(buffers);
    co_return !ec && n == hdr.size() + data.size();
  }

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> read_from_peer(
      AsioBuffer& buffer, size_t size_to_read) {
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      co_return co_await coro_io::async_read(*ssl_stream_, buffer,
                                             size_to_read);
    }
#endif
    co_return co_await coro_io::async_read(socket_, buffer, size_to_read);
  }

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> write_to_peer(
      AsioBuffer&& buffer) {
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      co_return co_await coro_io::async_write(*ssl_stream_, buffer);
    }
#endif
    co_return co_await coro_io::async_write(socket_, buffer);
  }

  async_simple::coro::Lazy<bool> write_header_block(
      uint32_t stream_id, const stream_state& st,
      std::span<const header_field> headers, uint8_t first_frame_flags) {
    co_return co_await write_header_block_impl(stream_id, st, headers,
                                               first_frame_flags);
  }

  async_simple::coro::Lazy<bool> write_header_block(
      uint32_t stream_id, const stream_state& st,
      std::span<const header_field_view> headers, uint8_t first_frame_flags) {
    co_return co_await write_header_block_impl(stream_id, st, headers,
                                               first_frame_flags);
  }

  template <typename HeaderSpan>
  async_simple::coro::Lazy<bool> write_header_block_impl(
      uint32_t stream_id, const stream_state& st, HeaderSpan headers,
      uint8_t first_frame_flags) {
    auto header_lock = co_await header_mutex_.coScopedLock();
    auto write_lock = co_await write_mutex_.coScopedLock();

    if (!can_send_stream(stream_id, st))
      co_return false;
    auto header_block = encoder_.encode(headers);
    auto frames = make_header_block_frames(
        stream_id, std::span<const uint8_t>(header_block), first_frame_flags,
        std::min(peer_max_frame_size_, MAX_ALLOWED_FRAME_SIZE));
    for (auto& frame : frames) {
      if (!can_send_stream(stream_id, st))
        co_return false;
      if (!co_await write_frame_locked(frame))
        co_return false;
    }
    co_return true;
  }

  bool outbound_headers_within_peer_limit(
      std::span<const header_field> headers) const {
    return header_list_size_within_limit(headers, peer_max_header_list_size_);
  }

  bool outbound_headers_within_peer_limit(
      std::span<const header_field_view> headers) const {
    return header_list_size_within_limit(headers, peer_max_header_list_size_);
  }

  async_simple::coro::Lazy<bool> send_request(uint32_t stream_id,
                                              stream_state& st,
                                              const h2_client_request& req) {
    std::string content_length_value;
    std::vector<header_field_view> hdrs;
    hdrs.reserve(4 + req.headers.size() + 1);
    hdrs.push_back({":method", req.method});
    if (req.method == "CONNECT") {
      if (!req.protocol.empty()) {
        hdrs.push_back({":protocol", req.protocol});
        hdrs.push_back({":scheme", req.scheme});
        hdrs.push_back({":path", req.path});
      }
      hdrs.push_back({":authority", req.authority});
    }
    else {
      hdrs.push_back({":path", req.path});
      hdrs.push_back({":scheme", req.scheme});
      hdrs.push_back({":authority", req.authority});
    }
    for (auto& hf : req.headers) hdrs.push_back({hf.name, hf.value});
    if (!req.body.empty() && !has_header(hdrs, "content-length")) {
      content_length_value = integer_to_string(req.body.size());
      hdrs.push_back({"content-length", content_length_value});
    }

    uint8_t hdr_flags = flags::END_HEADERS;
    if (req.body.empty() && req.trailers.empty())
      hdr_flags |= flags::END_STREAM;
    {
      auto stream_start_lock = co_await stream_start_mutex_.coScopedLock();
      if (!outbound_headers_within_peer_limit(hdrs) ||
          !outbound_headers_within_peer_limit(req.trailers)) {
        st.resp.net_err = std::make_error_code(std::errc::message_size);
        co_await advance_request_start_turn();
        co_return false;
      }
      if (!can_send_stream(stream_id, st) ||
          !co_await write_header_block(stream_id, st, hdrs, hdr_flags)) {
        co_return false;
      }
      co_await advance_request_start_turn();
    }
    if (req.body.empty()) {
      if (!req.trailers.empty()) {
        if (!can_send_stream(stream_id, st) ||
            !co_await write_header_block(
                stream_id, st, req.trailers,
                flags::END_HEADERS | flags::END_STREAM)) {
          co_return false;
        }
      }
      co_return true;
    }

    size_t offset = 0;
    while (offset < req.body.size()) {
      if (!can_send_stream(stream_id, st))
        co_return false;
      auto chunk = co_await reserve_send_window(st, req.body.size() - offset);
      if (chunk == 0)
        co_return false;

      auto data = std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(req.body.data()) + offset, chunk);
      bool last = (offset + chunk) == req.body.size();
      if (!co_await write_data_frame(
              stream_id,
              last && req.trailers.empty() ? flags::END_STREAM : uint8_t(0),
              data)) {
        co_return false;
      }
      offset += chunk;
    }

    if (!req.trailers.empty()) {
      if (!can_send_stream(stream_id, st) ||
          !co_await write_header_block(
              stream_id, st, req.trailers,
              flags::END_HEADERS | flags::END_STREAM)) {
        co_return false;
      }
    }
    co_return true;
  }

  async_simple::coro::Lazy<size_t> reserve_send_window(stream_state& st,
                                                       size_t remaining) {
    auto lock = co_await send_window_mutex_.coScopedLock();
    co_await send_window_cv_.wait(send_window_mutex_, [&] {
      return connection_closed_.load() || st.done_signaled.load() ||
             (connection_send_window_ > 0 && st.send_window > 0);
    });
    if (connection_closed_.load() || st.done_signaled.load())
      co_return 0;

    auto chunk = std::min<size_t>(
        remaining,
        std::min<uint32_t>(
            std::min<uint32_t>(MAX_FRAME_SIZE, peer_max_frame_size_),
            std::min<uint32_t>(connection_send_window_, st.send_window)));
    connection_send_window_ -= static_cast<int32_t>(chunk);
    st.send_window -= static_cast<int32_t>(chunk);
    co_return chunk;
  }

  async_simple::coro::Lazy<void> read_loop() {
    std::error_code end_ec;
    std::array<uint8_t, 9> hdr_buf;
    while (true) {
      auto hdr_buf_view = asio::buffer(hdr_buf);
      auto [ec, n] = co_await read_from_peer(hdr_buf_view, 9);
      if (ec || n != 9) {
        if (ec == asio::error::eof) {
          end_ec = std::make_error_code(std::errc::connection_aborted);
        }
        else {
          end_ec = ec ? ec : std::make_error_code(std::errc::connection_reset);
        }
        break;
      }

      auto hdr = parse_frame_header(hdr_buf);
      if (!peer_settings_ready_.load()) {
        if (hdr.type != frame_type::settings || hdr.stream_id != 0 ||
            (hdr.flags & flags::ACK)) {
          end_ec = std::make_error_code(std::errc::protocol_error);
          break;
        }
      }
      if (hdr.length > MAX_FRAME_SIZE) {
        end_ec = std::make_error_code(std::errc::message_size);
        break;
      }

      payload_buf_.resize(hdr.length);
      if (hdr.length > 0) {
        auto payload_buf_view = asio::buffer(payload_buf_);
        auto [read_ec, read_n] =
            co_await read_from_peer(payload_buf_view, hdr.length);
        if (read_ec || read_n != hdr.length) {
          end_ec = read_ec ? read_ec
                           : std::make_error_code(std::errc::connection_reset);
          break;
        }
      }

      if (!co_await handle_frame(
              hdr, std::span<const uint8_t>(payload_buf_.data(), hdr.length))) {
        end_ec = pending_connection_error_.value_or(
            std::make_error_code(std::errc::connection_aborted));
        pending_connection_error_.reset();
        break;
      }
    }

    fail_all_streams(end_ec);
    {
      std::scoped_lock lock(state_mutex_);
      connected_ = false;
    }
    {
      auto stream_limit_lock = co_await stream_limit_mutex_.coScopedLock();
      connection_closed_ = true;
      stream_limit_cv_.notifyAll();
    }
    send_window_cv_.notifyAll();
    co_return;
  }

  async_simple::coro::Lazy<bool> handle_frame(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (pending_continuation_.stream_id != 0) {
      if (hdr.type != frame_type::continuation ||
          hdr.stream_id != pending_continuation_.stream_id) {
        pending_connection_error_ =
            std::make_error_code(std::errc::protocol_error);
        co_return false;
      }
    }

    switch (hdr.type) {
      case frame_type::settings:
        co_return co_await on_settings(hdr, payload);
      case frame_type::headers:
        co_return co_await on_headers(hdr, payload);
      case frame_type::continuation:
        co_return co_await on_continuation(hdr, payload);
      case frame_type::data:
        co_return co_await on_data(hdr, payload);
      case frame_type::priority:
        co_return co_await on_priority(hdr, payload);
      case frame_type::ping:
        co_return co_await on_ping(hdr, payload);
      case frame_type::push_promise:
        co_return co_await on_push_promise(hdr, payload);
      case frame_type::rst_stream:
        co_return co_await on_rst_stream(hdr, payload);
      case frame_type::goaway:
        co_return co_await on_goaway(hdr, payload);
      case frame_type::window_update:
        co_return co_await on_window_update(hdr, payload);
      default:
        co_return true;
    }
  }

  async_simple::coro::Lazy<bool> on_settings(const frame_header& hdr,
                                             std::span<const uint8_t> payload) {
    if (hdr.stream_id != 0)
      co_return fail_connection(
          std::make_error_code(std::errc::protocol_error));
    if (hdr.flags & flags::ACK) {
      if (hdr.length != 0) {
        co_return fail_connection(
            std::make_error_code(std::errc::message_size));
      }
      co_return true;
    }
    if ((hdr.length % 6) != 0)
      co_return fail_connection(std::make_error_code(std::errc::message_size));

    uint32_t peer_initial = peer_initial_window_size_;
    uint32_t peer_frame = peer_max_frame_size_;
    uint32_t peer_concurrent = peer_max_concurrent_streams_.load();
    bool notify_stream_limit = false;
    for (auto setting : parse_settings_payload(payload)) {
      switch (setting.id) {
        case settings_param::header_table_size:
          encoder_.set_max_dynamic_table_size(setting.value);
          break;
        case settings_param::enable_push:
          // RFC 7540 §6.5.2: server MUST NOT set ENABLE_PUSH to anything
          // other than 0. Receipt of any other value from the server is
          // a connection error.
          if (setting.value != 0)
            co_return fail_connection(
                std::make_error_code(std::errc::protocol_error));
          break;
        case settings_param::max_concurrent_streams:
          peer_concurrent = setting.value;
          notify_stream_limit = true;
          break;
        case settings_param::initial_window_size:
          if (setting.value > MAX_WINDOW_SIZE)
            co_return fail_connection(
                std::make_error_code(std::errc::no_buffer_space));
          peer_initial = setting.value;
          break;
        case settings_param::max_frame_size:
          if (setting.value < MAX_FRAME_SIZE ||
              setting.value > MAX_ALLOWED_FRAME_SIZE) {
            co_return fail_connection(
                std::make_error_code(std::errc::protocol_error));
          }
          peer_frame = setting.value;
          break;
        case settings_param::max_header_list_size:
          peer_max_header_list_size_ = setting.value;
          break;
        case settings_param::enable_connect_protocol:
          if (setting.value > 1)
            co_return fail_connection(
                std::make_error_code(std::errc::protocol_error));
          peer_enable_connect_protocol_ = setting.value == 1;
          break;
      }
    }

    if (peer_initial != peer_initial_window_size_) {
      int64_t delta = static_cast<int64_t>(peer_initial) -
                      static_cast<int64_t>(peer_initial_window_size_);
      std::vector<std::shared_ptr<stream_state>> streams;
      {
        std::scoped_lock lock(state_mutex_);
        for (auto& [id, st] : streams_) streams.push_back(st);
      }
      auto lock = co_await send_window_mutex_.coScopedLock();
      for (auto& st : streams) {
        int64_t next = static_cast<int64_t>(st->send_window) + delta;
        if (next < 0 || next > MAX_WINDOW_SIZE)
          co_return fail_connection(
              std::make_error_code(std::errc::no_buffer_space));
        st->send_window = static_cast<int32_t>(next);
      }
      peer_initial_window_size_ = peer_initial;
      send_window_cv_.notifyAll();
    }

    peer_max_frame_size_ = peer_frame;
    if (!co_await write_frame(make_settings_frame({}, true))) {
      co_return false;
    }
    bool first_peer_settings = false;
    {
      auto stream_limit_lock = co_await stream_limit_mutex_.coScopedLock();
      peer_max_concurrent_streams_ = peer_concurrent;
      first_peer_settings = !peer_settings_ready_.exchange(true);
      if (first_peer_settings || notify_stream_limit)
        stream_limit_cv_.notifyAll();
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_window_update(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (hdr.length != 4)
      co_return fail_connection(std::make_error_code(std::errc::message_size));

    uint32_t increment = ((uint32_t(payload[0]) & 0x7f) << 24) |
                         (uint32_t(payload[1]) << 16) |
                         (uint32_t(payload[2]) << 8) | uint32_t(payload[3]);
    if (increment == 0) {
      // RFC 7540 §6.9: increment 0 is connection error when stream_id == 0,
      // stream error (RST_STREAM) otherwise.
      if (hdr.stream_id == 0)
        co_return fail_connection(
            std::make_error_code(std::errc::protocol_error));
      auto rst_st = find_stream(hdr.stream_id);
      if (rst_st) {
        rst_st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        finish_stream(rst_st);
      }
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
    }

    std::shared_ptr<stream_state> st;
    if (hdr.stream_id != 0) {
      std::scoped_lock lock(state_mutex_);
      if (auto it = streams_.find(hdr.stream_id); it != streams_.end())
        st = it->second;
    }

    auto lock = co_await send_window_mutex_.coScopedLock();
    if (hdr.stream_id == 0) {
      if (increment >
          static_cast<uint32_t>(MAX_WINDOW_SIZE - connection_send_window_)) {
        co_return fail_connection(
            std::make_error_code(std::errc::no_buffer_space));
      }
      connection_send_window_ += static_cast<int32_t>(increment);
    }
    else if (st) {
      if (increment >
          static_cast<uint32_t>(MAX_WINDOW_SIZE - st->send_window)) {
        co_return fail_connection(
            std::make_error_code(std::errc::no_buffer_space));
      }
      st->send_window += static_cast<int32_t>(increment);
    }
    send_window_cv_.notifyAll();
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_priority(const frame_header& hdr,
                                             std::span<const uint8_t> payload) {
    auto spec = parse_priority_payload(payload);
    if (hdr.stream_id == 0 || !spec.has_value() ||
        spec->stream_dependency == hdr.stream_id) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_push_promise(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (!enable_push_ || hdr.stream_id == 0) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }

    auto parent = find_stream(hdr.stream_id);
    if (!parent) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }

    auto parsed = strip_push_promise_payload(hdr, payload);
    if (!parsed.ok || parsed.payload.size() < 4) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }

    uint32_t promised_stream_id = ((uint32_t(parsed.payload[0]) & 0x7f) << 24) |
                                  (uint32_t(parsed.payload[1]) << 16) |
                                  (uint32_t(parsed.payload[2]) << 8) |
                                  uint32_t(parsed.payload[3]);
    if (promised_stream_id == 0 || (promised_stream_id & 1u) != 0 ||
        promised_stream_id <= last_promised_stream_id_) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }
    last_promised_stream_id_ = promised_stream_id;

    auto st = std::make_shared<stream_state>();
    st->is_pushed = true;
    st->stream_id = promised_stream_id;
    st->associated_stream_id = hdr.stream_id;
    st->send_window = static_cast<int32_t>(peer_initial_window_size_);
    st->recv_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
    st->hdr_block_buf.insert(st->hdr_block_buf.end(),
                             parsed.payload.begin() + 4, parsed.payload.end());
    {
      std::scoped_lock lock(state_mutex_);
      streams_.emplace(promised_stream_id, st);
    }

    if (hdr.flags & flags::END_HEADERS) {
      auto decode_result = decode_push_request_headers(*st);
      if (decode_result != header_decode_result::ok) {
        fail_all_streams(std::make_error_code(std::errc::protocol_error));
        co_return decode_result != header_decode_result::connection_error;
      }
    }
    else {
      pending_continuation_.stream_id = hdr.stream_id;
      pending_continuation_.target = st;
      pending_continuation_.block_kind = header_block_kind::initial;
      pending_continuation_.is_push_promise = true;
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_headers(const frame_header& hdr,
                                            std::span<const uint8_t> payload) {
    if (hdr.stream_id == 0) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }
    auto st = find_stream(hdr.stream_id);
    if (!st)
      co_return true;

    auto block_kind = st->final_response_received ? header_block_kind::trailer
                                                  : header_block_kind::initial;
    if (block_kind == header_block_kind::trailer &&
        (st->trailing_headers_received || !(hdr.flags & flags::END_STREAM))) {
      st->resp.net_err = std::make_error_code(std::errc::protocol_error);
      finish_stream(st);
      co_return true;
    }

    auto frag = strip_padding_priority(hdr, payload);
    if (!frag.ok) {
      st->resp.net_err = std::make_error_code(std::errc::protocol_error);
      finish_stream(st);
      co_return true;
    }
    st->hdr_block_buf.insert(st->hdr_block_buf.end(), frag.payload.begin(),
                             frag.payload.end());
    if (hdr.flags & flags::END_HEADERS) {
      pending_continuation_ = {};
      auto decode_result = decode_headers(*st, block_kind);
      if (decode_result != header_decode_result::ok) {
        finish_stream(st);
        co_return decode_result == header_decode_result::connection_error
            ? false
            : true;
      }
      if (block_kind == header_block_kind::trailer) {
        st->trailing_headers_received = true;
        if (st->content_length.has_value() &&
            *st->content_length != st->resp.body.size()) {
          st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        }
        finish_stream(st);
        co_return true;
      }

      bool informational =
          st->resp.status_code >= 100 && st->resp.status_code < 200;
      if (informational) {
        if (hdr.flags & flags::END_STREAM) {
          st->resp.net_err = std::make_error_code(std::errc::protocol_error);
          finish_stream(st);
          co_return true;
        }
        co_return true;
      }

      st->final_response_received = true;
      if ((hdr.flags & flags::END_STREAM) && st->content_length.has_value() &&
          *st->content_length != st->resp.body.size()) {
        st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        finish_stream(st);
        co_return true;
      }
      if (hdr.flags & flags::END_STREAM)
        finish_stream(st);
    }
    else {
      pending_continuation_.stream_id = hdr.stream_id;
      pending_continuation_.target = st;
      pending_continuation_.block_kind = block_kind;
      pending_continuation_.is_push_promise = false;
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_continuation(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    auto st = pending_continuation_.target;
    if (!st || hdr.stream_id != pending_continuation_.stream_id) {
      co_return fail_connection(
          std::make_error_code(std::errc::protocol_error));
    }
    st->hdr_block_buf.insert(st->hdr_block_buf.end(), payload.begin(),
                             payload.end());
    if (hdr.flags & flags::END_HEADERS) {
      auto block_kind = pending_continuation_.block_kind;
      bool is_push_promise = pending_continuation_.is_push_promise;
      pending_continuation_ = {};
      auto decode_result = is_push_promise ? decode_push_request_headers(*st)
                                           : decode_headers(*st, block_kind);
      if (decode_result != header_decode_result::ok) {
        if (is_push_promise) {
          fail_all_streams(std::make_error_code(std::errc::protocol_error));
          co_return decode_result != header_decode_result::connection_error;
        }
        finish_stream(st);
        co_return decode_result == header_decode_result::connection_error
            ? false
            : true;
      }
      if (is_push_promise)
        co_return true;
      if (block_kind == header_block_kind::trailer) {
        st->trailing_headers_received = true;
        if (st->content_length.has_value() &&
            *st->content_length != st->resp.body.size()) {
          st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        }
        finish_stream(st);
        co_return true;
      }

      bool informational =
          st->resp.status_code >= 100 && st->resp.status_code < 200;
      if (informational) {
        if (hdr.flags & flags::END_STREAM) {
          st->resp.net_err = std::make_error_code(std::errc::protocol_error);
          finish_stream(st);
          co_return true;
        }
        co_return true;
      }

      st->final_response_received = true;
      if ((hdr.flags & flags::END_STREAM) && st->content_length.has_value() &&
          *st->content_length != st->resp.body.size()) {
        st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        finish_stream(st);
        co_return true;
      }
      if (hdr.flags & flags::END_STREAM)
        finish_stream(st);
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_data(const frame_header& hdr,
                                         std::span<const uint8_t> payload) {
    if (hdr.stream_id == 0) {
      fail_all_streams(std::make_error_code(std::errc::protocol_error));
      co_return false;
    }
    auto st = find_stream(hdr.stream_id);
    if (!st)
      co_return true;

    if (!st->final_response_received) {
      if (hdr.length != 0 || (hdr.flags & flags::END_STREAM)) {
        st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        finish_stream(st);
        co_return true;
      }
      co_return true;
    }

    if (hdr.length > static_cast<uint32_t>(connection_recv_window_) ||
        hdr.length > static_cast<uint32_t>(st->recv_window)) {
      st->resp.net_err = std::make_error_code(std::errc::no_buffer_space);
      finish_stream(st);
      co_return false;
    }

    payload_view_result data{.ok = true, .payload = payload};
    if (hdr.flags & flags::PADDED) {
      data = strip_padded_payload(payload);
      if (!data.ok) {
        st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        finish_stream(st);
        co_return true;
      }
    }

    st->resp.body.append(reinterpret_cast<const char*>(data.payload.data()),
                         data.payload.size());

    // Flow control: decrement, accumulate pending, flush WINDOW_UPDATE when
    // half of the initial window has been consumed. Zero-length DATA does
    // not trigger WINDOW_UPDATE (increment 0 is a protocol error).
    bool stream_done = (hdr.flags & flags::END_STREAM) != 0;
    if (hdr.length > 0) {
      connection_recv_window_ -= static_cast<int32_t>(hdr.length);
      st->recv_window -= static_cast<int32_t>(hdr.length);
      connection_recv_pending_ += hdr.length;

      uint32_t threshold = DEFAULT_WINDOW_SIZE / 2;
      if (connection_recv_pending_ >= threshold) {
        uint32_t inc = connection_recv_pending_;
        connection_recv_pending_ = 0;
        connection_recv_window_ += static_cast<int32_t>(inc);
        if (!co_await write_frame(make_window_update(0, inc)))
          co_return false;
      }
      if (stream_done) {
        // Peer won't send more DATA for this stream; no WINDOW_UPDATE needed.
        st->recv_window += static_cast<int32_t>(hdr.length);
      }
      else {
        st->recv_pending += hdr.length;
        if (st->recv_pending >= threshold) {
          uint32_t inc = st->recv_pending;
          st->recv_pending = 0;
          st->recv_window += static_cast<int32_t>(inc);
          if (!co_await write_frame(make_window_update(hdr.stream_id, inc)))
            co_return false;
        }
      }
    }

    if (hdr.flags & flags::END_STREAM) {
      if (st->content_length.has_value() &&
          *st->content_length != st->resp.body.size()) {
        st->resp.net_err = std::make_error_code(std::errc::protocol_error);
        finish_stream(st);
        co_return true;
      }
      finish_stream(st);
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_ping(const frame_header& hdr,
                                         std::span<const uint8_t> payload) {
    if (hdr.stream_id != 0)
      co_return fail_connection(
          std::make_error_code(std::errc::protocol_error));
    if (payload.size() != 8)
      co_return fail_connection(std::make_error_code(std::errc::message_size));
    if (hdr.flags & flags::ACK)
      co_return true;

    std::array<uint8_t, 8> data;
    std::copy_n(payload.begin(), 8, data.begin());
    co_return co_await write_frame(make_ping_ack(data));
  }

  async_simple::coro::Lazy<bool> on_rst_stream(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (hdr.stream_id == 0)
      co_return fail_connection(
          std::make_error_code(std::errc::protocol_error));
    if (payload.size() != 4)
      co_return fail_connection(std::make_error_code(std::errc::message_size));
    auto st = find_stream(hdr.stream_id);
    if (!st)
      co_return true;

    st->resp.net_err = std::make_error_code(std::errc::operation_canceled);
    finish_stream(st);
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_goaway(const frame_header& hdr,
                                           std::span<const uint8_t> payload) {
    if (hdr.stream_id != 0)
      co_return fail_connection(
          std::make_error_code(std::errc::protocol_error));
    if (payload.size() < 8)
      co_return fail_connection(std::make_error_code(std::errc::message_size));
    uint32_t last_stream_id =
        ((uint32_t(payload[0]) & 0x7f) << 24) | (uint32_t(payload[1]) << 16) |
        (uint32_t(payload[2]) << 8) | uint32_t(payload[3]);
    auto stream_start_lock = co_await stream_start_mutex_.coScopedLock();
    {
      auto stream_limit_lock = co_await stream_limit_mutex_.coScopedLock();
      going_away_ = true;
      stream_limit_cv_.notifyAll();
    }
    if (last_stream_id < peer_goaway_last_stream_id_.load())
      peer_goaway_last_stream_id_ = last_stream_id;
    fail_streams_after_goaway(
        peer_goaway_last_stream_id_.load(),
        std::make_error_code(std::errc::connection_aborted));
    co_return true;
  }

  header_decode_result decode_headers(stream_state& st,
                                      header_block_kind block_kind) {
    try {
      auto decoded = decoder_.decode(st.hdr_block_buf);
      st.hdr_block_buf.clear();
      return validate_response_headers(decoded, st, block_kind)
                 ? header_decode_result::ok
                 : header_decode_result::stream_error;
    } catch (...) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      return header_decode_result::connection_error;
    }
  }

  bool validate_response_headers(std::span<const header_field> decoded,
                                 stream_state& st,
                                 header_block_kind block_kind) {
    if (block_kind == header_block_kind::trailer) {
      for (auto& hf : decoded) {
        if (contains_invalid_header_name(hf.name) ||
            contains_invalid_header_value(hf.value)) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
        if (!hf.name.empty() && hf.name.front() == ':') {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
        if (!validate_response_regular_header(hf.name, hf.value, block_kind)) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
        st.resp.trailers.push_back(hf);
      }
      return true;
    }

    bool seen_regular = false;
    bool seen_status = false;

    st.resp.status_code = 0;
    st.resp.headers.clear();
    st.resp.trailers.clear();
    st.content_length.reset();
    st.final_response_received = false;
    st.trailing_headers_received = false;

    for (auto& hf : decoded) {
      if (contains_invalid_header_name(hf.name) ||
          contains_invalid_header_value(hf.value)) {
        st.resp.net_err = std::make_error_code(std::errc::protocol_error);
        return false;
      }
      bool pseudo = !hf.name.empty() && hf.name.front() == ':';
      if (pseudo) {
        if (seen_regular || hf.name != ":status" || seen_status) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        if (hf.value.size() != 3) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        int status = 0;
        auto [ptr, err] = std::from_chars(
            hf.value.data(), hf.value.data() + hf.value.size(), status);
        if (err != std::errc{} || ptr != hf.value.data() + hf.value.size()) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        if (status == 101) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        seen_status = true;
        st.resp.status_code = status;
        continue;
      }

      seen_regular = true;
      if (!validate_response_regular_header(hf.name, hf.value, block_kind)) {
        st.resp.net_err = std::make_error_code(std::errc::protocol_error);
        return false;
      }

      if (hf.name == "content-length") {
        if (st.content_length.has_value()) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        auto len = parse_content_length(hf.value);
        if (!len.has_value()) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        if (st.resp.status_code >= 100 && st.resp.status_code < 200) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }

        if (st.resp.status_code == 204 && *len != 0) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
        st.content_length = *len;
      }

      st.resp.headers.push_back(hf);
    }

    if (!seen_status) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      return false;
    }

    return true;
  }

  void finish_stream(const std::shared_ptr<stream_state>& st) {
    if (!st || st->done_signaled.exchange(true))
      return;
    send_window_cv_.notifyAll();
    if (st->is_pushed) {
      h2_pushed_response pushed{
          .request = st->req,
          .response = std::move(st->resp),
      };
      {
        std::scoped_lock lock(state_mutex_);
        streams_.erase(st->stream_id);
      }
      if (push_handler_)
        push_handler_(std::move(pushed));
      return;
    }
    st->promise.setValue(std::move(st->resp));
  }

  void fail_all_streams(std::error_code ec) {
    std::vector<std::shared_ptr<stream_state>> streams;
    {
      std::scoped_lock lock(state_mutex_);
      for (auto& [id, st] : streams_) streams.push_back(st);
    }

    for (auto& st : streams) {
      st->resp.net_err = ec;
      finish_stream(st);
    }
  }

  void fail_streams_after_goaway(uint32_t last_stream_id, std::error_code ec) {
    std::vector<std::shared_ptr<stream_state>> streams;
    {
      std::scoped_lock lock(state_mutex_);
      for (auto& [id, st] : streams_) {
        if (id > last_stream_id)
          streams.push_back(st);
      }
    }

    for (auto& st : streams) {
      st->resp.net_err = ec;
      finish_stream(st);
    }
  }

  bool fail_connection(std::error_code ec) {
    pending_connection_error_ = ec;
    return false;
  }

  bool can_send_stream(uint32_t stream_id, const stream_state& st) const {
    if (connection_closed_.load() || st.done_signaled.load())
      return false;
    return !going_away_.load() ||
           stream_id <= peer_goaway_last_stream_id_.load();
  }

  std::shared_ptr<stream_state> find_stream(uint32_t stream_id) {
    std::scoped_lock lock(state_mutex_);
    if (auto it = streams_.find(stream_id); it != streams_.end())
      return it->second;
    return {};
  }

  template <typename Headers>
  static bool has_header(const Headers& headers, std::string_view name) {
    for (auto& hf : headers) {
      if (hf.name == name)
        return true;
    }
    return false;
  }

  template <typename Integer>
  static std::string integer_to_string(Integer value) {
    std::array<char, 32> buf{};
    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec != std::errc{})
      return {};
    return {buf.data(), ptr};
  }

  static std::optional<uint64_t> parse_content_length(std::string_view value) {
    if (value.empty())
      return std::nullopt;
    uint64_t result = 0;
    auto* begin = value.data();
    auto* end = value.data() + value.size();
    auto [ptr, err] = std::from_chars(begin, end, result);
    if (err != std::errc{} || ptr != end)
      return std::nullopt;
    return result;
  }

  static payload_view_result strip_padded_payload(
      std::span<const uint8_t> payload) {
    uint8_t pad_len = 0;
    if (!payload.empty()) {
      pad_len = payload[0];
      payload = payload.subspan(1);
    }
    if (pad_len > payload.size())
      return {};
    if (pad_len > 0)
      payload = payload.subspan(0, payload.size() - pad_len);
    return {.ok = true, .payload = payload};
  }

  static payload_view_result strip_padding_priority(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (hdr.flags & flags::PADDED) {
      if (payload.empty())
        return {};
      auto stripped = strip_padded_payload(payload);
      if (!stripped.ok)
        return stripped;
      payload = stripped.payload;
    }
    if ((hdr.flags & flags::PRIORITY) && payload.size() < 5)
      return {};
    if (hdr.flags & flags::PRIORITY) {
      auto spec = parse_priority_payload(payload.first<5>());
      if (!spec.has_value() || spec->stream_dependency == hdr.stream_id)
        return {};
      payload = payload.subspan(5);
    }
    return {.ok = true, .payload = payload};
  }

  static payload_view_result strip_push_promise_payload(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (hdr.flags & flags::PADDED) {
      if (payload.empty())
        return {};
      auto stripped = strip_padded_payload(payload);
      if (!stripped.ok)
        return stripped;
      payload = stripped.payload;
    }
    if (payload.size() < 4)
      return {};
    return {.ok = true, .payload = payload};
  }

  header_decode_result decode_push_request_headers(stream_state& st) {
    try {
      auto decoded = decoder_.decode(st.hdr_block_buf);
      st.hdr_block_buf.clear();
      return validate_push_request_headers(decoded, st)
                 ? header_decode_result::ok
                 : header_decode_result::stream_error;
    } catch (...) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      return header_decode_result::connection_error;
    }
  }

  bool validate_push_request_headers(std::span<const header_field> decoded,
                                     stream_state& st) {
    bool seen_regular = false;
    bool seen_method = false;
    bool seen_path = false;
    bool seen_scheme = false;
    bool seen_authority = false;
    st.req = {};

    for (auto& hf : decoded) {
      if (contains_invalid_header_name(hf.name) ||
          contains_invalid_header_value(hf.value)) {
        st.resp.net_err = std::make_error_code(std::errc::protocol_error);
        return false;
      }
      bool pseudo = !hf.name.empty() && hf.name.front() == ':';
      if (pseudo) {
        if (seen_regular) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
        if (hf.name == ":method") {
          if (seen_method)
            return false;
          seen_method = true;
          st.req.method = hf.value;
        }
        else if (hf.name == ":path") {
          if (seen_path)
            return false;
          seen_path = true;
          st.req.path = hf.value;
        }
        else if (hf.name == ":scheme") {
          if (seen_scheme)
            return false;
          seen_scheme = true;
          st.req.scheme = hf.value;
        }
        else if (hf.name == ":authority") {
          if (seen_authority)
            return false;
          seen_authority = true;
          st.req.authority = hf.value;
        }
        else {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
        continue;
      }

      seen_regular = true;
      if (!validate_request_regular_header(hf.name, hf.value,
                                           header_block_kind::initial)) {
        st.resp.net_err = std::make_error_code(std::errc::protocol_error);
        return false;
      }
      if (hf.name == "content-length") {
        auto len = parse_content_length(hf.value);
        if (!len.has_value() || *len != 0) {
          st.resp.net_err = std::make_error_code(std::errc::protocol_error);
          return false;
        }
      }
      st.req.headers.push_back(hf);
    }

    if (!seen_method || !seen_path || !seen_scheme || !seen_authority) {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      return false;
    }
    if (st.req.method == "CONNECT") {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      return false;
    }
    if (st.req.path == "*" && st.req.method != "OPTIONS") {
      st.resp.net_err = std::make_error_code(std::errc::protocol_error);
      return false;
    }
    return true;
  }

#ifdef CINATRA_ENABLE_SSL
  bool recreate_ssl_stream() {
    if (!ssl_ctx_)
      return false;
    ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(
        socket_, *ssl_ctx_);
    static constexpr unsigned char ALPN_H2_HTTP1[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_set_alpn_protos(ssl_stream_->native_handle(), ALPN_H2_HTTP1,
                            static_cast<unsigned int>(sizeof(ALPN_H2_HTTP1))) !=
        0) {
      ssl_stream_.reset();
      return false;
    }
    std::string sni = ssl_sni_hostname_.empty() ? host_ : ssl_sni_hostname_;
    if (!sni.empty()) {
      SSL_set_tlsext_host_name(ssl_stream_->native_handle(), sni.c_str());
      ssl_ctx_->set_verify_callback(asio::ssl::host_name_verification(sni));
    }
    return true;
  }
#endif

  coro_io::ExecutorWrapper<>* executor_;
  asio::ip::tcp::socket socket_;
  std::condition_variable read_loop_cv_;
  std::mutex read_loop_mutex_;
  bool read_loop_running_ = false;
  async_simple::coro::Mutex write_mutex_;
  async_simple::coro::Mutex header_mutex_;
  async_simple::coro::Mutex send_window_mutex_;
  async_simple::coro::Mutex stream_limit_mutex_;
  async_simple::coro::Mutex stream_start_mutex_;
  async_simple::coro::ConditionVariable<async_simple::coro::Mutex>
      send_window_cv_;
  async_simple::coro::ConditionVariable<async_simple::coro::Mutex>
      stream_limit_cv_;
  std::mutex state_mutex_;
  hpack_decoder decoder_;
  hpack_encoder encoder_;
  std::unordered_map<uint32_t, std::shared_ptr<stream_state>> streams_;
  std::vector<uint8_t> payload_buf_;
  continuation_state pending_continuation_;
  std::string host_;
  std::string port_;
  std::string scheme_ = "http";
  std::string authority_;
  uint32_t next_stream_id_ = 1;
  uint32_t last_promised_stream_id_ = 0;
  uint32_t peer_initial_window_size_ = DEFAULT_WINDOW_SIZE;
  uint32_t peer_max_frame_size_ = MAX_FRAME_SIZE;
  uint32_t peer_max_header_list_size_ = std::numeric_limits<uint32_t>::max();
  std::atomic<uint32_t> peer_max_concurrent_streams_ =
      std::numeric_limits<uint32_t>::max();
  bool peer_enable_connect_protocol_ = false;
  std::atomic<bool> peer_settings_ready_ = false;
  std::atomic<uint32_t> peer_goaway_last_stream_id_ =
      std::numeric_limits<uint32_t>::max();
  int32_t connection_send_window_ = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
  int32_t connection_recv_window_ = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
  uint32_t connection_recv_pending_ = 0;
  std::atomic<uint32_t> active_stream_count_ = 0;
  std::atomic<bool> connection_closed_ = false;
  std::atomic<bool> connected_ = false;
  std::atomic<bool> going_away_ = false;
  std::atomic<uint64_t> request_start_next_ticket_ = 0;
  std::atomic<uint64_t> request_start_turn_ = 0;
  std::optional<std::error_code> pending_connection_error_;
  bool enable_push_ = false;
  bool use_h2c_upgrade_ = false;
  std::function<void(h2_pushed_response)> push_handler_;
#ifdef CINATRA_ENABLE_SSL
  std::unique_ptr<asio::ssl::context> ssl_ctx_ = nullptr;
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_;
  std::string ssl_sni_hostname_;
  bool has_init_ssl_ = false;
  bool use_ssl_ = false;
#endif
};

}  // namespace cinatra::http2
