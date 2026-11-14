#include <asio/streambuf.hpp>
#include <filesystem>
#include <future>
#include <string>

#include "cinatra/http_header_matcher.hpp"
#include "cinatra/ylt/coro_io/coro_io.hpp"

namespace websocket_test_io {
inline thread_local std::string *initial_read = nullptr;
}

// Fix the upgrade read's contents without depending on packet boundaries.
namespace coro_io {
template <typename Socket>
async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_until(
    Socket &socket, asio::streambuf &buffer,
    cinatra::detail::http_header_matcher matcher) {
  auto *initial = websocket_test_io::initial_read;
  if (initial && !initial->empty()) {
    auto size = initial->find("\r\n\r\n") + 4;
    buffer.sputn(initial->data(), initial->size());
    initial->clear();
    co_return std::make_pair(std::error_code{}, size);
  }
  co_return co_await coro_io::async_read_until<
      Socket, asio::streambuf, cinatra::detail::http_header_matcher>(
      socket, buffer, matcher);
}
}  // namespace coro_io

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

using namespace cinatra;
using namespace std::chrono_literals;
using async_simple::coro::Lazy;

namespace {
std::string masked_frame(std::string data) {
  websocket encoder;
  std::span<char> payload(data);
  std::string header(encoder.encode_frame(payload, opcode::text, true));
  return header + data;
}

template <typename Stream>
Lazy<std::string> take_bytes(Stream &stream, asio::streambuf &input,
                             size_t size) {
  if (input.size() < size) {
    auto [ec, _] =
        co_await coro_io::async_read(stream, input, size - input.size());
    if (ec) {
      throw std::system_error(ec);
    }
  }
  std::string bytes(asio::buffer_cast<const char *>(input.data()), size);
  input.consume(size);
  co_return bytes;
}

template <typename Stream>
Lazy<std::string> take_frame(Stream &stream, asio::streambuf &input) {
  auto header = co_await take_bytes(stream, input, 2);
  REQUIRE((static_cast<unsigned char>(header[1]) & 0x80) == 0);
  size_t size = header[1] & 0x7f;
  if (size >= 126) {
    auto length = co_await take_bytes(stream, input, size == 126 ? 2 : 8);
    size = 0;
    for (unsigned char byte : length) {
      size = (size << 8) | byte;
    }
  }
  co_return co_await take_bytes(stream, input, size);
}

struct websocket_test {
  asio::io_context context{1};
  coro_io::ExecutorWrapper<> executor{context.get_executor()};
  coro_http_router router;
  asio::ip::tcp::socket peer{context};
  std::shared_ptr<coro_http_connection> connection;
  bool tls;

  explicit websocket_test(bool use_tls) : tls(use_tls) {
    using asio::ip::tcp;
    tcp::acceptor listener(context, {asio::ip::address_v4::loopback(), 0});
    peer.open(tcp::v4());
    peer.set_option(asio::socket_base::receive_buffer_size(1024));
    peer.connect(listener.local_endpoint());
    connection = std::make_shared<coro_http_connection>(
        &executor, listener.accept(), router);
    connection->tcp_socket().set_option(
        asio::socket_base::send_buffer_size(1024));
#ifdef CINATRA_ENABLE_SSL
    if (tls) {
      const auto dir =
          std::filesystem::path(__FILE__).parent_path().parent_path() /
          "include/cinatra";
      REQUIRE(connection->init_ssl((dir / "server.crt").string(),
                                   (dir / "server.key").string(), "test"));
    }
#endif
  }

