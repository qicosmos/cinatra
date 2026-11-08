#include <asio/streambuf.hpp>
#include <memory>
#include <string>
#include <thread>

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

using namespace cinatra;

namespace {
struct queue_pressure {
  asio::io_context context{1};
  std::shared_ptr<detail::http_io_queue> queue;
  size_t capacity;
  size_t cold = 0, completed = 0, seeded = 0, hot_steps = 0;
  bool failed = false;
  static constexpr size_t cold_count = 512;

  explicit queue_pressure(size_t n)
      : queue(
            std::make_shared<detail::http_io_queue>(context.get_executor(), n)),
        capacity(n) {}

  async_simple::coro::Lazy<void> task(size_t id) {
    std::string owned(1024 + id % 17, static_cast<char>('a' + id % 26));
    do {
      coro_io::callback_awaitor<std::pair<std::error_code, size_t>> awaitor;
      auto [ec, result] = co_await awaitor.await_resume([&](auto handler) {
        handler.set_value(std::error_code{}, id);
        ++seeded;
        queue->enqueue(handler);
      });
      if (ec || result != id ||
          !context.get_executor().running_in_this_thread() ||
          owned !=
              std::string(owned.size(), static_cast<char>('a' + id % 26))) {
        failed = true;
        break;
      }
      if (id >= capacity) {
        ++cold;
        break;
      }
      if (++hot_steps > cold_count * 32 + capacity * 2) {
        failed = true;
        break;
      }
    } while (cold != cold_count);
  }

  void run() {
    for (size_t id = 0; id < capacity + cold_count; ++id) {
      task(id).start([this](auto &&result) {
        CHECK_FALSE(result.hasError());
        ++completed;
      });
    }
    REQUIRE(seeded == capacity + cold_count);
    REQUIRE(completed == 0);
    context.run();
    CHECK_FALSE(failed);
    CHECK(cold == cold_count);
    CHECK(completed == capacity + cold_count);
    CHECK(queue->empty());
  }
};

async_simple::coro::Lazy<void> run_connection(
    std::shared_ptr<coro_http_connection> connection) {
  auto work = asio::make_work_guard(connection->tcp_socket().get_executor());
  co_await connection->start();
}

template <typename Stream>
std::string read_response(Stream &stream, asio::streambuf &buffer) {
  size_t size = asio::read_until(stream, buffer, TWO_CRCF);
  auto data = static_cast<const char *>(buffer.data().data());
  http_parser parser;
  REQUIRE(parser.parse_response(data, size, 0) > 0);
  REQUIRE(parser.status() == 200);
  size_t length = static_cast<size_t>(parser.body_len());
  if (buffer.size() < size + length) {
    asio::read(stream, buffer,
               asio::transfer_exactly(size + length - buffer.size()));
  }
  data = static_cast<const char *>(buffer.data().data());
  std::string body(data + size, length);
  buffer.consume(size + length);
  return body;
}
}  // namespace

TEST_CASE("io queue progresses overflow under continuous local work") {
  for (size_t capacity : {1, 32, 64, 128, 256}) {
    CAPTURE(capacity);
    queue_pressure test(capacity);
    test.run();
  }
}

TEST_CASE("io queue cancels a completed read before connection teardown") {
  asio::io_context context(1);
  coro_io::ExecutorWrapper<> executor(context.get_executor());
  auto queue =
      std::make_shared<detail::http_io_queue>(context.get_executor(), 64);
  auto router = std::make_unique<coro_http_router>();
  size_t handled = 0;
  router->set_http_handler<GET>(
      "/queued", [&](coro_http_request &, coro_http_response &response) {
        ++handled;
        response.set_status_and_content(status_type::ok, "must not run");
      });
  asio::ip::tcp::acceptor listener(context,
                                   {asio::ip::address_v4::loopback(), 0});
  asio::ip::tcp::socket peer(context);
  peer.connect(listener.local_endpoint());
  auto connection = std::make_shared<coro_http_connection>(
      &executor, listener.accept(), *router, queue);
  std::weak_ptr<coro_http_connection> weak_connection = connection;
  std::weak_ptr<detail::http_io_queue> weak_queue = queue;
  bool done = false;
  run_connection(connection).via(&executor).start([&](auto &&result) {
    CHECK_FALSE(result.hasError());
    done = true;
  });
  std::string request = "GET /queued HTTP/1.1\r\nHost: localhost\r\n\r\n";
  asio::write(peer, asio::buffer(request));
  // Stop exactly after the read completion enqueues its continuation, before
  // the posted drain runs. Packet boundaries and sleeps cannot affect this.
  for (size_t i = 0; i < 100 && queue->empty(); ++i) {
    REQUIRE(context.run_one() == 1);
  }
  REQUIRE_FALSE(queue->empty());
  connection->close(false);
  connection.reset();
  router.reset();
  queue.reset();
  context.run();
  CHECK(done);
  CHECK(handled == 0);
  CHECK(weak_connection.expired());
  CHECK(weak_queue.expired());
}

