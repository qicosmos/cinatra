#include <chrono>
#include <future>
#include <system_error>
#include <thread>

#include "async_simple/coro/Lazy.h"
#include "cinatra/ylt/coro_io/coro_io.hpp"
#include "cinatra/ylt/coro_io/io_context_pool.hpp"
#define DOCTEST_CONFIG_IMPLEMENT
#include <iostream>

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

using namespace cinatra;

using namespace std::chrono_literals;

TEST_CASE("coro_server example, will block") {
  return;  // remove this line when you run the coro server.
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/", [](cinatra::coro_http_response &resp) {
        // response in io thread.
        std::cout << std::this_thread::get_id() << "\n";
        resp.set_keepalive(true);
        resp.set_status_and_content(cinatra::status_type::ok, "hello world");
      });

  server.set_http_handler<cinatra::GET>(
      "/coro",
      [](cinatra::coro_http_response &resp) -> async_simple::coro::Lazy<void> {
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

TEST_CASE("set http handler") {
  cinatra::coro_http_server server(1, 9001);

  auto &router = coro_http_router::instance();
  auto &handlers = router.get_handlers();

  server.set_http_handler<cinatra::GET>(
      "/", [](cinatra::coro_http_response &response) {
      });
  CHECK(handlers.size() == 1);
  server.set_http_handler<cinatra::GET>(
      "/", [](cinatra::coro_http_response &response) {
      });
  CHECK(handlers.size() == 1);
  server.set_http_handler<cinatra::GET>(
      "/aa", [](cinatra::coro_http_response &response) {
      });
  CHECK(handlers.size() == 2);

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/bb", [](cinatra::coro_http_response &response) {
      });
  CHECK(handlers.size() == 4);

  auto coro_func = [](cinatra::coro_http_response &response)
      -> async_simple::coro::Lazy<void> {
    co_return;
  };

  auto &coro_handlers = router.get_coro_handlers();
  server.set_http_handler<cinatra::GET>("/", coro_func);
  CHECK(coro_handlers.size() == 1);
  server.set_http_handler<cinatra::GET>("/", coro_func);
  CHECK(coro_handlers.size() == 1);
  server.set_http_handler<cinatra::GET>("/aa", coro_func);
  CHECK(coro_handlers.size() == 2);

  server.set_http_handler<cinatra::GET, cinatra::POST>("/bb", coro_func);
  CHECK(coro_handlers.size() == 4);

  CHECK(router.get_handler("GET /") != nullptr);
  CHECK(router.get_handler("GET /cc") == nullptr);
  CHECK(router.get_coro_handler("POST /bb") != nullptr);
  CHECK(router.get_coro_handler("POST /cc") == nullptr);
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

TEST_CASE("get post") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/test", [](cinatra::coro_http_response &resp) {
        resp.set_keepalive(true);
        resp.set_status_and_content(cinatra::status_type::ok, "hello world");
      });

  server.set_http_handler<cinatra::GET>(
      "/test_coro",
      [](cinatra::coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        co_await coro_io::post([&] {
          resp.set_status(cinatra::status_type::ok);
          resp.set_content("hello world in coro");
        });
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/empty", [](cinatra::coro_http_response &resp) {
        resp.add_header("Host", "Cinatra");
        resp.set_status_and_content(cinatra::status_type::ok, "");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/close", [](cinatra::coro_http_response &resp) {
        resp.set_keepalive(false);
        resp.set_status_and_content(cinatra::status_type::ok, "hello");
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client{};
  resp_data result;
  result = client.get("http://127.0.0.1:9001/test");
  CHECK(result.status == 200);
  CHECK(result.resp_body == "hello world");

  result =
      client.post("http://127.0.0.1:9001/test", "", req_content_type::text);
  CHECK(result.status == 200);
  CHECK(result.resp_body == "hello world");

  result = client.get("http://127.0.0.1:9001/test_coro");
  CHECK(result.status == 200);
  CHECK(result.resp_body == "hello world in coro");

  result = client.get("http://127.0.0.1:9001/not_exist");
  CHECK(result.status == 404);

  result = client.get("http://127.0.0.1:9001/empty");
  CHECK(result.status == 200);
  auto &headers = result.resp_headers;
  auto it =
      std::find_if(headers.begin(), headers.end(), [](http_header &header) {
        return header.name == "Host" && header.value == "Cinatra";
      });
  CHECK(it != headers.end());
  CHECK(result.resp_body.empty());

  client.add_header("Connection", "close");
  result = client.get("http://127.0.0.1:9001/close");
  CHECK(result.status == 200);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007) int main(int argc, char **argv) {
  return doctest::Context(argc, argv).run();
}
DOCTEST_MSVC_SUPPRESS_WARNING_POP