  template <typename Stream, typename Client>
  void run(Stream &stream, std::string frames, Client client) {
    std::string initial =
        "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n" +
        frames;
    websocket_test_io::initial_read = &initial;
    struct reset_read {
      ~reset_read() { websocket_test_io::initial_read = nullptr; }
    } reset;
    bool client_done = false;
    bool server_done = false;
    bool timed_out = false;
    asio::steady_timer deadline(context);
    auto finish = [&] {
      if (client_done && server_done) {
        deadline.cancel();
      }
    };
    deadline.expires_after(10s);
    deadline.async_wait([&](std::error_code ec) {
      if (!ec) {
        timed_out = true;
        connection->close();
        peer.close(ec);
      }
    });
    connection->start().via(&executor).start([&](auto &&result) {
      CHECK_FALSE(result.hasError());
      server_done = true;
      finish();
    });
    auto run_client = [&]() -> Lazy<void> {
#ifdef CINATRA_ENABLE_SSL
      if constexpr (requires { stream.next_layer(); }) {
        auto *ssl_stream = &stream;
        auto ec = co_await coro_io::async_handshake(
            ssl_stream, asio::ssl::stream_base::client);
        if (ec) {
          throw std::system_error(ec);
        }
      }
#endif
      asio::streambuf input;
      auto [ec, size] =
          co_await coro_io::async_read_until(stream, input, TWO_CRCF);
      if (ec) {
        throw std::system_error(ec);
      }
      CHECK(
          std::string_view(asio::buffer_cast<const char *>(input.data()), size)
              .starts_with("HTTP/1.1 101"));
      input.consume(size);
      co_await client(stream, input);
    };
    run_client().via(&executor).start([&](auto &&result) {
      if (result.hasError()) {
        try {
          std::rethrow_exception(result.getException());
        } catch (const std::exception &e) {
          FAIL_CHECK(e.what());
        }
      }
      client_done = true;
      std::error_code ec;
      peer.close(ec);
      connection->close();
      finish();
    });
    context.run();
    CHECK_FALSE(timed_out);
    CHECK(client_done);
    CHECK(server_done);
    CHECK(connection->has_closed());
  }

  template <typename Client>
  void check(std::string frames, Client client) {
#ifdef CINATRA_ENABLE_SSL
    if (tls) {
      asio::ssl::context ssl_context(asio::ssl::context::tls_client);
      asio::ssl::stream<asio::ip::tcp::socket &> stream(peer, ssl_context);
      run(stream, std::move(frames), client);
      return;
    }
#endif
    run(peer, std::move(frames), client);
  }
};
}  // namespace

