#pragma once
#include <async_simple/Executor.h>
#include <async_simple/coro/ConditionVariable.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/Mutex.h>
#include <async_simple/coro/Sleep.h>

#include <algorithm>
#include <array>
#include <asio/ip/tcp.hpp>
#include <atomic>
#include <charconv>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cinatra/coro_http_request.hpp"
#include "cinatra/coro_http_response.hpp"
#include "cinatra/utils.hpp"
#include "cinatra/ylt/coro_io/coro_io.hpp"
#include "frame.hpp"
#ifdef CINATRA_ENABLE_SSL
#include "h2_request_context.hpp"
#endif
#include "hpack.hpp"

// HTTP/2 server-side connection.
// Handles preface, SETTINGS, HEADERS, DATA, PING, RST_STREAM, GOAWAY,
// WINDOW_UPDATE, CONTINUATION, stream lifecycle, flow control, trailers,
// server push, and request validation.
namespace cinatra::http2 {

#ifdef CINATRA_ENABLE_SSL
inline bool alpn_list_contains_h2(const unsigned char* data, unsigned int len) {
  unsigned int pos = 0;
  while (pos < len) {
    unsigned int proto_len = data[pos++];
    if (pos + proto_len > len)
      return false;
    if (proto_len == 2 && data[pos] == 'h' && data[pos + 1] == '2') {
      return true;
    }
    pos += proto_len;
  }
  return false;
}

inline int select_h2_alpn_callback(::SSL* /*ssl*/, const unsigned char** out,
                                   unsigned char* outlen,
                                   const unsigned char* in, unsigned int inlen,
                                   void* /*arg*/) {
  static constexpr unsigned char H2_PROTO[] = {'h', '2'};
  if (!alpn_list_contains_h2(in, inlen)) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  *out = H2_PROTO;
  *outlen = static_cast<unsigned char>(sizeof(H2_PROTO));
  return SSL_TLSEXT_ERR_OK;
}

inline bool selected_alpn_is_h2(::SSL* ssl) {
  const unsigned char* proto = nullptr;
  unsigned int proto_len = 0;
  SSL_get0_alpn_selected(ssl, &proto, &proto_len);
  return proto_len == 2 && proto != nullptr && proto[0] == 'h' &&
         proto[1] == '2';
}
#endif

// --- Per-stream request and response ---

struct h2_request {
  std::string method;
  std::string path;
  std::string scheme;
  std::string authority;
  std::string protocol;
  std::string body;
  bool saw_post_end_stream_control = false;
  bool needs_flow_control_probe_body = false;
  std::vector<header_field> headers;
  std::vector<header_field> trailers;
  std::unordered_map<std::string, std::string> params_;
  std::smatch matches_;

  std::string_view get_header(std::string_view name) const {
    for (auto& hf : headers)
      if (hf.name == name)
        return hf.value;
    return {};
  }
};

struct common_request_metadata {
  bool needs_flow_control_probe_body = false;
};

struct h2_response {
  int status_code = 200;
  std::string body;
  std::vector<header_field> headers;
  std::vector<header_field> trailers;
  struct server_push {
    std::string method = "GET";
    std::string path = "/";
    std::string scheme;
    std::string authority;
    int status_code = 200;
    std::string body;
    std::vector<header_field> request_headers;
    std::vector<header_field> response_headers;
    std::vector<header_field> response_trailers;

    void add_request_header(std::string name, std::string value) {
      request_headers.push_back({std::move(name), std::move(value)});
    }
    void add_response_header(std::string name, std::string value) {
      response_headers.push_back({std::move(name), std::move(value)});
    }
    void add_response_trailer(std::string name, std::string value) {
      response_trailers.push_back({std::move(name), std::move(value)});
    }
  };
  std::vector<server_push> pushes;

  void set_status_and_body(int code, std::string b) {
    status_code = code;
    body = std::move(b);
  }
  void add_header(std::string name, std::string value) {
    headers.push_back({std::move(name), std::move(value)});
  }
  void add_trailer(std::string name, std::string value) {
    trailers.push_back({std::move(name), std::move(value)});
  }
  server_push& add_push(std::string path, std::string body = {},
                        int code = 200) {
    auto& push = pushes.emplace_back();
    push.path = std::move(path);
    push.body = std::move(body);
    push.status_code = code;
    return push;
  }
};

enum class header_block_kind {
  initial,
  trailer,
};

inline bool contains_invalid_header_name(std::string_view value) {
  if (value.empty())
    return true;
  for (unsigned char ch : value) {
    if (ch <= 0x20 || ch == 0x7f || (ch >= 'A' && ch <= 'Z'))
      return true;
  }
  return false;
}

inline bool contains_invalid_header_value(std::string_view value) {
  for (unsigned char ch : value) {
    if (ch == '\0' || ch == '\r' || ch == '\n')
      return true;
  }
  return false;
}

inline std::optional<uint64_t> parse_content_length(std::string_view value) {
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

inline std::string_view trim_optional_whitespace(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}

inline bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    unsigned char l = static_cast<unsigned char>(lhs[i]);
    unsigned char r = static_cast<unsigned char>(rhs[i]);
    if (l >= 'A' && l <= 'Z')
      l = static_cast<unsigned char>(l - 'A' + 'a');
    if (r >= 'A' && r <= 'Z')
      r = static_cast<unsigned char>(r - 'A' + 'a');
    if (l != r)
      return false;
  }
  return true;
}

inline bool is_connection_specific_header(std::string_view name) {
  return name == "connection" || name == "keep-alive" ||
         name == "proxy-connection" || name == "transfer-encoding" ||
         name == "upgrade";
}

inline bool validate_request_regular_header(std::string_view name,
                                            std::string_view value,
                                            header_block_kind kind) {
  if (is_connection_specific_header(name))
    return false;
  if (name == "te") {
    return kind == header_block_kind::initial &&
           ascii_iequals(trim_optional_whitespace(value), "trailers");
  }
  if (kind == header_block_kind::trailer && name == "content-length")
    return false;
  return true;
}

inline bool validate_response_regular_header(std::string_view name,
                                             std::string_view value,
                                             header_block_kind kind) {
  if (is_connection_specific_header(name) || name == "te")
    return false;
  if (kind == header_block_kind::trailer && name == "content-length")
    return false;
  return true;
}

inline std::string ascii_lower_copy(std::string_view value) {
  std::string result(value);
  for (auto& ch : result) {
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
  }
  return result;
}

inline bool token_list_contains(std::string_view value,
                                std::string_view token) {
  size_t pos = 0;
  while (pos <= value.size()) {
    size_t next = value.find(',', pos);
    auto part = trim_optional_whitespace(value.substr(
        pos, next == std::string_view::npos ? value.size() - pos : next - pos));
    if (ascii_iequals(part, token))
      return true;
    if (next == std::string_view::npos)
      break;
    pos = next + 1;
  }
  return false;
}

inline std::optional<std::vector<uint8_t>> decode_http2_settings_header(
    std::string_view value) {
  std::string encoded(trim_optional_whitespace(value));
  if (encoded.empty())
    return std::vector<uint8_t>{};
  size_t remainder = encoded.size() % 4;
  if (remainder == 1)
    return std::nullopt;
  if (remainder != 0)
    encoded.append(4 - remainder, '=');
  auto decoded = cinatra::base64_decode(encoded);
  if (!decoded.has_value())
    return std::nullopt;
  return std::vector<uint8_t>(decoded->begin(), decoded->end());
}

using h2_handler =
    std::function<async_simple::coro::Lazy<void>(h2_request&, h2_response&)>;
#ifdef CINATRA_ENABLE_SSL
using common_http_handler = std::function<async_simple::coro::Lazy<void>(
    coro_http_request&, coro_http_response&)>;
#endif

// --- Connection class ---

