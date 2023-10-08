#include <chrono>
#include <future>
#include <system_error>
#include <thread>

#include "async_simple/coro/Lazy.h"
#include "cinatra/ylt/coro_io/coro_io.hpp"
#include "cinatra/ylt/coro_io/io_context_pool.hpp"
#define DOCTEST_CONFIG_IMPLEMENT
#include <iostream>

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

using namespace std::chrono_literals;

TEST_CASE("test listen random port") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET>(
      "/", [](cinatra::coro_http_response& resp) {
        // response in io thread.
        std::cout << std::this_thread::get_id() << "\n";
        resp.set_keepalive(true);
        resp.set_status(cinatra::status_type::ok);
        resp.set_content("hello world");
      });

  server.set_http_handler<cinatra::GET>(
      "/coro",
      [](cinatra::coro_http_response& resp) -> async_simple::coro::Lazy<void> {
        std::cout << std::this_thread::get_id() << "\n";

        co_await coro_io::post([&] {
          // coroutine in other thread.
          std::cout << std::this_thread::get_id() << "\n";
          resp.set_status(cinatra::status_type::ok);
          resp.set_content("hello world in coro");
        });
        std::cout << std::this_thread::get_id() << "\n";
        co_return;
      });
  server.sync_start();
  CHECK(server.port() > 0);
}

TEST_CASE("test server start and stop") {
  cinatra::coro_http_server server(1, 9000);
  auto future = server.async_start();

  cinatra::coro_http_server server2(1, 9000);
  auto future2 = server2.async_start();
  future2.wait();
  auto ec = future2.value();
  CHECK(ec == std::errc::address_in_use);
}

TEST_CASE("test server sync_start and stop") {
  cinatra::coro_http_server server(1, 0);

  std::promise<void> promise;
  std::errc ec;
  std::thread thd([&] {
    promise.set_value();
    ec = server.sync_start();
  });
  promise.get_future().wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  server.stop();
  thd.join();
  CHECK(server.port() > 0);
  CHECK(ec == std::errc::operation_canceled);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007) int main(int argc, char** argv) {
  return doctest::Context(argc, argv).run();
}
DOCTEST_MSVC_SUPPRESS_WARNING_POP