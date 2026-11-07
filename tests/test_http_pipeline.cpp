#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

using namespace cinatra;
using namespace std::chrono_literals;

namespace {
std::string request_for(std::string_view value) {
  return "GET /pipeline?n=" + std::string(value) +
         " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

template <typename Stream>
async_simple::coro::Lazy<std::string> read_response(Stream &stream,
                                                    asio::streambuf &buffer) {
  auto [ec, size] =
      co_await coro_io::async_read_until(stream, buffer, TWO_CRCF);
  if (ec) {
    throw std::system_error(ec);
  }
  http_parser parser;
  auto data = asio::buffer_cast<const char *>(buffer.data());
  REQUIRE(parser.parse_response(data, size, 0) > 0);
  CHECK(parser.status() == 200);
  auto body_size = static_cast<size_t>(parser.body_len());
  buffer.consume(size);
  if (buffer.size() < body_size) {
    auto [body_ec, _] =
        co_await coro_io::async_read(stream, buffer, body_size - buffer.size());
    if (body_ec) {
      throw std::system_error(body_ec);
    }
  }
  std::string body(asio::buffer_cast<const char *>(buffer.data()), body_size);
  buffer.consume(body_size);
  co_return body;
}

struct pipeline_test {
  asio::io_context context{1};
  coro_io::ExecutorWrapper<> executor{context.get_executor()};
  coro_http_router router;
  asio::ip::tcp::socket peer{context};
  std::shared_ptr<coro_http_connection> connection;
  bool tls;
  size_t handled = 0;

  pipeline_test(bool use_tls, bool suspend_handler) : tls(use_tls) {
    using asio::ip::tcp;
    tcp::acceptor listener(context, {asio::ip::address_v4::loopback(), 0});
    peer.connect(listener.local_endpoint());
    connection = std::make_shared<coro_http_connection>(
        &executor, listener.accept(), router);
#ifdef CINATRA_ENABLE_SSL
    if (tls) {
      const auto dir =
          std::filesystem::path(__FILE__).parent_path().parent_path() /
          "include/cinatra";
      REQUIRE(connection->init_ssl((dir / "server.crt").string(),
                                   (dir / "server.key").string(), "test"));
    }
#endif
    if (suspend_handler) {
      router.set_http_handler<GET>(
          "/pipeline",
          [this](coro_http_request &req,
                 coro_http_response &resp) -> async_simple::coro::Lazy<void> {
            auto value = req.get_query_value("n");
            co_await coro_io::post([] {
            });
            CHECK(context.get_executor().running_in_this_thread());
            resp.set_status_and_content(status_type::ok, std::string(value));
            ++handled;
          });
    }
    else {
      router.set_http_handler<GET>(
          "/pipeline",
          [this](coro_http_request &req, coro_http_response &resp) {
            resp.set_status_and_content(status_type::ok,
                                        std::string(req.get_query_value("n")));
            ++handled;
          });
    }
  }

  template <typename Stream>
  void run(Stream &stream, const std::string &prefix, const std::string &suffix,
           const std::vector<std::string> &expected,
           size_t replies_before_suffix) {
    // Queue the entire first fragment before starting any TCP reads. Its size
    // in the split-header cases is below the initial 512-byte read buffer.
    if (!tls) {
      asio::write(peer, asio::buffer(prefix));
    }
    bool server_done = false;
    connection->start().via(&executor).start([&](auto &&result) {
      CHECK_FALSE(result.hasError());
      server_done = true;
    });
    asio::steady_timer deadline(context);
    deadline.expires_after(10s);
    deadline.async_wait([&](std::error_code ec) {
      if (!ec) {
        connection->close();
        peer.close(ec);
      }
    });
    auto client = [&]() -> async_simple::coro::Lazy<void> {
#ifdef CINATRA_ENABLE_SSL
      if constexpr (requires { stream.next_layer(); }) {
        auto *ssl_stream = &stream;
        auto ec = co_await coro_io::async_handshake(
            ssl_stream, asio::ssl::stream_base::client);
        if (ec) {
          throw std::system_error(ec);
        }
        auto [write_ec, _] =
            co_await coro_io::async_write(stream, asio::buffer(prefix));
        if (write_ec) {
          throw std::system_error(write_ec);
        }
      }
#endif
      asio::streambuf received;
      for (size_t i = 0; i < expected.size(); ++i) {
        if (i == replies_before_suffix && !suffix.empty()) {
          // The preceding replies synchronize with the server's pipeline
          // drain. Deliver the unfinished request only after both arrive.
          auto [ec, _] =
              co_await coro_io::async_write(stream, asio::buffer(suffix));
          if (ec) {
            throw std::system_error(ec);
          }
        }
        auto body = co_await read_response(stream, received);
        CHECK(body == expected[i]);
      }
    };
    bool client_done = false;
    client().via(&executor).start([&](auto &&result) {
      if (result.hasError()) {
        try {
          std::rethrow_exception(result.getException());
        } catch (const std::exception &e) {
          FAIL_CHECK(e.what());
        }
      }
      client_done = true;
      deadline.cancel();
      std::error_code ec;
      peer.close(ec);
    });
    context.run();
    CHECK(client_done);
    CHECK(server_done);
    CHECK(handled == expected.size());
    connection->close();
    context.restart();
    context.poll();
  }

  void check(const std::string &prefix, const std::string &suffix,
             const std::vector<std::string> &expected,
             size_t replies_before_suffix) {
#ifdef CINATRA_ENABLE_SSL
    if (tls) {
      asio::ssl::context ssl_context(asio::ssl::context::tls_client);
      asio::ssl::stream<asio::ip::tcp::socket &> stream(peer, ssl_context);
      run(stream, prefix, suffix, expected, replies_before_suffix);
      return;
    }
#endif
    run(peer, prefix, suffix, expected, replies_before_suffix);
  }
};
}  // namespace

TEST_CASE("pipeline preserves incomplete request tails") {
  bool tls = false;
  bool suspend_handler = false;
  SUBCASE("TCP") {}
  SUBCASE("TCP with suspended handlers") { suspend_handler = true; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS") { tls = true; }
  SUBCASE("TLS with suspended handlers") {
    tls = true;
    suspend_handler = true;
  }
#endif
  const auto next = request_for("third");
  for (size_t split = 1; split < next.size(); ++split) {
    CAPTURE(split);
    pipeline_test test(tls, suspend_handler);
    test.check(
        request_for("first") + request_for("second") + next.substr(0, split),
        next.substr(split) + request_for("fourth"),
        {"first", "second", "third", "fourth"}, 2);
  }
}

TEST_CASE("pipeline handles batches spanning read buffers") {
  pipeline_test test(false, false);
  std::string batch;
  std::vector<std::string> expected;
  for (size_t i = 0; i < 16; ++i) {
    expected.push_back(std::to_string(i));
    batch += request_for(expected.back());
  }
  test.check(batch, "", expected, expected.size());
}