class coro_http2_connection
    : public std::enable_shared_from_this<coro_http2_connection> {
 public:
  static constexpr uint32_t MAX_FRAME_SIZE = 16384;
  static constexpr uint32_t DEFAULT_WINDOW_SIZE = 65535;
  static constexpr uint32_t MAX_WINDOW_SIZE = 0x7fffffff;
  static constexpr uint32_t MAX_ALLOWED_FRAME_SIZE = 16777215;
  static constexpr uint32_t DEFAULT_MAX_CONCURRENT_STREAMS = 100;

  explicit coro_http2_connection(asio::ip::tcp::socket socket,
                                 h2_handler handler,
                                 async_simple::Executor* executor = nullptr)
      : socket_(std::move(socket)),
        h2_handler_(std::move(handler)),
        executor_(executor) {}

#ifdef CINATRA_ENABLE_SSL
  explicit coro_http2_connection(asio::ip::tcp::socket socket,
                                 common_http_handler handler,
                                 async_simple::Executor* executor = nullptr)
      : socket_(std::move(socket)),
        common_handler_(std::move(handler)),
        executor_(executor) {}

  explicit coro_http2_connection(
      asio::ssl::stream<asio::ip::tcp::socket&>& ssl_stream,
      common_http_handler handler, async_simple::Executor* executor = nullptr)
      : socket_(ssl_stream.next_layer().get_executor()),
        common_handler_(std::move(handler)),
        executor_(executor),
        external_ssl_stream_(&ssl_stream),
        use_ssl_(true),
        ssl_handshake_done_(true) {}

  bool init_ssl(const std::string& cert_file, const std::string& key_file,
                std::string passwd = {}) {
    unsigned long ssl_options =
        asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
        asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use;
    try {
      ssl_ctx_ =
          std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
      ssl_ctx_->set_options(ssl_options);
      if (!passwd.empty()) {
        ssl_ctx_->set_password_callback([pwd = std::move(passwd)](auto, auto) {
          return pwd;
        });
      }

      std::error_code ec;
      if (std::filesystem::exists(cert_file, ec)) {
        ssl_ctx_->use_certificate_chain_file(cert_file);
      }
      else {
        return false;
      }

      if (std::filesystem::exists(key_file, ec)) {
        ssl_ctx_->use_private_key_file(key_file, asio::ssl::context::pem);
      }
      else {
        return false;
      }

      SSL_CTX_set_alpn_select_cb(ssl_ctx_->native_handle(),
                                 select_h2_alpn_callback, nullptr);
      ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(
          socket_, *ssl_ctx_);
      use_ssl_ = true;
    } catch (...) {
      return false;
    }
    return true;
  }
#endif

  void set_quit_callback(std::function<void()> cb) {
    quit_callback_ = std::move(cb);
  }

  void set_enable_connect_protocol(bool enabled) {
    enable_connect_protocol_ = enabled;
  }

  async_simple::Executor* get_executor() const { return executor_; }

  void close() {
    asio::dispatch(active_socket().get_executor(), [self = shared_from_this()] {
      self->force_close();
    });
  }

  void force_close() {
    close_send_waiters();
    std::error_code ignored;
    auto& socket = active_socket();
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
  }

  // Initiate graceful shutdown: send GOAWAY, stop accepting new streams.
  async_simple::coro::Lazy<void> graceful_shutdown() {
    if (!going_away_) {
      graceful_wait_for_peer_close_ = has_active_streams();
      going_away_ = true;
      if (!co_await write_frame(
              make_goaway(last_stream_id_, h2_error_code::no_error))) {
        force_close();
        co_return;
      }
    }
    finish_graceful_shutdown();
  }

  // Entry point: run the connection until it closes.
  async_simple::coro::Lazy<void> start() {
    struct scoped_notify {
      coro_http2_connection* self;
      ~scoped_notify() {
        auto keepalive = std::move(self->lifetime_guard_);
        self->close_send_waiters();
        self->notify_quit();
      }
    } on_exit{this};

    lifetime_guard_ = shared_from_this();
    if (executor_ == nullptr)
      executor_ = co_await async_simple::CurrentExecutor{};
    auto finish_before_return = [this]() -> async_simple::coro::Lazy<void> {
      close_send_waiters();
      flush_pending_dispatches();
      co_await wait_for_dispatches();
      cleanup_done_streams();
    };
    bool sent_server_settings = false;
    bool startup_preface_consumed = false;
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      auto* ssl_stream = active_ssl_stream();
      if (ssl_stream == nullptr) {
        co_await finish_before_return();
        co_return;
      }
      if (!ssl_handshake_done_) {
        auto ec = co_await coro_io::async_handshake(
            ssl_stream_, asio::ssl::stream_base::server);
        if (ec || !selected_alpn_is_h2(ssl_stream->native_handle())) {
          force_close();
          co_await finish_before_return();
          co_return;
        }
        ssl_handshake_done_ = true;
      }
      if (!co_await read_preface()) {
        co_await finish_before_return();
        co_return;
      }
      startup_preface_consumed = true;
    }
#endif
    if (!startup_preface_consumed) {
      auto startup = co_await handle_plaintext_startup();
      if (startup == startup_mode::failed) {
        co_await finish_before_return();
        co_return;
      }
      if (startup == startup_mode::h2c_upgrade) {
        std::array<settings_entry, 5> srv_settings{
            settings_entry{settings_param::header_table_size, 4096},
            settings_entry{settings_param::max_concurrent_streams,
                           local_max_concurrent_streams_},
            settings_entry{settings_param::initial_window_size,
                           local_initial_window_size_},
            settings_entry{settings_param::max_frame_size, MAX_FRAME_SIZE},
            settings_entry{settings_param::enable_connect_protocol,
                           enable_connect_protocol_ ? 1u : 0u}};
        if (!co_await write_frame(make_settings_frame(srv_settings)))
          co_return;
        sent_server_settings = true;
        if (pending_upgrade_stream_id_ != 0 &&
            !start_stream_dispatch(pending_upgrade_stream_id_)) {
          co_await finish_before_return();
          co_return;
        }
        if (!co_await read_preface()) {
          co_await finish_before_return();
          co_return;
        }
      }
    }

    // Send server SETTINGS immediately
    std::array<settings_entry, 5> srv_settings{
        settings_entry{settings_param::header_table_size, 4096},
        settings_entry{settings_param::max_concurrent_streams,
                       local_max_concurrent_streams_},
        settings_entry{settings_param::initial_window_size,
                       local_initial_window_size_},
        settings_entry{settings_param::max_frame_size, MAX_FRAME_SIZE},
        settings_entry{settings_param::enable_connect_protocol,
                       enable_connect_protocol_ ? 1u : 0u}};
    if (!sent_server_settings &&
        !co_await write_frame(make_settings_frame(srv_settings))) {
      co_await finish_before_return();
      co_return;
    }

    // Single read loop: read one frame, handle inline, repeat
    std::array<uint8_t, 9> hdr_buf;
    while (true) {
      flush_pending_dispatches();
      cleanup_done_streams();
      bool input_idle_before_read = !has_buffered_input();
      if (input_idle_before_read)
        reset_remote_stream_burst_to_active();

      // Read 9-byte frame header
      {
        auto buf = asio::buffer(hdr_buf);
        auto [ec, n] = co_await read_from_peer(buf, 9);
        if (ec || n != 9) {
          co_await finish_before_return();
          co_return;
        }
      }

      auto hdr = parse_frame_header(hdr_buf);
      if (!peer_settings_received_) {
        if (hdr.type != frame_type::settings || hdr.stream_id != 0 ||
            (hdr.flags & flags::ACK)) {
          co_await write_frame(
              make_goaway(last_stream_id_, h2_error_code::protocol_error));
          std::error_code ignored;
          active_socket().shutdown(asio::ip::tcp::socket::shutdown_send,
                                   ignored);
          co_await finish_before_return();
          co_return;
        }
      }
      if (hdr.length > MAX_FRAME_SIZE) {
        co_await write_frame(
            make_goaway(last_stream_id_, h2_error_code::frame_size_error));
        std::error_code ignored;
        active_socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        co_await finish_before_return();
        co_return;
      }

      // Read payload
      payload_buf_.resize(hdr.length);
      if (hdr.length > 0) {
        auto buf = asio::buffer(payload_buf_);
        auto [ec, n] = co_await read_from_peer(buf, hdr.length);
        if (ec || n != hdr.length) {
          co_await finish_before_return();
          co_return;
        }
      }

      if (input_idle_before_read) {
        cleanup_done_streams();
        reset_remote_stream_burst_to_active();
      }

      std::span<const uint8_t> payload(payload_buf_.data(), hdr.length);
      if (!co_await handle_frame(hdr, payload)) {
        // Graceful TCP shutdown so GOAWAY reaches the peer before close.
        std::error_code ignored;
        active_socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        co_await finish_before_return();
        co_return;
      }
      cleanup_done_streams();
    }
  }

 private:
  // --- Stream lifecycle (RFC 7540 section 5.1) ---

  enum class stream_lifecycle {
    idle,
    open,
    half_closed_remote,  // client sent END_STREAM
    half_closed_local,   // server sent END_STREAM
    closed,
  };

  struct stream_state {
    h2_request req;
    std::vector<uint8_t> hdr_block_buf;
    bool end_stream = false;
    bool end_headers = false;
    bool initial_headers_received = false;
    bool trailing_headers_received = false;
    stream_lifecycle state = stream_lifecycle::idle;
    int32_t recv_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
    uint32_t recv_pending = 0;  // bytes consumed but not yet ACKed
    int32_t send_window = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
    bool dispatch_started = false;
    bool dispatch_done = false;  // set by dispatch_stream
    std::optional<uint64_t> content_length;
    priority_spec priority{false, 0, 15};
    size_t pending_send_bytes = 0;
  };

  enum class flow_control_result {
    ok,
    stream_error,
    connection_error,
  };

  enum class header_decode_result {
    ok,
    stream_error,
    connection_error,
  };

  struct payload_view_result {
    bool ok = false;
    bool stream_error = false;  // if true, caller should RST_STREAM not GOAWAY
    std::span<const uint8_t> payload{};
    h2_error_code error_code = h2_error_code::protocol_error;
    std::optional<priority_spec> priority;
  };

  struct h2c_upgrade_request {
    h2_request req;
    std::vector<settings_entry> settings;
  };

  struct send_schedule_entry {
    uint32_t stream_id = 0;
    std::weak_ptr<stream_state> state;
  };

  struct priority_node {
    uint32_t stream_id = 0;
    uint32_t parent_stream_id = 0;
    uint8_t weight = 15;
    std::vector<uint32_t> children;
  };

  struct prepared_push_response {
    uint32_t stream_id = 0;
    std::shared_ptr<stream_state> stream;
    h2_response response;
  };

  enum class startup_mode {
    prior_knowledge,
    h2c_upgrade,
    failed,
  };

  // --- Preface ---

  async_simple::coro::Lazy<bool> read_preface() {
    std::array<uint8_t, 24> buf;
    auto abuf = asio::buffer(buf);
    auto [ec, n] = co_await read_from_peer(abuf, CLIENT_PREFACE.size());
    if (ec || n != CLIENT_PREFACE.size())
      co_return false;
    co_return std::string_view(reinterpret_cast<const char*>(buf.data()), n) ==
        CLIENT_PREFACE;
  }

  // --- Frame writer ---

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
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>>
  read_some_from_peer(AsioBuffer&& buffer) {
    auto mutable_buffer = buffer;
    if (!prefetched_bytes_.empty()) {
      size_t copied = std::min(prefetched_bytes_.size(), mutable_buffer.size());
      std::memcpy(asio::buffer_cast<void*>(mutable_buffer),
                  prefetched_bytes_.data(), copied);
      prefetched_bytes_.erase(prefetched_bytes_.begin(),
                              prefetched_bytes_.begin() + copied);
      co_return std::pair<std::error_code, size_t>{std::error_code{}, copied};
    }
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      co_return co_await coro_io::async_read_some(*active_ssl_stream(), buffer);
    }
#endif
    co_return co_await coro_io::async_read_some(socket_, buffer);
  }

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> read_from_peer(
      AsioBuffer& buffer, size_t size_to_read) {
    auto mutable_buffer = buffer;
    size_t copied = 0;
    if (!prefetched_bytes_.empty()) {
      copied = std::min(prefetched_bytes_.size(), size_to_read);
      std::memcpy(asio::buffer_cast<void*>(mutable_buffer),
                  prefetched_bytes_.data(), copied);
      prefetched_bytes_.erase(prefetched_bytes_.begin(),
                              prefetched_bytes_.begin() + copied);
      if (copied == size_to_read) {
        co_return std::pair<std::error_code, size_t>{std::error_code{}, copied};
      }
      mutable_buffer += copied;
    }
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      auto [ec, n] = co_await coro_io::async_read(
          *active_ssl_stream(), mutable_buffer, size_to_read - copied);
      co_return std::pair<std::error_code, size_t>{ec, copied + n};
    }
#endif
    auto [ec, n] = co_await coro_io::async_read(socket_, mutable_buffer,
                                                size_to_read - copied);
    co_return std::pair<std::error_code, size_t>{ec, copied + n};
  }

  template <typename AsioBuffer>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> write_to_peer(
      AsioBuffer&& buffer) {
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      co_return co_await coro_io::async_write(*active_ssl_stream(), buffer);
    }
