#include <asio/streambuf.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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
template <typename Socket, typename Match>
async_simple::coro::Lazy<std::pair<std::error_code, size_t>>
controlled_header_read(Socket &socket, asio::streambuf &buffer, Match matcher) {
  auto *control = pipeline_test_io::current;
  if (control) {
    control->buffer = &buffer;
    if (!control->initial_data.empty()) {
      buffer.sputn(control->initial_data.data(), control->initial_data.size());
      control->initial_data.clear();
      auto buffers = buffer.data();
      auto begin = asio::buffers_begin(buffers);
      auto match = matcher(begin, asio::buffers_end(buffers));
      if (match.second) {
        co_return std::make_pair(std::error_code{},
                                 size_t(match.first - begin));
      }
    }
  }
  co_return co_await coro_io::async_read_until<Socket, asio::streambuf, Match>(
      socket, buffer, matcher);
}
template <typename Socket>
async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_until(
    Socket &socket, asio::streambuf &buffer,
    cinatra::detail::http_header_matcher matcher) {
  return controlled_header_read(socket, buffer, matcher);
}
template <typename Socket>
async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_until(
    Socket &socket, asio::streambuf &buffer,
    cinatra::detail::bounded_http_header_matcher matcher) {
  return controlled_header_read(socket, buffer, matcher);
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
  bool extra_headers = false;
  bool count_records = false;
  size_t application_records = 0;

  void add_test_headers(coro_http_response &response) {
    if (extra_headers) {
      count_records = true;
      response.add_header("Content-Type", "text/plain; charset=UTF-8");
      response.add_header("Vary", "Accept-Encoding");
    }
  }

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
      auto ssl_context = coro_http_connection::make_ssl_context(
          (dir / "server.crt").string(), (dir / "server.key").string(), "test");
#ifdef TLS1_3_VERSION
      SSL_CTX_set_num_tickets(ssl_context->native_handle(), 0);
#endif
      SSL_CTX_set_msg_callback_arg(ssl_context->native_handle(), this);
      SSL_CTX_set_msg_callback(
          ssl_context->native_handle(),
          [](int write, int, int type, const void *data, size_t size, ::SSL *,
             void *state) {
            auto *test = static_cast<pipeline_test *>(state);
            if (test->count_records && write && type == SSL3_RT_HEADER &&
                size == 5 &&
                static_cast<const unsigned char *>(data)[0] == 23) {
              ++test->application_records;
            }
          });
      REQUIRE(connection->init_ssl(std::move(ssl_context)));
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
            add_test_headers(resp);
            resp.set_status_and_content(status_type::ok, std::string(value));
            ++handled;
          });
    }
    else {
      router.set_http_handler<GET>(
          "/pipeline",
          [this](coro_http_request &req, coro_http_response &resp) {
            add_test_headers(resp);
            resp.set_status_and_content(status_type::ok,
                                        std::string(req.get_query_value("n")));
            ++handled;
          });
    }
  }

  template <typename Stream>
  void run(Stream &stream, const std::string &prefix, const std::string &suffix,
           const std::vector<std::string> &expected,
           size_t replies_before_suffix, int expected_status,
           const std::vector<int> &statuses = {},
           size_t expected_handled = static_cast<size_t>(-1)) {
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
        const auto status = statuses.empty() ? expected_status : statuses[i];
        auto body = co_await read_response(stream, received, status);
        CHECK(body == expected[i]);
      }
      if ((statuses.empty() ? expected_status : statuses.back()) == 200) {
        REQUIRE(reads.buffer != nullptr);
        CHECK(reads.buffered().empty());
      }
      else {
        char byte;
        auto buffer = asio::buffer(&byte, 1);
        auto [ec, _] = co_await coro_io::async_read(stream, buffer, 1);
#ifdef CINATRA_ENABLE_SSL
        CHECK((ec == asio::error::eof ||
               ec == asio::ssl::error::stream_truncated));
#else
        CHECK(ec == asio::error::eof);
#endif
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
    CHECK(handled == (expected_handled == static_cast<size_t>(-1)
                          ? expected.size()
                          : expected_handled));
    if (expected_status != 200 ||
        (!statuses.empty() && statuses.back() != 200)) {
      CHECK(connection->has_closed());
      // The valid request was consumed; the malformed tail was not replayed.
      REQUIRE(reads.buffer != nullptr);
      if (statuses.empty()) {
        CHECK(reads.buffered() ==
              prefix.substr(prefix.find(TWO_CRCF) + TWO_CRCF.size()));
      }
      else {
        CHECK(reads.buffered().empty());
      }
    }
    connection->close();
    context.restart();
    context.poll();
  }

  void check(const std::string &prefix, const std::string &suffix,
             const std::vector<std::string> &expected,
             size_t replies_before_suffix, int expected_status = 200,
             const std::vector<int> &statuses = {},
             size_t expected_handled = static_cast<size_t>(-1)) {
#ifdef CINATRA_ENABLE_SSL
    if (tls) {
      asio::ssl::context ssl_context(asio::ssl::context::tls_client);
      asio::ssl::stream<asio::ip::tcp::socket &> stream(peer, ssl_context);
      run(stream, prefix, suffix, expected, replies_before_suffix,
          expected_status, statuses, expected_handled);
      return;
    }
#endif
    run(peer, prefix, suffix, expected, replies_before_suffix, expected_status,
        statuses, expected_handled);
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

TEST_CASE(
    "bounded header matcher ignores body bytes at an exact header limit") {
  const std::string eol{char(13), char(10)};
  const auto header =
      "POST / HTTP/1.1" + eol + "Content-Length: 10240" + eol + eol;
  asio::streambuf buffer;
  buffer.sputn(header.data(), header.size());
  const std::string body(10240, 'x');
  buffer.sputn(body.data(), body.size());
  bool exceeded = false;
  detail::bounded_http_header_matcher matcher{&buffer, header.size(),
                                              &exceeded};
  auto data = buffer.data();
  auto begin = asio::buffers_begin(data);
  auto result = matcher(begin, asio::buffers_end(data));
  CHECK(result.second);
  CHECK_FALSE(exceeded);
  CHECK(size_t(result.first - begin) == header.size());
  detail::bounded_http_header_matcher smaller{&buffer, header.size() - 1,
                                              &exceeded};
  result = smaller(begin, asio::buffers_end(data));
  CHECK(result.second);
  CHECK(exceeded);
}

TEST_CASE(
    "bounded header matcher preserves limits across incremental searches") {
  const std::string eol{char(13), char(10)};
  const auto header = "GET / HTTP/1.1" + eol + "Host: localhost" + eol + eol;
  asio::streambuf buffer;
  const auto first = header.size() - 3;
  buffer.sputn(header.data(), first);
  bool exceeded = false;
  detail::bounded_http_header_matcher matcher{&buffer, header.size(),
                                              &exceeded};
  auto data = buffer.data();
  auto begin = asio::buffers_begin(data);
  auto result = matcher(begin, asio::buffers_end(data));
  REQUIRE_FALSE(result.second);
  CHECK_FALSE(exceeded);
  const auto position = size_t(result.first - begin);
  buffer.sputn(header.data() + first, header.size() - first);
  const std::string body(4096, 'b');
  buffer.sputn(body.data(), body.size());
  data = buffer.data();
  begin = asio::buffers_begin(data);
  result = matcher(begin + position, asio::buffers_end(data));
  CHECK(result.second);
  CHECK_FALSE(exceeded);
  CHECK(size_t(result.first - begin) == header.size());

  asio::streambuf invalid;
  invalid.sputn(body.data(), 64);
  detail::bounded_http_header_matcher limit{&invalid, 96, &exceeded};
  data = invalid.data();
  begin = asio::buffers_begin(data);
  result = limit(begin, asio::buffers_end(data));
  REQUIRE_FALSE(result.second);
  invalid.sputn(body.data(), 32);
  data = invalid.data();
  begin = asio::buffers_begin(data);
  result = limit(begin + 64, asio::buffers_end(data));
  CHECK(result.second);
  CHECK(exceeded);
  CHECK(size_t(result.first - begin) == 96);
}

TEST_CASE(
    "pipeline keeps prior response before rejecting a large next header") {
  bool tls = false;
  SUBCASE("TCP") {}
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS") { tls = true; }
#endif
  pipeline_test test(tls, false);
  test.connection->set_max_http_header_size(128);
  const std::string eol{char(13), char(10)};
  const auto oversized = "GET /pipeline?n=must-not-run HTTP/1.1" + eol +
                         "Host: localhost" + eol +
                         "X-Pad: " + std::string(200, 'x') + eol + eol;
  test.check(request_for("first") + oversized, "",
             {"first", "request header too large"}, 2, 200, {200, 431}, 1);
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("native response headers share one TLS application record") {
  bool suspend = false;
  SUBCASE("direct handler") {}
  SUBCASE("after worker work") { suspend = true; }
  pipeline_test test(true, suspend);
  test.extra_headers = true;
  test.check(request_for("body"), "", {"body"}, 1);
  CHECK(test.application_records == 1);
}
#endif

TEST_CASE("SSE awaits the event handler on the shared executor") {
  bool tls = false;
  SUBCASE("HTTP") {}
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("HTTPS") { tls = true; }
#endif
  asio::io_context context;
  coro_http_server server(context, 0, "127.0.0.1");
#ifdef CINATRA_ENABLE_SSL
  const auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() /
                   "include/cinatra";
  if (tls) {
    server.init_ssl((dir / "server.crt").string(),
                    (dir / "server.key").string(), "test");
  }
#endif
  using async_simple::coro::Lazy;
  server.set_http_handler<POST>(
      "/", [](coro_http_request &, coro_http_response &resp) -> Lazy<void> {
        auto *conn = resp.get_conn();
        if (co_await conn->begin_sse() &&
            co_await conn->write_chunked(
                "data: first\r\n\r\ndata: stop\r\n\r\ndata: extra\r\n\r\n")) {
          co_await conn->end_sse();
        }
      });
  auto serving = server.async_start();
  REQUIRE_FALSE(server.get_errc());
  coro_http_client client(context.get_executor());
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    REQUIRE(client.init_ssl(asio::ssl::verify_peer, dir.string(), "server.crt",
                            "localhost"));
  }
#endif
  std::jthread io([&] {
    context.run();
  });

  client.set_req_timeout(2s);
  bool callback_throws = true;
  std::vector<std::string> events;
  auto handler = [&](const sse_event &event) -> Lazy<bool> {
    // Posting suspends the handler even when all events arrived in one chunk.
    co_await coro_io::post(
        [] {
        },
        &client.get_executor());
    CHECK(context.get_executor().running_in_this_thread());
    if (callback_throws) {
      throw std::runtime_error("SSE callback failed");
    }
    events.push_back(event.data);
    co_return event.data != "stop";
  };
  const auto url = (tls ? "https://" : "http://") + std::string("127.0.0.1:") +
                   std::to_string(server.port()) + "/";
  const auto request = [&] {
    return async_simple::coro::syncAwait(
        client.async_post_sse(url, "prompt", req_content_type::text, handler)
            .via(&client.get_executor()));
  };
  CHECK_THROWS_WITH_AS(request(), "SSE callback failed", std::runtime_error);
  CHECK(client.has_closed());
  callback_throws = false;
  const auto result = request();
  client.close();
  server.stop();
  io.join();

  CHECK_FALSE(result.net_err);
  CHECK(result.status == 200);
  CHECK_FALSE(result.eof);
  CHECK((events == std::vector<std::string>{"first", "stop"}));
}