TEST_CASE("server io queue configuration is set before startup") {
  coro_http_server server(1, static_cast<unsigned short>(0), "127.0.0.1",
                          false);
  CHECK_THROWS_AS(server.set_io_queue(true, 0), std::invalid_argument);
  CHECK_THROWS_AS(server.set_io_queue(true, 257), std::invalid_argument);
  server.set_io_queue(true, 256);
  server.set_io_queue(false);
  auto started = server.async_start();
  CHECK_THROWS_AS(server.set_io_queue(true), std::logic_error);
  server.stop();
  started.wait();
}

TEST_CASE("server io queue serves TCP TLS and external contexts") {
  bool external = false, tls = false, enabled = true;
  SUBCASE("owned TCP") {}
  SUBCASE("external TCP") { external = true; }
  SUBCASE("default disabled TCP") { enabled = false; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("owned TLS") { tls = true; }
  SUBCASE("external TLS") {
    external = true;
    tls = true;
  }
#endif
  asio::io_context context(1);
  auto work = asio::make_work_guard(context);
  std::thread io_thread([&] {
    context.run();
  });
  auto server =
      external ? std::make_unique<coro_http_server>(context, 0, "127.0.0.1")
               : std::make_unique<coro_http_server>(
                     2, static_cast<unsigned short>(0), "127.0.0.1", false);
  if (enabled) {
    server->set_io_queue(true);
  }
  server->set_http_handler<GET, POST>(
      "/queued",
      [](coro_http_request &request,
         coro_http_response &response) -> async_simple::coro::Lazy<void> {
        std::string body;
        if (request.is_chunked()) {
          for (;;) {
            auto chunk = co_await request.get_conn()->read_chunked();
            REQUIRE_FALSE(chunk.ec);
            if (chunk.eof)
              break;
            body.append(chunk.data);
          }
        }
        else {
          body = request.get_body();
        }
        co_await coro_io::post([] {
        });
        CHECK(request.get_conn()
                  ->get_executor()
                  ->context()
                  .get_executor()
                  .running_in_this_thread());
        response.need_date_head(false);
        response.set_status_and_content(status_type::ok,
                                        body.empty() ? "ok" : body);
      });
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    const auto certs =
        fs::path(__FILE__).parent_path().parent_path() / "include/cinatra";
    server->init_ssl((certs / "server.crt").string(),
                     (certs / "server.key").string(), "test");
  }
#endif
  auto started = server->async_start();
  REQUIRE(server->port() != 0);
  asio::io_context client_context;
  asio::ip::tcp::socket peer(client_context);
  peer.connect({asio::ip::address_v4::loopback(), server->port()});
  auto exchange = [&](auto &stream) {
    asio::streambuf received;
    for (int i = 0; i < 8; ++i) {
      std::string request;
      if (i % 2 == 0) {
        request = "GET /queued HTTP/1.1\r\nHost: localhost\r\n\r\n";
      }
      else {
        request =
            "POST /queued HTTP/1.1\r\nHost: localhost\r\n"
            "Transfer-Encoding: chunked\r\n\r\n2\r\n20\r\n0\r\n\r\n";
      }
      asio::write(stream, asio::buffer(request));
      CHECK(read_response(stream, received) == (i % 2 == 0 ? "ok" : "20"));
    }
  };
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    asio::ssl::context ssl_context(asio::ssl::context::tls_client);
    asio::ssl::stream<asio::ip::tcp::socket &> stream(peer, ssl_context);
    stream.handshake(asio::ssl::stream_base::client);
    exchange(stream);
  }
  else
#endif
  {
    exchange(peer);
  }
  server->stop();
  started.wait();
  work.reset();
  io_thread.join();
}
