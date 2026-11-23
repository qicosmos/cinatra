#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "cinatra/http_header_matcher.hpp"
#include "cinatra/ylt/coro_io/coro_io.hpp"

namespace tls_body_test_io {
inline thread_local std::string_view prefetched_body;
}

// Observe the real read so the lifetime check proves that it exercised the
// borrowed body branch, without injecting or changing transport bytes.
namespace coro_io {
template <typename Socket>
async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_until(
    Socket &socket, asio::streambuf &buffer,
    cinatra::detail::bounded_http_header_matcher matcher) {
  auto result = co_await coro_io::async_read_until<
      Socket, asio::streambuf, cinatra::detail::bounded_http_header_matcher>(
      socket, buffer, matcher);
  tls_body_test_io::prefetched_body = {};
  if (!result.first && buffer.size() >= result.second) {
    tls_body_test_io::prefetched_body = {
        static_cast<const char *>(buffer.data().data()) + result.second,
        buffer.size() - result.second};
  }
  co_return result;
}
}  // namespace coro_io

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"
using namespace cinatra;
using namespace std::chrono_literals;

TEST_CASE("buffered request body survives shutdown and off-thread work") {
  bool external = false;
  bool tls = false;
  SUBCASE("owned context") {}
  SUBCASE("external context") { external = true; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("owned context with TLS") { tls = true; }
  SUBCASE("external context with TLS") {
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
               : std::make_unique<coro_http_server>(1, 0, "127.0.0.1");
  const std::string payload(10 * 1024, 'p');
  std::atomic<bool> body_kept{false};
  std::atomic<bool> fully_prefetched{false};
  std::atomic<bool> borrowed_body{false};
  std::promise<void> entered, release, worker_finished;
  auto entered_future = entered.get_future();
  auto release_future = release.get_future().share();
  auto worker_future = worker_finished.get_future();
  auto *worker = coro_io::get_global_block_executor();
  std::weak_ptr<coro_http_connection> connection;
  std::atomic<bool> resumed_on_socket_executor{false};
  server->set_http_handler<POST>(
      "/test/{}/{}",
      [&](coro_http_request &req,
          coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        connection = req.get_conn()->shared_from_this();
        const auto body = req.get_body();
        // Keep enough response header fragments across the worker barrier to
        // exercise their connection-owned compact representation on shutdown.
        resp.add_header("X-Owned-Header", "shutdown-body");
        resp.add_header("Vary", "Accept-Encoding");
        fully_prefetched =
            tls_body_test_io::prefetched_body.size() >= payload.size();
        borrowed_body = body.data() == tls_body_test_io::prefetched_body.data();
        co_await coro_io::post(
            [&] {
              entered.set_value();
              release_future.wait();
            },
            worker);
        resumed_on_socket_executor = req.get_conn()
                                         ->get_executor()
                                         ->context()
                                         .get_executor()
                                         .running_in_this_thread();
        body_kept = body == payload;
        resp.set_status_and_content_view(status_type::ok, body);
      });
#ifdef CINATRA_ENABLE_SSL
  const auto cert_dir =
      fs::path(__FILE__).parent_path().parent_path() / "include/cinatra";
  if (tls) {
    server->init_ssl((cert_dir / "server.crt").string(),
                     (cert_dir / "server.key").string(), "test");
  }
#endif
  auto started = server->async_start();
  asio::io_context client_context;
  asio::ip::tcp::socket peer(client_context);
  peer.connect({asio::ip::address_v4::loopback(), server->port()});
  const std::string eol{char(13), char(10)};
  std::string request = "POST /test/a/b HTTP/1.1" + eol + "Host: localhost" +
                        eol + "Content-Type: application/octet-stream" + eol +
                        "Content-Length: " + std::to_string(payload.size()) +
                        eol + eol + payload;

#ifdef CINATRA_ENABLE_SSL
  asio::ssl::context ssl_context(asio::ssl::context::tls_client);
  asio::ssl::stream<asio::ip::tcp::socket &> ssl_peer(peer, ssl_context);
  if (tls) {
    ssl_peer.handshake(asio::ssl::stream_base::client);
    asio::write(ssl_peer, asio::const_buffer(request.data(), request.size()));
  }
  else
#endif
  {
    asio::write(peer, asio::buffer(request));
  }
  entered_future.wait();
  auto stopped = std::async(std::launch::async, [&] {
    server->stop();
  });

  // Observe socket closure while the handler is held at an explicit barrier.
  char byte;
  std::error_code ec;
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    ssl_peer.read_some(asio::buffer(&byte, 1), ec);
  }
  else
#endif
  {
    peer.read_some(asio::buffer(&byte, 1), ec);
  }
  CHECK(ec);
  auto stopped_early = stopped.wait_for(100ms) == std::future_status::ready;
  work.reset();
  release.set_value();
  // This marker runs after the blocked worker callback has returned.
  asio::post(worker->get_asio_executor(), [&] {
    worker_finished.set_value();
  });
  worker_future.wait();
  stopped.get();
  io_thread.join();
  CHECK((external || !stopped_early));
  CHECK(resumed_on_socket_executor);
  CHECK(body_kept);
#if defined(CINATRA_TLS_HEADER_WINDOW) && CINATRA_TLS_HEADER_WINDOW > 0 && \
    defined(CINATRA_TLS_HEADER_BODY_VIEW) && CINATRA_TLS_HEADER_BODY_VIEW
  CHECK(borrowed_body == tls);
  if (tls) {
    CHECK(fully_prefetched);
  }
#else
  CHECK_FALSE(borrowed_body);
#endif
  CHECK(connection.expired());
}