TEST_CASE("websocket preserves frames prefetched with the upgrade") {
  bool tls = false;
  bool multi_buffer = true;
  SUBCASE("TCP buffers") {}
  SUBCASE("TCP string") { multi_buffer = false; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS buffers") { tls = true; }
  SUBCASE("TLS string") {
    tls = true;
    multi_buffer = false;
  }
#endif
  websocket_test test(tls);
  test.connection->set_multi_buf(multi_buffer);
  size_t received = 0;
  test.router.set_http_handler<GET>(
      "/ws", [&](coro_http_request &req, coro_http_response &) -> Lazy<void> {
        for (const auto &expected : {"first", "second"}) {
          auto result = co_await req.get_conn()->read_websocket();
          CHECK_FALSE(result.ec);
          if (result.ec) {
            co_return;
          }
          CHECK(result.data == expected);
          ++received;
          auto ec = co_await req.get_conn()->write_websocket(result.data);
          CHECK_FALSE(ec);
        }
      });
  test.check(masked_frame("first") + masked_frame("second"),
             [&](auto &stream, asio::streambuf &input) -> Lazy<void> {
               // No additional frame bytes can arrive. Before buffering this
               // EOF deterministically wins over the already-prefetched data.
               test.peer.shutdown(asio::ip::tcp::socket::shutdown_send);
               auto first = co_await take_frame(stream, input);
               CHECK(first == "first");
               auto second = co_await take_frame(stream, input);
               CHECK(second == "second");
             });
  CHECK(received == 2);
}

#ifndef CINATRA_WS_RECEIVE_REGRESSION_ONLY
TEST_CASE(
    "websocket batches own bytes across suspended writes and worker work") {
  bool tls = false;
  bool cancel = false;
  SUBCASE("TCP") {}
  SUBCASE("TCP close during flush") { cancel = true; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS") { tls = true; }
  SUBCASE("TLS close during flush") {
    tls = true;
    cancel = true;
  }
#endif
  websocket_test test(tls);
  bool flush_started = false;
  bool flush_done = false;
  test.router.set_http_handler<GET>(
      "/ws", [&](coro_http_request &req, coro_http_response &) -> Lazy<void> {
        auto *conn = req.get_conn();
        conn->set_websocket_auto_flush(false);
        {
          std::string first(30000, 'a');
          auto ec = co_await conn->write_websocket(first);
          CHECK_FALSE(ec);
          first.assign(first.size(), 'x');
          std::string second(30001, 'b');
          ec = co_await conn->write_websocket(second, opcode::binary);
          CHECK_FALSE(ec);
          second.assign(second.size(), 'y');
        }
        co_await coro_io::post([] {
        });
        CHECK(test.context.get_executor().running_in_this_thread());
        flush_started = true;
        auto ec = co_await conn->flush_websocket();
        flush_done = true;
        CHECK(test.context.get_executor().running_in_this_thread());
        if (cancel) {
          CHECK(ec);
          co_return;
        }
        CHECK_FALSE(ec);
        auto result = co_await conn->read_websocket();
        CHECK(result.type == ws_frame_type::WS_CLOSE_FRAME);
      });
  test.check({}, [&](auto &stream, asio::streambuf &input) -> Lazy<void> {
    // The receive window and send buffer force flow control. Release it only
    // after observing a prefix while the owning flush is still suspended.
    auto prefix = co_await take_bytes(stream, input, 7);
    CHECK(flush_started);
    CHECK_FALSE(flush_done);
    if (cancel) {
      test.connection->close();
      co_return;
    }
    CHECK(prefix.substr(0, 4) == std::string("\x81\x7e\x75\x30", 4));
    auto first = prefix.substr(4);
    first += co_await take_bytes(stream, input, 30000 - first.size());
    CHECK(first == std::string(30000, 'a'));
    auto second = co_await take_frame(stream, input);
    CHECK(second == std::string(30001, 'b'));
    websocket encoder;
    auto payload = encoder.format_close_payload(close_code::normal, nullptr, 0);
    std::span<char> close_data(payload);
    std::string close_wire(
        encoder.encode_frame(close_data, opcode::close, true));
    close_wire += payload;
    auto [ec, _] =
        co_await coro_io::async_write(stream, asio::buffer(close_wire));
    CHECK_FALSE(ec);
    auto close_reply = co_await take_frame(stream, input);
    CHECK(close_reply.size() == 2);
  });
  CHECK(flush_done);
}

TEST_CASE("websocket batching resumes on a shared server executor") {
  bool tls = false;
  SUBCASE("TCP") {}
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS") { tls = true; }
#endif
  asio::io_context context(1);
  coro_io::ExecutorWrapper<> executor(context.get_executor());
  coro_http_server server(context, 0, "127.0.0.1");
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    const auto dir =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "include/cinatra";
    server.init_ssl((dir / "server.crt").string(),
                    (dir / "server.key").string(), "test");
  }
#endif
  size_t received = 0;
  server.set_http_handler<GET>(
      "/ws", [&](coro_http_request &req, coro_http_response &) -> Lazy<void> {
        auto *conn = req.get_conn();
        conn->set_websocket_auto_flush(false);
        while (true) {
          auto result = co_await conn->read_websocket();
          if (result.ec || result.type == ws_frame_type::WS_CLOSE_FRAME) {
            co_return;
          }
          co_await coro_io::post([] {
          });
          CHECK(context.get_executor().running_in_this_thread());
          std::string copy(result.data);
          auto ec = co_await conn->write_websocket(copy);
          CHECK_FALSE(ec);
          copy.assign(copy.size(), 'x');
          ++received;
        }
      });
  auto serving = server.async_start();
  coro_http_client client(context.get_executor());
  const auto uri = std::string(tls ? "wss" : "ws") +
                   "://127.0.0.1:" + std::to_string(server.port()) + "/ws";
  bool timed_out = false;
  std::exception_ptr error;
  std::promise<void> done;
  auto completion = done.get_future();
  asio::steady_timer deadline(context);
  deadline.expires_after(10s);
  deadline.async_wait([&](std::error_code ec) {
    if (!ec) {
      timed_out = true;
      client.close();
    }
  });
  auto requests = [&]() -> Lazy<void> {
    auto upgrade = co_await client.connect(uri);
    REQUIRE_FALSE(upgrade.net_err);
    for (const auto &message : {"first", "second"}) {
      auto write = co_await client.write_websocket(message);
      REQUIRE_FALSE(write.net_err);
      auto response = co_await client.read_websocket();
      REQUIRE_FALSE(response.net_err);
      CHECK(response.resp_body == message);
    }
  };
  requests().via(&executor).start([&](auto &&result) {
    if (result.hasError()) {
      error = result.getException();
    }
    client.close();
    deadline.cancel();
    done.set_value();
  });
  std::thread runner([&] {
    context.run();
  });
  completion.wait();
  server.stop();
  runner.join();
  CHECK_FALSE(timed_out);
  CHECK(received == 2);
  if (error) {
    std::rethrow_exception(error);
  }
}
#endif
