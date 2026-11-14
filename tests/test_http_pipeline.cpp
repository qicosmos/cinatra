#include <asio/streambuf.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cinatra/http_header_matcher.hpp"
#include "cinatra/ylt/coro_io/coro_io.hpp"

namespace pipeline_test_io {
struct read_control {
  std::string initial_data;
  asio::streambuf *buffer = nullptr;

  std::string_view buffered() const {
    return {static_cast<const char *>(buffer->data().data()), buffer->size()};
  }
};
inline thread_local read_control *current = nullptr;

struct scoped_control {
  explicit scoped_control(read_control &control) { current = &control; }
  ~scoped_control() { current = nullptr; }
};
}  // namespace pipeline_test_io

// Select this overload only in this test translation unit. It supplies one
// controlled header read; subsequent reads use the real TCP/TLS stream.
namespace coro_io {
template <typename Socket>
async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_until(
    Socket &socket, asio::streambuf &buffer,
    cinatra::detail::http_header_matcher matcher) {
  auto *control = pipeline_test_io::current;
  if (control) {
    control->buffer = &buffer;
    if (!control->initial_data.empty()) {
      buffer.sputn(control->initial_data.data(), control->initial_data.size());
      auto size = control->initial_data.find("\r\n\r\n") + 4;
      control->initial_data.clear();
      co_return std::make_pair(std::error_code{}, size);
    }
  }
  co_return co_await coro_io::async_read_until<
      Socket, asio::streambuf, cinatra::detail::http_header_matcher>(
      socket, buffer, matcher);
}
}  // namespace coro_io

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
                                                    asio::streambuf &buffer,
                                                    int expected_status) {
  auto [ec, size] =
      co_await coro_io::async_read_until(stream, buffer, TWO_CRCF);
  if (ec) {
    throw std::system_error(ec);
  }
  http_parser parser;
  auto data = asio::buffer_cast<const char *>(buffer.data());
  REQUIRE(parser.parse_response(data, size, 0) > 0);
  CHECK(parser.status() == expected_status);
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
           size_t replies_before_suffix, int expected_status) {
    // Fix the first read's contents independently of TCP/TLS packet boundaries.
    const auto seeded_size = std::min<size_t>(prefix.size(), 512);
    pipeline_test_io::read_control reads{prefix.substr(0, seeded_size)};
    pipeline_test_io::scoped_control controlled_read(reads);
    auto wire_prefix = prefix.substr(seeded_size);
    asio::steady_timer deadline(context);
    bool client_done = false;
    bool server_done = false;
    bool timed_out = false;
    auto cancel_deadline_if_done = [&] {
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
        context.stop();
      }
    });
    connection->start().via(&executor).start([&](auto &&result) {
      CHECK_FALSE(result.hasError());
      server_done = true;
      cancel_deadline_if_done();
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
      }
#endif
      if (!wire_prefix.empty()) {
        auto [write_ec, _] =
            co_await coro_io::async_write(stream, asio::buffer(wire_prefix));
        if (write_ec) {
          throw std::system_error(write_ec);
        }
      }
      asio::streambuf received;
      for (size_t i = 0; i < expected.size(); ++i) {
        if (i == replies_before_suffix && !suffix.empty()) {
          size_t consumed = 0;
          for (size_t j = 0; j < replies_before_suffix; ++j) {
            consumed = prefix.find(TWO_CRCF, consumed) + TWO_CRCF.size();
          }
          REQUIRE(reads.buffer != nullptr);
          CHECK(reads.buffered() == prefix.substr(consumed));
          // Only release the suffix after checking the exact buffered tail.
          auto [ec, _] =
              co_await coro_io::async_write(stream, asio::buffer(suffix));
          if (ec) {
            throw std::system_error(ec);
          }
        }
        auto body = co_await read_response(stream, received, expected_status);
        CHECK(body == expected[i]);
      }
      if (expected_status == 200) {
        REQUIRE(reads.buffer != nullptr);
        CHECK(reads.buffered().empty());
      }
      else {
        char byte;
        auto buffer = asio::buffer(&byte, 1);
        auto [ec, _] = co_await coro_io::async_read(stream, buffer, 1);
        CHECK(ec == asio::error::eof);
      }
    };
    client().via(&executor).start([&](auto &&result) {
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
      cancel_deadline_if_done();
    });
    context.run();
    CHECK_FALSE(timed_out);
    CHECK(client_done);
    CHECK(server_done);
    CHECK(handled == expected.size());
    if (expected_status != 200) {
      CHECK(connection->has_closed());
      // The valid request was consumed; the malformed tail was not replayed.
      REQUIRE(reads.buffer != nullptr);
      CHECK(reads.buffered() ==
            prefix.substr(prefix.find(TWO_CRCF) + TWO_CRCF.size()));
    }
    connection->close();
    context.restart();
    context.poll();
  }

  void check(const std::string &prefix, const std::string &suffix,
             const std::vector<std::string> &expected,
             size_t replies_before_suffix, int expected_status = 200) {
#ifdef CINATRA_ENABLE_SSL
    if (tls) {
      asio::ssl::context ssl_context(asio::ssl::context::tls_client);
      asio::ssl::stream<asio::ip::tcp::socket &> stream(peer, ssl_context);
      run(stream, prefix, suffix, expected, replies_before_suffix,
          expected_status);
      return;
    }
#endif
    run(peer, prefix, suffix, expected, replies_before_suffix, expected_status);
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

TEST_CASE("pipeline closes after malformed buffered headers") {
  pipeline_test test(false, false);
  test.check(request_for("first") +
                 "GET /bad HTTP/1.x\r\nHost: localhost\r\n\r\n" +
                 request_for("must-not-run"),
             "", {"invalid http protocol"}, 1, 400);
}