#endif
    co_return co_await coro_io::async_write(socket_, buffer);
  }

  async_simple::coro::Lazy<bool> write_header_block(
      uint32_t stream_id, std::span<const header_field> headers,
      uint8_t first_frame_flags) {
    co_return co_await write_header_block_impl(stream_id, headers,
                                               first_frame_flags);
  }

  async_simple::coro::Lazy<bool> write_header_block(
      uint32_t stream_id, std::span<const header_field_view> headers,
      uint8_t first_frame_flags) {
    co_return co_await write_header_block_impl(stream_id, headers,
                                               first_frame_flags);
  }

  template <typename HeaderSpan>
  async_simple::coro::Lazy<bool> write_header_block_impl(
      uint32_t stream_id, HeaderSpan headers, uint8_t first_frame_flags) {
    auto header_lock = co_await header_mutex_.coScopedLock();
    auto write_lock = co_await write_mutex_.coScopedLock();

    auto header_block = encoder_.encode(headers);
    auto frames = make_header_block_frames(
        stream_id, std::span<const uint8_t>(header_block), first_frame_flags,
        std::min(peer_max_frame_size_, MAX_ALLOWED_FRAME_SIZE));
    for (auto& frame : frames) {
      if (!co_await write_frame_locked(frame))
        co_return false;
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> write_push_promise_block(
      uint32_t parent_stream_id, uint32_t promised_stream_id,
      std::span<const header_field> headers) {
    auto header_lock = co_await header_mutex_.coScopedLock();
    auto write_lock = co_await write_mutex_.coScopedLock();

    auto header_block = encoder_.encode(headers);
    auto frames = make_push_promise_frames(
        parent_stream_id, promised_stream_id,
        std::span<const uint8_t>(header_block), flags::END_HEADERS,
        std::min(peer_max_frame_size_, MAX_ALLOWED_FRAME_SIZE));
    for (auto& frame : frames) {
      if (!co_await write_frame_locked(frame))
        co_return false;
    }
    co_return true;
  }

  std::optional<h2_error_code> apply_peer_settings(
      std::span<const settings_entry> settings) {
    for (auto setting : settings) {
      switch (setting.id) {
        case settings_param::header_table_size:
          encoder_.set_max_dynamic_table_size(setting.value);
          break;
        case settings_param::enable_push:
          if (setting.value > 1)
            return h2_error_code::protocol_error;
          peer_enable_push_ = setting.value == 1;
          break;
        case settings_param::max_concurrent_streams:
          peer_max_concurrent_streams_ = setting.value;
          break;
        case settings_param::initial_window_size:
          if (setting.value > MAX_WINDOW_SIZE)
            return h2_error_code::flow_control_error;
          {
            int64_t delta = static_cast<int64_t>(setting.value) -
                            static_cast<int64_t>(peer_initial_window_size_);
            for (auto& [id, st] : streams_) {
              int64_t next = static_cast<int64_t>(st->send_window) + delta;
              if (next < 0 || next > MAX_WINDOW_SIZE)
                return h2_error_code::flow_control_error;
              st->send_window = static_cast<int32_t>(next);
            }
          }
          peer_initial_window_size_ = setting.value;
          send_window_cv_.notifyAll();
          break;
        case settings_param::max_frame_size:
          if (setting.value < MAX_FRAME_SIZE ||
              setting.value > MAX_ALLOWED_FRAME_SIZE) {
            return h2_error_code::protocol_error;
          }
          peer_max_frame_size_ = setting.value;
          break;
        case settings_param::max_header_list_size:
          peer_max_header_list_size_ = setting.value;
          break;
        case settings_param::enable_connect_protocol:
          if (setting.value > 1)
            return h2_error_code::protocol_error;
          peer_enable_connect_protocol_ = setting.value == 1;
          break;
      }
    }
    return std::nullopt;
  }

  void remember_closed_stream(uint32_t stream_id) {
    if (stream_id == 0)
      return;
    if (closed_stream_ids_.find(stream_id) != closed_stream_ids_.end())
      return;
    closed_stream_ids_.insert(stream_id);
    closed_stream_order_.push_back(stream_id);
    while (closed_stream_order_.size() > closed_stream_history_limit_) {
      closed_stream_ids_.erase(closed_stream_order_.front());
      closed_stream_order_.pop_front();
    }
  }

  bool stream_was_closed(uint32_t stream_id) const {
    return closed_stream_ids_.find(stream_id) != closed_stream_ids_.end();
  }

  bool stream_is_idle(uint32_t stream_id) const {
    return stream_id != 0 && streams_.find(stream_id) == streams_.end() &&
           !stream_was_closed(stream_id) && stream_id > last_stream_id_;
  }

  static std::optional<h2c_upgrade_request> parse_h2c_upgrade_headers(
      std::string_view header_block, uint64_t& content_length) {
    auto line_end = header_block.find("\r\n");
    if (line_end == std::string_view::npos)
      return std::nullopt;

    auto request_line = header_block.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
        sp1 == sp2) {
      return std::nullopt;
    }

    auto method = request_line.substr(0, sp1);
    auto target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    auto version = request_line.substr(sp2 + 1);
    if (version != "HTTP/1.1" || method.empty() || target.empty())
      return std::nullopt;

    h2c_upgrade_request parsed;
    parsed.req.method = std::string(method);
    parsed.req.path = std::string(target);
    parsed.req.scheme = "http";

    bool saw_upgrade = false;
    bool saw_connection_upgrade = false;
    bool saw_connection_http2_settings = false;
    bool saw_http2_settings = false;
    std::optional<uint64_t> parsed_content_length;

    size_t pos = line_end + 2;
    while (pos < header_block.size()) {
      size_t next = header_block.find("\r\n", pos);
      if (next == std::string_view::npos)
        next = header_block.size();
      if (next == pos)
        break;

      auto line = header_block.substr(pos, next - pos);
      auto colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0)
        return std::nullopt;

      auto name =
          ascii_lower_copy(trim_optional_whitespace(line.substr(0, colon)));
      auto value = trim_optional_whitespace(line.substr(colon + 1));
      if (contains_invalid_header_name(name) ||
          contains_invalid_header_value(value)) {
        return std::nullopt;
      }

      if (name == "host") {
        if (parsed.req.authority.empty())
          parsed.req.authority = std::string(value);
        pos = next + 2;
        continue;
      }
      if (name == "connection") {
        saw_connection_upgrade = token_list_contains(value, "upgrade");
        saw_connection_http2_settings =
            token_list_contains(value, "http2-settings");
        pos = next + 2;
        continue;
      }
      if (name == "upgrade") {
        saw_upgrade = ascii_iequals(value, "h2c");
        pos = next + 2;
        continue;
      }
      if (name == "http2-settings") {
        if (saw_http2_settings)
          return std::nullopt;
        auto decoded = decode_http2_settings_header(value);
        if (!decoded.has_value() || (decoded->size() % 6) != 0)
          return std::nullopt;
        parsed.settings = parse_settings_payload(*decoded);
        saw_http2_settings = true;
        pos = next + 2;
        continue;
      }
      if (name == "transfer-encoding")
        return std::nullopt;
      if (name == "content-length") {
        if (parsed_content_length.has_value())
          return std::nullopt;
        auto len = parse_content_length(value);
        if (!len.has_value())
          return std::nullopt;
        parsed_content_length = *len;
      }
      if (!validate_request_regular_header(name, value,
                                           header_block_kind::initial))
        return std::nullopt;
      parsed.req.headers.push_back({std::move(name), std::string(value)});
      pos = next == header_block.size() ? next : next + 2;
    }

    if (!saw_upgrade || !saw_connection_upgrade ||
        !saw_connection_http2_settings || !saw_http2_settings ||
        parsed.req.authority.empty()) {
      return std::nullopt;
    }
    if (parsed.req.method == "CONNECT")
      return std::nullopt;
    if (parsed.req.path == "*" && parsed.req.method != "OPTIONS")
      return std::nullopt;

    content_length = parsed_content_length.value_or(0);
    return parsed;
  }

  bool prepare_h2c_upgrade_stream(h2c_upgrade_request&& upgrade) {
    auto st = std::make_shared<stream_state>();
    st->req = std::move(upgrade.req);
    st->state = stream_lifecycle::half_closed_remote;
    st->recv_window = static_cast<int32_t>(local_initial_window_size_);
    st->send_window = static_cast<int32_t>(peer_initial_window_size_);
    st->initial_headers_received = true;
    st->end_stream = true;
    st->end_headers = true;
    if (auto content_length =
            parse_content_length(st->req.get_header("content-length"));
        content_length.has_value()) {
      st->content_length = *content_length;
      if (st->req.body.size() != *content_length)
        return false;
    }
    streams_.emplace(1, st);
    ensure_priority_node(0);
    auto& node = ensure_priority_node(1);
    st->priority.stream_dependency = node.parent_stream_id;
    st->priority.weight = node.weight;
    last_stream_id_ = 1;
    pending_upgrade_stream_id_ = 1;
    return true;
  }

  async_simple::coro::Lazy<bool> write_http1_switching_protocols() {
    static constexpr std::string_view response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: h2c\r\n\r\n";
    auto [ec, n] = co_await write_to_peer(asio::buffer(response));
    co_return !ec && n == response.size();
  }

  async_simple::coro::Lazy<bool> write_http1_bad_request() {
    static constexpr std::string_view response =
        "HTTP/1.1 400 Bad Request\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n\r\n";
    auto [ec, n] = co_await write_to_peer(asio::buffer(response));
    co_return !ec && n == response.size();
  }

  static bool looks_like_http1_request(std::string_view header_block) {
    auto line_end = header_block.find("\r\n");
    if (line_end == std::string_view::npos)
      return false;

    auto request_line = header_block.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
        sp1 == sp2) {
      return false;
    }

    auto method = request_line.substr(0, sp1);
    auto target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    auto version = request_line.substr(sp2 + 1);
    return !method.empty() && !target.empty() && version == "HTTP/1.1";
  }

  async_simple::coro::Lazy<startup_mode> handle_plaintext_startup() {
    std::string startup_bytes;
    startup_bytes.reserve(1024);
    std::array<uint8_t, 1024> read_buf{};

    while (startup_bytes.size() < 32768) {
      if (startup_bytes.size() >= CLIENT_PREFACE.size() &&
          std::string_view(startup_bytes.data(), CLIENT_PREFACE.size()) ==
              CLIENT_PREFACE) {
        if (startup_bytes.size() > CLIENT_PREFACE.size()) {
          prefetched_bytes_.insert(
              prefetched_bytes_.end(),
              reinterpret_cast<const uint8_t*>(startup_bytes.data()) +
                  CLIENT_PREFACE.size(),
              reinterpret_cast<const uint8_t*>(startup_bytes.data()) +
                  startup_bytes.size());
        }
        co_return startup_mode::prior_knowledge;
      }

      bool could_be_preface =
          startup_bytes.size() < CLIENT_PREFACE.size() &&
          std::string_view(CLIENT_PREFACE.data(), startup_bytes.size()) ==
              std::string_view(startup_bytes.data(), startup_bytes.size());
      if (!could_be_preface) {
        auto header_end_pos = startup_bytes.find("\r\n\r\n");
        if (header_end_pos != std::string::npos) {
          uint64_t content_length = 0;
          auto parsed = parse_h2c_upgrade_headers(
              std::string_view(startup_bytes.data(), header_end_pos),
              content_length);
          if (!parsed.has_value()) {
            if (looks_like_http1_request(
                    std::string_view(startup_bytes.data(), header_end_pos))) {
              co_await write_http1_bad_request();
            }
            else {
              co_await write_frame(
                  make_goaway(0, h2_error_code::protocol_error));
            }
            co_return startup_mode::failed;
          }

          size_t message_end =
              header_end_pos + 4 + static_cast<size_t>(content_length);
          while (startup_bytes.size() < message_end) {
            auto [ec, n] = co_await read_some_from_peer(asio::buffer(read_buf));
            if (ec || n == 0)
              co_return startup_mode::failed;
            startup_bytes.append(reinterpret_cast<const char*>(read_buf.data()),
                                 n);
          }

          parsed->req.body.assign(startup_bytes.data() + header_end_pos + 4,
                                  static_cast<size_t>(content_length));
          if (auto apply_error = apply_peer_settings(parsed->settings);
              apply_error.has_value()) {
            co_await write_http1_bad_request();
            co_return startup_mode::failed;
          }
          if (!prepare_h2c_upgrade_stream(std::move(*parsed))) {
            co_await write_http1_bad_request();
            co_return startup_mode::failed;
          }
          if (!co_await write_http1_switching_protocols())
            co_return startup_mode::failed;
          if (startup_bytes.size() > message_end) {
            prefetched_bytes_.insert(
                prefetched_bytes_.end(),
                reinterpret_cast<const uint8_t*>(startup_bytes.data()) +
                    message_end,
                reinterpret_cast<const uint8_t*>(startup_bytes.data()) +
                    startup_bytes.size());
          }
          co_return startup_mode::h2c_upgrade;
        }
      }

      auto [ec, n] = co_await read_some_from_peer(asio::buffer(read_buf));
      if (ec || n == 0)
        co_return startup_mode::failed;
      startup_bytes.append(reinterpret_cast<const char*>(read_buf.data()), n);
    }

    co_await write_http1_bad_request();
    co_return startup_mode::failed;
  }

  // --- Frame dispatcher ---

  async_simple::coro::Lazy<bool> handle_frame(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    // RFC 7540 section 6.10: while a header block is incomplete, only
    // CONTINUATION frames for the same stream are allowed. Anything else is
    // PROTOCOL_ERROR.
    if (pending_continuation_stream_ != 0) {
      if (hdr.type != frame_type::continuation ||
          hdr.stream_id != pending_continuation_stream_) {
        co_await write_frame(
            make_goaway(last_stream_id_, h2_error_code::protocol_error));
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

  // --- SETTINGS ---

  async_simple::coro::Lazy<bool> on_settings(const frame_header& hdr,
                                             std::span<const uint8_t> payload) {
    if (hdr.stream_id != 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    if (hdr.flags & flags::ACK) {
      if (hdr.length != 0) {
        co_await write_frame(
            make_goaway(last_stream_id_, h2_error_code::frame_size_error));
        co_return false;
      }
      co_return true;
    }

    if ((hdr.length % 6) != 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::frame_size_error));
      co_return false;
    }

    if (auto apply_error = apply_peer_settings(parse_settings_payload(payload));
        apply_error.has_value()) {
      co_await write_frame(make_goaway(last_stream_id_, *apply_error));
      co_return false;
    }

    peer_settings_received_ = true;
    co_return co_await write_frame(make_settings_frame({}, true));
  }

  // --- WINDOW_UPDATE ---

  async_simple::coro::Lazy<bool> on_window_update(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (hdr.length != 4) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::frame_size_error));
      co_return false;
    }

    uint32_t increment = ((uint32_t(payload[0]) & 0x7f) << 24) |
                         (uint32_t(payload[1]) << 16) |
                         (uint32_t(payload[2]) << 8) | uint32_t(payload[3]);
    if (increment == 0) {
      if (hdr.stream_id == 0) {
        co_await write_frame(
            make_goaway(last_stream_id_, h2_error_code::protocol_error));
        co_return false;
      }
      abort_stream(hdr.stream_id);
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
    }
    if (hdr.stream_id == 0) {
      if (increment >
          static_cast<uint32_t>(MAX_WINDOW_SIZE - connection_send_window_)) {
        co_await write_frame(
            make_goaway(last_stream_id_, h2_error_code::flow_control_error));
        co_return false;
      }
      connection_send_window_ += static_cast<int32_t>(increment);
    }
    else if (auto it = streams_.find(hdr.stream_id); it != streams_.end()) {
      if (increment >
          static_cast<uint32_t>(MAX_WINDOW_SIZE - it->second->send_window)) {
        abort_stream(hdr.stream_id);
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, h2_error_code::flow_control_error));
      }
      if (it->second->state == stream_lifecycle::half_closed_remote) {
        it->second->req.saw_post_end_stream_control = true;
        if (peer_initial_window_size_ <= 1)
          it->second->req.needs_flow_control_probe_body = true;
      }
      it->second->send_window += static_cast<int32_t>(increment);
    }
    else if (stream_is_idle(hdr.stream_id) ||
             !stream_was_closed(hdr.stream_id)) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    send_window_cv_.notifyAll();
    co_return true;
  }

  // --- HEADERS ---

  async_simple::coro::Lazy<bool> on_headers(const frame_header& hdr,
                                            std::span<const uint8_t> payload) {
    if (hdr.stream_id == 0) {
      co_await write_frame(make_goaway(0, h2_error_code::protocol_error));
      co_return false;
    }

    // RFC 7540 section 5.1.1: client-initiated stream IDs must be odd and
    // strictly greater than any previously opened client stream.
    bool is_new = streams_.find(hdr.stream_id) == streams_.end();
    if (is_new) {
      if ((hdr.stream_id & 1u) == 0 || hdr.stream_id <= last_stream_id_) {
        co_await write_frame(
            make_goaway(last_stream_id_, stream_was_closed(hdr.stream_id)
                                             ? h2_error_code::stream_closed
                                             : h2_error_code::protocol_error));
        co_return false;
      }
      if (remote_stream_limit_reached()) {
        last_stream_id_ = hdr.stream_id;
        remember_closed_stream(hdr.stream_id);
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, h2_error_code::refused_stream));
      }
      if (going_away_) {
        // Reject new streams after GOAWAY.
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, h2_error_code::refused_stream));
      }
      note_remote_stream_opened();
      last_stream_id_ = hdr.stream_id;
    }

    auto& st = streams_[hdr.stream_id];
    if (is_new) {
      st = std::make_shared<stream_state>();
      st->state = stream_lifecycle::open;
      st->recv_window = static_cast<int32_t>(local_initial_window_size_);
      st->send_window = static_cast<int32_t>(peer_initial_window_size_);
      ensure_priority_node(0);
      auto& node = ensure_priority_node(hdr.stream_id);
      st->priority.stream_dependency = node.parent_stream_id;
      st->priority.weight = node.weight;
    }
    else if (!st->initial_headers_received) {
      abort_stream(hdr.stream_id);
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
    }
    else if (st->state == stream_lifecycle::closed) {
      abort_stream(hdr.stream_id);
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::stream_closed));
      co_return false;
    }
    else if (st->trailing_headers_received || st->dispatch_started ||
             st->state != stream_lifecycle::open) {
      abort_stream(hdr.stream_id);
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::stream_closed));
    }

    auto frag = strip_padding_priority(hdr, payload);
    if (!frag.ok) {
      if (frag.stream_error) {
        abort_stream(hdr.stream_id);
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, frag.error_code));
      }
      co_await write_frame(make_goaway(last_stream_id_, frag.error_code));
      co_return false;
    }
    st->hdr_block_buf.insert(st->hdr_block_buf.end(), frag.payload.begin(),
                             frag.payload.end());
    if (frag.priority.has_value()) {
      st->priority = *frag.priority;
      apply_priority_spec(hdr.stream_id, *frag.priority);
    }
    st->end_stream = (hdr.flags & flags::END_STREAM) != 0;
    st->end_headers = (hdr.flags & flags::END_HEADERS) != 0;

    auto block_kind = st->initial_headers_received ? header_block_kind::trailer
                                                   : header_block_kind::initial;
    if (block_kind == header_block_kind::trailer && !st->end_stream) {
      abort_stream(hdr.stream_id);
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
    }

    if (!st->end_headers) {
      pending_continuation_stream_ = hdr.stream_id;
      co_return true;
    }

    pending_continuation_stream_ = 0;
    auto decode_result = decode_headers(*st, block_kind);
    if (decode_result != header_decode_result::ok) {
      if (decode_result == header_decode_result::connection_error) {
        auto ec =
            pending_connection_error_.value_or(h2_error_code::protocol_error);
        pending_connection_error_.reset();
        co_await write_frame(make_goaway(last_stream_id_, ec));
        co_return false;
      }
      abort_stream(hdr.stream_id);
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
    }
    if (block_kind == header_block_kind::trailer) {
      st->trailing_headers_received = true;
      if (st->content_length.has_value() &&
          st->req.body.size() != *st->content_length) {
        abort_stream(hdr.stream_id);
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
      }
    }
    else {
      st->initial_headers_received = true;
    }
    if (st->end_stream) {
      st->state = stream_lifecycle::half_closed_remote;
      co_return start_stream_dispatch(hdr.stream_id);
    }
    co_return true;
  }

  // --- PRIORITY / PUSH_PROMISE ---

  async_simple::coro::Lazy<bool> on_priority(const frame_header& hdr,
                                             std::span<const uint8_t> payload) {
    if (hdr.stream_id == 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    auto spec = parse_priority_payload(payload);
    if (!spec.has_value()) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::frame_size_error));
      co_return false;
    }
    if (spec->stream_dependency == hdr.stream_id) {
      // RFC 7540 section 5.3.1: self-dependency is a stream error of type
      // PROTOCOL_ERROR, not a connection error.
      abort_stream(hdr.stream_id);
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
    }
    apply_priority_spec(hdr.stream_id, *spec);
    if (auto it = streams_.find(hdr.stream_id); it != streams_.end()) {
      if (it->second->state == stream_lifecycle::half_closed_remote) {
        it->second->req.saw_post_end_stream_control = true;
        if (peer_initial_window_size_ <= 1)
          it->second->req.needs_flow_control_probe_body = true;
      }
      it->second->priority = *spec;
      send_window_cv_.notifyAll();
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_push_promise(
      const frame_header& /*hdr*/, std::span<const uint8_t> /*payload*/) {
    co_await write_frame(
        make_goaway(last_stream_id_, h2_error_code::protocol_error));
    co_return false;
  }

  // --- CONTINUATION ---

  async_simple::coro::Lazy<bool> on_continuation(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (pending_continuation_stream_ == 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    auto it = streams_.find(hdr.stream_id);
    if (it == streams_.end() || it->second->state == stream_lifecycle::closed) {
      co_await write_frame(
          make_goaway(last_stream_id_, stream_was_closed(hdr.stream_id)
                                           ? h2_error_code::stream_closed
                                           : h2_error_code::protocol_error));
      co_return false;
    }
    auto st = it->second;
    st->hdr_block_buf.insert(st->hdr_block_buf.end(), payload.begin(),
                             payload.end());
    if (hdr.flags & flags::END_HEADERS) {
      st->end_headers = true;
      pending_continuation_stream_ = 0;
      auto block_kind = st->initial_headers_received
                            ? header_block_kind::trailer
                            : header_block_kind::initial;
      auto decode_result = decode_headers(*st, block_kind);
      if (decode_result != header_decode_result::ok) {
        if (decode_result == header_decode_result::connection_error) {
          auto ec =
              pending_connection_error_.value_or(h2_error_code::protocol_error);
          pending_connection_error_.reset();
          co_await write_frame(make_goaway(last_stream_id_, ec));
          co_return false;
        }
        abort_stream(hdr.stream_id);
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
      }
      if (block_kind == header_block_kind::trailer) {
        st->trailing_headers_received = true;
        if (st->content_length.has_value() &&
            st->req.body.size() != *st->content_length) {
          abort_stream(hdr.stream_id);
          co_return co_await write_frame(
              make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
        }
      }
      else {
        st->initial_headers_received = true;
      }
      if (st->end_stream) {
        st->state = stream_lifecycle::half_closed_remote;
        co_return start_stream_dispatch(hdr.stream_id);
      }
    }
    co_return true;
  }

  // --- RST_STREAM ---

  async_simple::coro::Lazy<bool> on_rst_stream(
      const frame_header& hdr, std::span<const uint8_t> /*payload*/) {
    // RFC 7540 section 6.4: RST_STREAM must have stream_id != 0 and payload
    // length 4.
    if (hdr.stream_id == 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    if (hdr.length != 4) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::frame_size_error));
      co_return false;
    }
    if (stream_is_idle(hdr.stream_id)) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    if (!stream_was_closed(hdr.stream_id) &&
        streams_.find(hdr.stream_id) == streams_.end()) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    // Clean up stream; do not dispatch. (Note: a RST_STREAM arriving while
    // pending_continuation_stream_ != 0 is already rejected by handle_frame,
    // so no continuation reset is needed here.)
    abort_stream(hdr.stream_id);
    co_return true;
  }

  async_simple::coro::Lazy<bool> on_goaway(const frame_header& hdr,
                                           std::span<const uint8_t> payload) {
    if (hdr.stream_id != 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    if (payload.size() < 8) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::frame_size_error));
      co_return false;
    }

    going_away_ = true;
    peer_sent_goaway_ = true;
    if (streams_.empty()) {
      force_close();
      co_return false;
    }
    co_return true;
  }

  // --- DATA ---

  async_simple::coro::Lazy<bool> on_data(const frame_header& hdr,
                                         std::span<const uint8_t> payload) {
    if (hdr.stream_id == 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    auto it = streams_.find(hdr.stream_id);
    if (it == streams_.end()) {
      if (stream_is_idle(hdr.stream_id) || !stream_was_closed(hdr.stream_id)) {
        co_await write_frame(
            make_goaway(last_stream_id_, h2_error_code::protocol_error));
        co_return false;
      }
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::stream_closed));
    }
    auto st = it->second;
    // DATA is only valid in open / half_closed_local (client can still send).
    if (st->state != stream_lifecycle::open &&
        st->state != stream_lifecycle::half_closed_local) {
      co_return co_await write_frame(
          make_rst_stream(hdr.stream_id, h2_error_code::stream_closed));
    }
    payload_view_result data{.ok = true, .payload = payload};
    if (hdr.flags & flags::PADDED) {
      data = strip_padded_payload(payload);
      if (!data.ok) {
        co_await write_frame(make_goaway(last_stream_id_, data.error_code));
        co_return false;
      }
    }
    auto flow_result =
        co_await consume_flow_control(hdr.stream_id, hdr.length, *st);
    if (flow_result == flow_control_result::connection_error)
      co_return false;
    if (flow_result == flow_control_result::stream_error)
      co_return true;
    st->req.body.append(reinterpret_cast<const char*>(data.payload.data()),
                        data.payload.size());
    bool stream_done = (hdr.flags & flags::END_STREAM) != 0;
    if (!co_await refill_flow_control(hdr.stream_id, *st, hdr.length,
                                      stream_done))
      co_return false;
    if (hdr.flags & flags::END_STREAM) {
      if (st->content_length.has_value() &&
          st->req.body.size() != *st->content_length) {
        abort_stream(hdr.stream_id);
        co_return co_await write_frame(
            make_rst_stream(hdr.stream_id, h2_error_code::protocol_error));
      }
      st->state = stream_lifecycle::half_closed_remote;
      co_return start_stream_dispatch(hdr.stream_id);
    }
    co_return true;
  }

  // --- PING ---

  async_simple::coro::Lazy<bool> on_ping(const frame_header& hdr,
                                         std::span<const uint8_t> payload) {
    if (hdr.stream_id != 0) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::protocol_error));
      co_return false;
    }
    if (payload.size() != 8) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::frame_size_error));
      co_return false;
    }
    if (hdr.flags & flags::ACK)
      co_return true;
    std::array<uint8_t, 8> data;
    std::copy_n(payload.begin(), 8, data.begin());
    co_return co_await write_frame(make_ping_ack(data));
  }

  // --- Handler dispatch ---

  bool start_stream_dispatch(uint32_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end())
      return true;
    auto st = it->second;
    if (st->dispatch_started)
      return true;
    st->dispatch_started = true;
    pending_dispatch_streams_.push_back(stream_id);
    return true;
  }

  void flush_pending_dispatches() {
    if (pending_dispatch_streams_.empty())
      return;
    if (executor_ == nullptr)
      return;

    auto pending = std::move(pending_dispatch_streams_);
    pending_dispatch_streams_.clear();
    auto self = shared_from_this();
    for (auto stream_id : pending) {
      self->active_dispatches_.fetch_add(1, std::memory_order_acq_rel);
      [self, stream_id]() -> async_simple::coro::Lazy<void> {
        struct dispatch_guard {
          std::shared_ptr<coro_http2_connection> self;
          ~dispatch_guard() {
            self->active_dispatches_.fetch_sub(1, std::memory_order_acq_rel);
          }
        } guard{self};
        co_await self->dispatch_stream(stream_id);
      }()
                                 .via(self->executor_)
                                 .detach();
    }
  }

#ifdef CINATRA_ENABLE_SSL
  static int status_code_from_common_response(status_type status) {
    auto code = static_cast<int>(status);
    if (code > 0) {
      return code;
    }
    return static_cast<int>(status_type::not_implemented);
  }

  static void append_common_headers(std::vector<header_field>& target,
                                    const std::vector<resp_header>& headers) {
    for (auto& header : headers) {
      target.push_back(
          {ascii_lower_copy(header.key), std::string(header.value)});
    }
  }

  static void append_common_headers(std::vector<header_field>& target,
                                    std::span<const http_header> headers) {
    for (auto& header : headers) {
      target.push_back(
          {ascii_lower_copy(header.name), std::string(header.value)});
    }
  }

  static bool contains_header_case_insensitive(
      std::span<const header_field> headers, std::string_view name) {
    for (auto& header : headers) {
      if (ascii_iequals(header.name, name)) {
        return true;
      }
    }
    return false;
  }

  static h2_response make_h2_response(coro_http_response& resp) {
    h2_response translated;
    auto status = resp.status();
    translated.status_code = status_code_from_common_response(status);
    translated.body = std::string(resp.body_view());
    if (translated.body.empty() && !resp.has_explicit_content()) {
      translated.body = std::string(default_status_content(
          status == status_type::init ? status_type::not_implemented : status));
    }

    append_common_headers(translated.headers, resp.headers());
    append_common_headers(translated.headers, resp.header_span());
    append_common_headers(translated.trailers, resp.trailers());

    if (!resp.content_type_value().empty() &&
        !contains_header_case_insensitive(translated.headers, "content-type")) {
      translated.headers.push_back(
          {"content-type", std::string(resp.content_type_value())});
    }

    if (!contains_header_case_insensitive(translated.headers, "server")) {
      translated.headers.push_back({"server", "cinatra"});
    }

    for (auto& [_, cookie] : resp.cookies()) {
      translated.headers.push_back({"set-cookie", cookie.to_string()});
    }

    if (auto protocol_response = resp.protocol_response()) {
      for (auto& push : protocol_response->pushes) {
        auto& translated_push = translated.pushes.emplace_back();
        translated_push.method = push.method;
        translated_push.path = push.path;
        translated_push.scheme = push.scheme;
        translated_push.authority = push.authority;
        translated_push.status_code =
            status_code_from_common_response(push.status);
        translated_push.body = push.body;
        for (auto& header : push.request_headers) {
          translated_push.request_headers.push_back(
              {ascii_lower_copy(header.key), header.value});
        }
        for (auto& header : push.response_headers) {
          translated_push.response_headers.push_back(
              {ascii_lower_copy(header.key), header.value});
        }
        for (auto& trailer : push.response_trailers) {
          translated_push.response_trailers.push_back(
              {ascii_lower_copy(trailer.key), trailer.value});
        }
      }
    }

    return translated;
  }

  static void populate_common_request(const h2_request& source,
                                      coro_http_request& target) {
    auto& request = target.reset_protocol_request<h2_request_context>(nullptr);
    request.method_value = source.method;
    request.scheme_value = source.scheme;
    request.authority_value = source.authority;
    request.protocol_value = source.protocol;
    request.set_url(source.path);
    for (auto& header : source.headers) {
      request.add_header(header.name, header.value);
    }
    for (auto& trailer : source.trailers) {
      request.add_trailer(trailer.name, trailer.value);
    }
    request.set_body(source.body,
                     target.get_content_type() == content_type::urlencoded);
    if (source.needs_flow_control_probe_body) {
      target.set_user_data(
          common_request_metadata{.needs_flow_control_probe_body = true});
    }
  }
#endif

  async_simple::coro::Lazy<void> dispatch_stream(uint32_t stream_id) {
    // Look up stream - we hold a shared_ptr, so it stays alive even if
    // the read loop clears the map entry later.
    std::shared_ptr<stream_state> st;
    {
      auto it = streams_.find(stream_id);
      if (it == streams_.end())
        co_return;
      st = it->second;
    }

    if (st->state == stream_lifecycle::half_closed_remote &&
        st->req.body.empty() && st->req.trailers.empty()) {
      // Give the read loop a short chance to observe frames already queued
      // after END_STREAM. This keeps protocol errors on closed streams from
      // being hidden behind a normal response frame.
      co_await async_simple::coro::sleep(executor_,
                                         std::chrono::milliseconds(25));
      if (connection_closed_.load() || st->state == stream_lifecycle::closed) {
        st->dispatch_done = true;
        pending_cleanup_ = true;
        co_return;
      }
    }

    bool ok = false;
#ifdef CINATRA_ENABLE_SSL
    if (common_handler_) {
      coro_http_request req;
      populate_common_request(st->req, req);
      coro_http_response resp;
      try {
        co_await common_handler_(req, resp);
      } catch (...) {
        resp.set_status(status_type::internal_server_error);
      }
      if (req.has_session()) {
        auto session =
            session_manager::get().get_session(req.get_cached_session_id());
        if (session != nullptr && session->get_need_set_to_client()) {
          resp.add_cookie(session->get_session_cookie());
          session->set_need_set_to_client(false);
        }
      }
      ok = co_await send_response(stream_id, st, make_h2_response(resp));
    }
    else {
#endif
      h2_response resp;
      try {
        co_await h2_handler_(st->req, resp);
      } catch (...) {
        resp.set_status_and_body(500, "Internal Server Error");
      }
      ok = co_await send_response(stream_id, st, resp);
#ifdef CINATRA_ENABLE_SSL
    }
#endif
    // Mark for deferred cleanup by the read loop (avoids data race on
    // streams_ map).  The read loop calls cleanup_done_streams() after
    // every frame.
    st->dispatch_done = true;
    pending_cleanup_ = true;
    finish_graceful_shutdown();
    if (!ok && connection_closed_.load()) {
      std::error_code ignored;
      active_socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }
    co_return;
  }

  // --- Response serialization ---

  async_simple::coro::Lazy<bool> prepare_push_promises(
      uint32_t parent_stream_id, const h2_request& parent_req,
      const h2_response& resp,
      std::vector<prepared_push_response>& prepared_pushes) {
    if (!peer_enable_push_)
      co_return true;

    for (auto& push : resp.pushes) {
      if (streams_.size() >= peer_max_concurrent_streams_)
        break;

      std::vector<header_field> req_hdrs;
      std::string scheme =
          push.scheme.empty() ? parent_req.scheme : push.scheme;
      std::string authority =
          push.authority.empty() ? parent_req.authority : push.authority;
      req_hdrs.push_back({":method", push.method});
      req_hdrs.push_back({":path", push.path});
      req_hdrs.push_back({":scheme", scheme});
      req_hdrs.push_back({":authority", authority});
      for (auto& hf : push.request_headers) req_hdrs.push_back(hf);

      h2_response push_resp;
      push_resp.status_code = push.status_code;
      push_resp.body = push.body;
      push_resp.headers = push.response_headers;
      push_resp.trailers = push.response_trailers;
      if (!outbound_headers_within_peer_limit(req_hdrs) ||
          !response_header_blocks_within_peer_limit(push_resp)) {
        continue;
      }

      uint32_t promised_stream_id = next_push_stream_id_;
      next_push_stream_id_ += 2;
      if (!co_await write_push_promise_block(parent_stream_id,
                                             promised_stream_id, req_hdrs)) {
        co_return false;
      }

      auto push_stream = std::make_shared<stream_state>();
      push_stream->req.method = push.method;
      push_stream->req.path = push.path;
      push_stream->req.scheme = std::move(scheme);
      push_stream->req.authority = std::move(authority);
      push_stream->req.headers = push.request_headers;
      push_stream->state = stream_lifecycle::half_closed_remote;
      push_stream->initial_headers_received = true;
      push_stream->end_stream = true;
      push_stream->end_headers = true;
      push_stream->recv_window =
          static_cast<int32_t>(local_initial_window_size_);
      push_stream->send_window =
          static_cast<int32_t>(peer_initial_window_size_);
      streams_[promised_stream_id] = push_stream;
      ensure_priority_node(0);
      auto& push_node = ensure_priority_node(promised_stream_id);
      push_stream->priority.stream_dependency = push_node.parent_stream_id;
      push_stream->priority.weight = push_node.weight;
      prepared_pushes.push_back(
          {promised_stream_id, push_stream, std::move(push_resp)});
    }

    co_return true;
  }

  async_simple::coro::Lazy<bool> send_response(
      uint32_t stream_id, const std::shared_ptr<stream_state>& st_ptr,
      const h2_response& resp) {
    auto& st = *st_ptr;
    if (st.state == stream_lifecycle::closed || connection_closed_.load())
      co_return false;

    std::vector<prepared_push_response> prepared_pushes;
    if (!resp.pushes.empty() && !co_await prepare_push_promises(
                                    stream_id, st.req, resp, prepared_pushes)) {
      co_return false;
    }

    if (!response_header_blocks_within_peer_limit(resp)) {
      abort_stream(stream_id);
      co_return co_await write_frame(
          make_rst_stream(stream_id, h2_error_code::internal_error));
    }

    std::string status_value = integer_to_string(resp.status_code);
    std::string content_length_value;
    if (!resp.body.empty() && !has_header(resp.headers, "content-length"))
      content_length_value = integer_to_string(resp.body.size());
    auto resp_hdrs =
        build_response_header_views(resp, status_value, content_length_value);

    uint8_t hdr_flags = flags::END_HEADERS;
    if (resp.body.empty() && resp.trailers.empty())
      hdr_flags |= flags::END_STREAM;
    bool response_end_stream_sent = false;
    auto mark_response_end_stream = [&]() {
      if (response_end_stream_sent)
        return;
      response_end_stream_sent = true;
      mark_response_end_stream_sent(st);
    };

    if (hdr_flags & flags::END_STREAM)
      mark_response_end_stream();
    if (!co_await write_header_block(stream_id, resp_hdrs, hdr_flags)) {
      co_return false;
    }

    if (!resp.body.empty()) {
      struct send_registration {
        coro_http2_connection* self;
        uint32_t stream_id;
        ~send_registration() { self->unregister_send_candidate(stream_id); }
      } registration{this, stream_id};
      register_send_candidate(stream_id, st_ptr);
      co_await async_simple::coro::Yield{};
      size_t offset = 0;
      while (offset < resp.body.size()) {
        if (st.state == stream_lifecycle::closed || connection_closed_.load())
          co_return false;
        auto chunk = co_await reserve_send_window(stream_id, st,
                                                  resp.body.size() - offset);
        if (chunk == 0)
          co_return false;
        bool last =
            (offset + chunk == resp.body.size()) && resp.trailers.empty();
        auto span = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(resp.body.data()) + offset, chunk);
        if (!co_await write_data_frame(
                stream_id, last ? flags::END_STREAM : uint8_t(0), span)) {
          co_return false;
        }
        offset += chunk;
      }
    }

    if (!resp.trailers.empty()) {
      if (!co_await write_header_block(
              stream_id, resp.trailers,
              flags::END_HEADERS | flags::END_STREAM)) {
        co_return false;
      }
    }

    mark_response_end_stream();

    for (auto& prepared : prepared_pushes) {
      if (!co_await send_response(prepared.stream_id, prepared.stream,
                                  prepared.response)) {
        co_return false;
      }
      prepared.stream->dispatch_done = true;
      pending_cleanup_ = true;
    }

    co_return true;
  }

  void register_send_candidate(uint32_t stream_id,
                               const std::shared_ptr<stream_state>& st) {
    std::scoped_lock lock(send_schedule_mutex_);
    auto it = std::find_if(send_schedule_.begin(), send_schedule_.end(),
                           [stream_id](const send_schedule_entry& entry) {
                             return entry.stream_id == stream_id;
                           });
    if (it == send_schedule_.end())
      send_schedule_.push_back({stream_id, st});
  }

  void unregister_send_candidate(uint32_t stream_id) {
    std::scoped_lock lock(send_schedule_mutex_);
    send_schedule_.erase(
        std::remove_if(send_schedule_.begin(), send_schedule_.end(),
                       [stream_id](const send_schedule_entry& entry) {
                         return entry.stream_id == stream_id;
                       }),
        send_schedule_.end());
  }

  static bool response_fully_sent(const stream_state& st) {
    return st.state == stream_lifecycle::half_closed_local ||
           st.state == stream_lifecycle::closed;
  }

  void mark_response_end_stream_sent(stream_state& st) {
    st.state = st.state == stream_lifecycle::half_closed_remote
                   ? stream_lifecycle::closed
                   : stream_lifecycle::half_closed_local;
    send_window_cv_.notifyAll();
  }

  priority_node& ensure_priority_node(uint32_t stream_id) {
    auto [it, inserted] = priority_nodes_.try_emplace(stream_id);
    if (inserted) {
      it->second.stream_id = stream_id;
      if (stream_id != 0) {
        auto& root = ensure_priority_node(0);
        add_priority_child(root, stream_id);
      }
    }
    return it->second;
  }

  static void add_priority_child(priority_node& parent,
                                 uint32_t child_stream_id) {
    if (std::find(parent.children.begin(), parent.children.end(),
                  child_stream_id) == parent.children.end()) {
      parent.children.push_back(child_stream_id);
    }
  }

  void remove_priority_child(uint32_t parent_stream_id,
                             uint32_t child_stream_id) {
    auto it = priority_nodes_.find(parent_stream_id);
    if (it == priority_nodes_.end())
      return;
    auto& children = it->second.children;
    children.erase(
        std::remove(children.begin(), children.end(), child_stream_id),
        children.end());
  }

  bool priority_is_descendant(uint32_t ancestor_stream_id,
                              uint32_t stream_id) const {
    if (ancestor_stream_id == stream_id)
      return true;
    auto it = priority_nodes_.find(stream_id);
    while (it != priority_nodes_.end() && it->second.parent_stream_id != 0) {
      if (it->second.parent_stream_id == ancestor_stream_id)
        return true;
      it = priority_nodes_.find(it->second.parent_stream_id);
    }
    return false;
  }

  void apply_priority_spec(uint32_t stream_id, const priority_spec& spec) {
    if (stream_id == 0 || spec.stream_dependency == stream_id)
      return;

    ensure_priority_node(0);
    auto& node = ensure_priority_node(stream_id);
    uint32_t old_parent_stream_id = node.parent_stream_id;
    uint32_t new_parent_stream_id = spec.stream_dependency;
    auto& new_parent = ensure_priority_node(new_parent_stream_id);

    if (priority_is_descendant(stream_id, new_parent_stream_id)) {
      auto& descendant = ensure_priority_node(new_parent_stream_id);
      remove_priority_child(descendant.parent_stream_id, new_parent_stream_id);
      descendant.parent_stream_id = old_parent_stream_id;
      add_priority_child(ensure_priority_node(old_parent_stream_id),
                         new_parent_stream_id);
    }

    remove_priority_child(old_parent_stream_id, stream_id);
    node.parent_stream_id = new_parent_stream_id;
    node.weight = spec.weight;
    add_priority_child(new_parent, stream_id);

    if (spec.exclusive) {
      auto& attached_parent = ensure_priority_node(new_parent_stream_id);
      std::vector<uint32_t> adopted_children;
      adopted_children.reserve(attached_parent.children.size());
      for (auto child_stream_id : attached_parent.children) {
        if (child_stream_id != stream_id)
          adopted_children.push_back(child_stream_id);
      }
      attached_parent.children.erase(
          std::remove_if(attached_parent.children.begin(),
                         attached_parent.children.end(),
                         [stream_id](uint32_t child_stream_id) {
                           return child_stream_id != stream_id;
                         }),
          attached_parent.children.end());
      for (auto child_stream_id : adopted_children) {
        auto& child = ensure_priority_node(child_stream_id);
        child.parent_stream_id = stream_id;
        add_priority_child(node, child_stream_id);
      }
    }
  }

  static bool stream_ready_to_send(const stream_state& st) {
    return st.state != stream_lifecycle::closed && st.pending_send_bytes > 0 &&
           st.send_window > 0;
  }

  enum class priority_subtree_state {
    none,
    blocked,
    schedulable,
  };

  priority_subtree_state priority_subtree_state_unlocked(uint32_t stream_id) {
    auto node_it = priority_nodes_.find(stream_id);
    if (node_it == priority_nodes_.end())
      return priority_subtree_state::none;

    auto st_it = streams_.find(stream_id);
    if (st_it != streams_.end() && st_it->second) {
      auto& st = *st_it->second;
      if (!response_fully_sent(st))
        return stream_ready_to_send(st) ? priority_subtree_state::schedulable
                                        : priority_subtree_state::blocked;
    }

    bool saw_blocked_descendant = false;
    for (auto child_stream_id : node_it->second.children) {
      auto child_state = priority_subtree_state_unlocked(child_stream_id);
      if (child_state == priority_subtree_state::schedulable)
        return child_state;
      if (child_state == priority_subtree_state::blocked)
        saw_blocked_descendant = true;
    }
    return saw_blocked_descendant ? priority_subtree_state::blocked
                                  : priority_subtree_state::none;
  }

  uint32_t pick_schedulable_stream_unlocked(uint32_t stream_id) {
    auto node_it = priority_nodes_.find(stream_id);
    if (node_it == priority_nodes_.end())
      return 0;

    auto st_it = streams_.find(stream_id);
    if (st_it != streams_.end() && st_it->second) {
      auto& st = *st_it->second;
      if (!response_fully_sent(st))
        return stream_ready_to_send(st) ? stream_id : 0;
    }

    uint32_t best_child_stream_id = 0;
    uint8_t best_weight = 0;
    priority_subtree_state best_child_state = priority_subtree_state::none;
    for (auto child_stream_id : node_it->second.children) {
      auto child_state = priority_subtree_state_unlocked(child_stream_id);
      if (child_state == priority_subtree_state::none)
        continue;
      auto child_it = priority_nodes_.find(child_stream_id);
      uint8_t child_weight = child_it == priority_nodes_.end()
                                 ? uint8_t(15)
                                 : child_it->second.weight;
      if (best_child_stream_id == 0 || child_weight > best_weight ||
          (child_weight == best_weight && child_state > best_child_state) ||
          (child_weight == best_weight && child_state == best_child_state &&
           child_stream_id < best_child_stream_id)) {
        best_child_stream_id = child_stream_id;
        best_weight = child_weight;
        best_child_state = child_state;
      }
    }

    if (best_child_stream_id == 0 ||
        best_child_state != priority_subtree_state::schedulable) {
      return 0;
    }
    return pick_schedulable_stream_unlocked(best_child_stream_id);
  }

  bool can_schedule_stream(uint32_t stream_id, const stream_state& current) {
    if (current.state == stream_lifecycle::closed ||
        current.pending_send_bytes == 0 || current.send_window <= 0) {
      return false;
    }
    return pick_schedulable_stream_unlocked(0) == stream_id;
  }

  async_simple::coro::Lazy<size_t> reserve_send_window(uint32_t stream_id,
                                                       stream_state& st,
                                                       size_t remaining) {
    auto lock = co_await send_window_mutex_.coScopedLock();
    st.pending_send_bytes = remaining;
    co_await send_window_cv_.wait(send_window_mutex_, [&] {
      return connection_closed_.load() ||
             st.state == stream_lifecycle::closed ||
             (connection_send_window_ > 0 && st.send_window > 0 &&
              can_schedule_stream(stream_id, st));
    });
    if (connection_closed_.load() || st.state == stream_lifecycle::closed) {
      st.pending_send_bytes = 0;
      co_return 0;
    }

    auto chunk = std::min<size_t>(
        remaining,
        std::min<uint32_t>(
            std::min<uint32_t>(MAX_FRAME_SIZE, peer_max_frame_size_),
            std::min<uint32_t>(connection_send_window_, st.send_window)));
    connection_send_window_ -= static_cast<int32_t>(chunk);
    st.send_window -= static_cast<int32_t>(chunk);
    st.pending_send_bytes = remaining - chunk;
    send_window_cv_.notifyAll();
    co_return chunk;
  }

  // --- Utilities ---

  static payload_view_result strip_padded_payload(
      std::span<const uint8_t> payload) {
    uint8_t pad_len = 0;
    if (!payload.empty()) {
      pad_len = payload[0];
      payload = payload.subspan(1);
    }
    if (pad_len > payload.size()) {
      return {.ok = false, .error_code = h2_error_code::protocol_error};
    }
    if (pad_len > 0)
      payload = payload.subspan(0, payload.size() - pad_len);
    return {.ok = true, .payload = payload};
  }

  static payload_view_result strip_padding_priority(
      const frame_header& hdr, std::span<const uint8_t> payload) {
    if (hdr.flags & flags::PADDED) {
      if (payload.empty()) {
        return {.ok = false, .error_code = h2_error_code::protocol_error};
      }
      auto stripped = strip_padded_payload(payload);
      if (!stripped.ok)
        return stripped;
      payload = stripped.payload;
    }
    if (hdr.flags & flags::PRIORITY) {
      if (payload.size() < 5) {
        return {.ok = false, .error_code = h2_error_code::frame_size_error};
      }
      auto spec = parse_priority_payload(payload.first<5>());
      if (!spec.has_value()) {
        return {.ok = false, .error_code = h2_error_code::frame_size_error};
      }
      if (spec->stream_dependency == hdr.stream_id) {
        return {.ok = false,
                .stream_error = true,
                .error_code = h2_error_code::protocol_error};
      }
      payload = payload.subspan(5);
      return {.ok = true, .payload = payload, .priority = spec};
    }
    return {.ok = true, .payload = payload};
  }

  header_decode_result decode_headers(stream_state& st,
                                      header_block_kind block_kind) {
    try {
      auto decoded = decoder_.decode(st.hdr_block_buf);
      st.hdr_block_buf.clear();
      if (!validate_request_headers(decoded, st, block_kind))
        return header_decode_result::stream_error;
      return header_decode_result::ok;
    } catch (...) {
      pending_connection_error_ = h2_error_code::compression_error;
      return header_decode_result::connection_error;
    }
  }

  bool validate_request_headers(std::span<const header_field> decoded,
                                stream_state& st,
                                header_block_kind block_kind) {
    if (block_kind == header_block_kind::trailer) {
      for (auto& hf : decoded) {
        if (contains_invalid_header_name(hf.name) ||
            contains_invalid_header_value(hf.value)) {
          return false;
        }
        if (!hf.name.empty() && hf.name.front() == ':')
          return false;
        if (!validate_request_regular_header(hf.name, hf.value, block_kind))
          return false;
        st.req.trailers.push_back(hf);
      }
      return true;
    }

    bool seen_regular = false;
    bool seen_method = false;
    bool seen_path = false;
    bool seen_scheme = false;
    bool seen_authority = false;
    bool seen_protocol = false;

    st.req = {};
    st.content_length.reset();

    for (auto& hf : decoded) {
      if (contains_invalid_header_name(hf.name) ||
          contains_invalid_header_value(hf.value)) {
        return false;
      }

      bool pseudo = !hf.name.empty() && hf.name.front() == ':';
      if (pseudo) {
        if (seen_regular)
          return false;

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
        else if (hf.name == ":protocol") {
          if (seen_protocol)
            return false;
          seen_protocol = true;
          st.req.protocol = hf.value;
        }
        else {
          return false;
        }
        continue;
      }

      seen_regular = true;
      if (!validate_request_regular_header(hf.name, hf.value, block_kind))
        return false;

      if (hf.name == "content-length") {
        if (st.content_length.has_value())
          return false;
        auto len = parse_content_length(hf.value);
        if (!len.has_value())
          return false;
        st.content_length = *len;
      }
      st.req.headers.push_back(hf);
    }

    if (!seen_method)
      return false;

    bool regular_connect = st.req.method == "CONNECT" && !seen_protocol;
    bool extended_connect = seen_protocol;

    if (extended_connect) {
      if (!enable_connect_protocol_ || st.req.method != "CONNECT" ||
          !seen_scheme || !seen_path || !seen_authority) {
        return false;
      }
    }
    else if (regular_connect) {
      if (seen_scheme || seen_path || !seen_authority) {
        return false;
      }
    }
    else if (!seen_path || !seen_scheme) {
      return false;
    }

    if (seen_path && st.req.path.empty())
      return false;
    if (seen_path && st.req.path == "*" && st.req.method != "OPTIONS")
      return false;
    if (st.end_stream && st.content_length.has_value() &&
        *st.content_length != 0)
      return false;
    return true;
  }

  async_simple::coro::Lazy<flow_control_result> consume_flow_control(
      uint32_t stream_id, uint32_t amount, stream_state& st) {
    if (amount > static_cast<uint32_t>(connection_recv_window_)) {
      co_await write_frame(
          make_goaway(last_stream_id_, h2_error_code::flow_control_error));
      co_return flow_control_result::connection_error;
    }
    if (amount > static_cast<uint32_t>(st.recv_window)) {
      co_await write_frame(
          make_rst_stream(stream_id, h2_error_code::flow_control_error));
      streams_.erase(stream_id);
      co_return flow_control_result::stream_error;
    }

    connection_recv_window_ -= static_cast<int32_t>(amount);
    st.recv_window -= static_cast<int32_t>(amount);
    co_return flow_control_result::ok;
  }

  // Accumulate consumed bytes and send WINDOW_UPDATE when a threshold is
  // crossed (half of the initial window). Zero-length DATA produces no
  // WINDOW_UPDATE (increment 0 is a protocol error per RFC 7540 section 6.9).
  // When stream_done is true (END_STREAM), no stream-level WINDOW_UPDATE
  // is sent since the peer will send no more DATA for this stream.
  async_simple::coro::Lazy<bool> refill_flow_control(uint32_t stream_id,
                                                     stream_state& st,
                                                     uint32_t amount,
                                                     bool stream_done) {
    if (amount == 0)
      co_return true;
    connection_recv_pending_ += amount;

    uint32_t threshold = local_initial_window_size_ / 2;
    if (threshold == 0)
      threshold = 1;
    if (connection_recv_pending_ >= threshold) {
      uint32_t inc = connection_recv_pending_;
      connection_recv_pending_ = 0;
      connection_recv_window_ += static_cast<int32_t>(inc);
      if (!co_await write_frame(make_window_update(0, inc)))
        co_return false;
    }
    if (stream_done) {
      // Peer won't send more on this stream; just absorb locally.
      st.recv_window += static_cast<int32_t>(amount);
      co_return true;
    }
    st.recv_pending += amount;
    if (st.recv_pending >= threshold) {
      uint32_t inc = st.recv_pending;
      st.recv_pending = 0;
      st.recv_window += static_cast<int32_t>(inc);
      if (!co_await write_frame(make_window_update(stream_id, inc)))
        co_return false;
    }
    co_return true;
  }

  void notify_quit() {
    if (quit_callback_)
      quit_callback_();
  }

  void close_send_waiters() {
    connection_closed_ = true;
    send_window_cv_.notifyAll();
  }

  async_simple::coro::Lazy<void> wait_for_dispatches() {
    if (executor_ == nullptr)
      co_return;

    constexpr auto drain_step = std::chrono::milliseconds(5);
    constexpr auto drain_timeout = std::chrono::seconds(2);
    auto deadline = std::chrono::steady_clock::now() + drain_timeout;
    while (active_dispatches_.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
      co_await async_simple::coro::sleep(executor_, drain_step);
      cleanup_done_streams();
    }
  }

  // Called from the read loop to erase streams that dispatch_stream has
  // finished processing.  This keeps streams_ modifications on the read
  // coroutine, eliminating data races.
  void cleanup_done_streams() {
    if (!pending_cleanup_)
      return;
    pending_cleanup_ = false;
    for (auto it = streams_.begin(); it != streams_.end();) {
      if (it->second->dispatch_done) {
        remember_closed_stream(it->first);
        it = streams_.erase(it);
      }
      else
        ++it;
    }
  }

  size_t active_remote_stream_count() const {
    size_t count = 0;
    for (auto& [stream_id, st] : streams_) {
      if ((stream_id & 1u) != 0 && st &&
          st->state != stream_lifecycle::closed && !st->dispatch_done) {
        ++count;
      }
    }
    return count;
  }

  bool remote_stream_limit_reached() const {
    size_t active_count = active_remote_stream_count();
    size_t burst_count = remote_streams_seen_since_input_idle_;
    return std::max(active_count, burst_count) >= local_max_concurrent_streams_;
  }

  void note_remote_stream_opened() {
    if (remote_streams_seen_since_input_idle_ <
        std::numeric_limits<uint32_t>::max()) {
      ++remote_streams_seen_since_input_idle_;
    }
  }

  void reset_remote_stream_burst_to_active() {
    auto active_count = active_remote_stream_count();
    remote_streams_seen_since_input_idle_ =
        active_count > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(active_count);
  }

  bool has_buffered_input() {
    if (!prefetched_bytes_.empty())
      return true;
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      auto* ssl_stream = active_ssl_stream();
      if (ssl_stream != nullptr &&
          ::SSL_pending(ssl_stream->native_handle()) > 0) {
        return true;
      }
    }
#endif
    std::error_code ec;
    auto& socket = active_socket();
    return socket.is_open() && socket.available(ec) > 0;
  }

  void abort_stream(uint32_t stream_id) {
    if (auto it = streams_.find(stream_id); it != streams_.end()) {
      it->second->state = stream_lifecycle::closed;
      remember_closed_stream(stream_id);
      streams_.erase(it);
      send_window_cv_.notifyAll();
    }
  }

  bool has_active_streams() const {
    for (auto& [id, st] : streams_) {
      if (!st->dispatch_done)
        return true;
    }
    return false;
  }

  void finish_graceful_shutdown() {
    if (!going_away_ || connection_closed_.load() || has_active_streams())
      return;
    if (peer_sent_goaway_ || !graceful_wait_for_peer_close_) {
      force_close();
      return;
    }
    close_send_waiters();
    std::error_code ignored;
    active_socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
  }

  static bool has_header(std::span<const header_field> headers,
                         std::string_view name) {
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

  static size_t decimal_digit_count(uint64_t value) noexcept {
    size_t digits = 1;
    while (value >= 10) {
      value /= 10;
      ++digits;
    }
    return digits;
  }

  static bool add_header_list_size(uint64_t& total, size_t name_size,
                                   size_t value_size,
                                   uint32_t max_header_list_size) noexcept {
    total += static_cast<uint64_t>(name_size) +
             static_cast<uint64_t>(value_size) + 32;
    return total <= max_header_list_size;
  }

  bool outbound_headers_within_peer_limit(
      std::span<const header_field> headers) const {
    return header_list_size_within_limit(headers, peer_max_header_list_size_);
  }

  bool outbound_headers_within_peer_limit(
      std::span<const header_field_view> headers) const {
    return header_list_size_within_limit(headers, peer_max_header_list_size_);
  }

  static std::vector<header_field_view> build_response_header_views(
      const h2_response& resp, std::string_view status_value,
      std::string_view content_length_value) {
    std::vector<header_field_view> resp_hdrs;
    resp_hdrs.reserve(1 + resp.headers.size() +
                      (content_length_value.empty() ? 0 : 1));
    resp_hdrs.push_back({":status", status_value});
    if (!content_length_value.empty())
      resp_hdrs.push_back({"content-length", content_length_value});
    for (auto& hf : resp.headers) resp_hdrs.push_back({hf.name, hf.value});
    return resp_hdrs;
  }

  bool response_header_blocks_within_peer_limit(const h2_response& resp) const {
    uint64_t total = 0;
    if (!add_header_list_size(total, std::string_view(":status").size(),
                              decimal_digit_count(resp.status_code),
                              peer_max_header_list_size_)) {
      return false;
    }
    if (!resp.body.empty() && !has_header(resp.headers, "content-length") &&
        !add_header_list_size(total, std::string_view("content-length").size(),
                              decimal_digit_count(resp.body.size()),
                              peer_max_header_list_size_)) {
      return false;
    }
    for (auto& hf : resp.headers) {
      if (!add_header_list_size(total, hf.name.size(), hf.value.size(),
                                peer_max_header_list_size_)) {
        return false;
      }
    }
    return outbound_headers_within_peer_limit(resp.trailers);
  }

#ifdef CINATRA_ENABLE_SSL
  asio::ssl::stream<asio::ip::tcp::socket&>* active_ssl_stream() noexcept {
    return external_ssl_stream_ != nullptr ? external_ssl_stream_
                                           : ssl_stream_.get();
  }
#endif

  asio::ip::tcp::socket& active_socket() noexcept {
#ifdef CINATRA_ENABLE_SSL
    if (use_ssl_) {
      if (auto* ssl_stream = active_ssl_stream(); ssl_stream != nullptr) {
        return ssl_stream->next_layer();
      }
    }
#endif
    return socket_;
  }

  // --- Members ---

  asio::ip::tcp::socket socket_;
  h2_handler h2_handler_;
#ifdef CINATRA_ENABLE_SSL
  common_http_handler common_handler_;
#endif
  std::function<void()> quit_callback_;
  async_simple::coro::Mutex write_mutex_;
  async_simple::coro::Mutex header_mutex_;
  async_simple::coro::Mutex send_window_mutex_;
  static constexpr size_t closed_stream_history_limit_ = 256;
  std::mutex send_schedule_mutex_;
  async_simple::coro::ConditionVariable<async_simple::coro::Mutex>
      send_window_cv_;
  async_simple::Executor* executor_ = nullptr;
  hpack_decoder decoder_;
  hpack_encoder encoder_;
  std::unordered_map<uint32_t, std::shared_ptr<stream_state>> streams_;
  std::unordered_set<uint32_t> closed_stream_ids_;
  std::deque<uint32_t> closed_stream_order_;
  std::unordered_map<uint32_t, priority_node> priority_nodes_;
  uint32_t last_stream_id_ = 0;
  uint32_t next_push_stream_id_ = 2;
  uint32_t pending_continuation_stream_ = 0;
  uint32_t local_initial_window_size_ = DEFAULT_WINDOW_SIZE;
  uint32_t local_max_concurrent_streams_ = DEFAULT_MAX_CONCURRENT_STREAMS;
  uint32_t peer_initial_window_size_ = DEFAULT_WINDOW_SIZE;
  uint32_t peer_max_frame_size_ = MAX_FRAME_SIZE;
  uint32_t peer_max_concurrent_streams_ = DEFAULT_MAX_CONCURRENT_STREAMS;
  uint32_t peer_max_header_list_size_ = std::numeric_limits<uint32_t>::max();
  bool peer_enable_push_ = true;
  bool peer_enable_connect_protocol_ = false;
  bool peer_settings_received_ = false;
  uint32_t pending_upgrade_stream_id_ = 0;
  uint32_t remote_streams_seen_since_input_idle_ = 0;
  int32_t connection_send_window_ = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
  int32_t connection_recv_window_ = static_cast<int32_t>(DEFAULT_WINDOW_SIZE);
  uint32_t connection_recv_pending_ = 0;
  std::atomic<bool> connection_closed_ = false;
  std::atomic<uint32_t> active_dispatches_ = 0;
  bool going_away_ = false;
  bool peer_sent_goaway_ = false;
  bool graceful_wait_for_peer_close_ = false;
  bool enable_connect_protocol_ = false;
  bool pending_cleanup_ = false;
  std::optional<h2_error_code> pending_connection_error_;
  std::vector<uint8_t> prefetched_bytes_;
  std::vector<uint8_t> payload_buf_;
  std::vector<uint32_t> pending_dispatch_streams_;
  std::vector<send_schedule_entry> send_schedule_;
  std::shared_ptr<coro_http2_connection> lifetime_guard_;
#ifdef CINATRA_ENABLE_SSL
  std::unique_ptr<asio::ssl::context> ssl_ctx_ = nullptr;
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_;
  asio::ssl::stream<asio::ip::tcp::socket&>* external_ssl_stream_ = nullptr;
  bool use_ssl_ = false;
  bool ssl_handshake_done_ = false;
#endif
};

}  // namespace cinatra::http2
