#include <chrono>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "async_simple/coro/Lazy.h"
#include "async_simple/coro/SyncAwait.h"
#include "cinatra/coro_http_connection.hpp"
#include "cinatra/define.h"
#include "cinatra/response_cv.hpp"
#include "cinatra/utils.hpp"
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
  cinatra::coro_http_server server(std::thread::hardware_concurrency(), 9001);
  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/", [](coro_http_request &req, coro_http_response &resp) {
        // response in io thread.
        std::this_thread::sleep_for(10ms);
        resp.set_status_and_content(cinatra::status_type::ok, "hello world");
      });

  server.set_http_handler<cinatra::GET>(
      "/coro",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        co_await coro_io::post([&]() {
          // coroutine in other thread.
          std::this_thread::sleep_for(10ms);
          resp.set_status_and_content(cinatra::status_type::ok, "hello world");
        });
        co_return;
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/echo", [](coro_http_request &req, coro_http_response &resp) {
        // response in io thread.
        resp.set_status_and_content(cinatra::status_type::ok, "hello world");
      });
  server.sync_start();
  CHECK(server.port() > 0);
}

TEST_CASE("set http handler") {
  cinatra::coro_http_server server(1, 9001);

  auto &router = coro_http_router::instance();
  auto &handlers = router.get_handlers();

  server.set_http_handler<cinatra::GET>(
      "/", [](coro_http_request &req, coro_http_response &response) {
        response.set_status_and_content(status_type::ok, "ok");
      });
  CHECK(handlers.size() == 1);
  server.set_http_handler<cinatra::GET>(
      "/", [](coro_http_request &req, coro_http_response &response) {
        response.set_status_and_content(status_type::ok, "ok");
      });
  CHECK(handlers.size() == 1);
  server.set_http_handler<cinatra::GET>(
      "/aa", [](coro_http_request &req, coro_http_response &response) {
        response.set_status_and_content(status_type::ok, "ok");
      });
  CHECK(handlers.size() == 2);

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/bb", [](coro_http_request &req, coro_http_response &response) {
        response.set_status_and_content(status_type::ok, "ok");
      });
  CHECK(handlers.size() == 4);

  auto coro_func =
      [](coro_http_request &req,
         coro_http_response &response) -> async_simple::coro::Lazy<void> {
    response.set_status_and_content(status_type::ok, "ok");
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
      "/test", [](coro_http_request &req, coro_http_response &resp) {
        auto value = req.get_header_value("connection");
        CHECK(!value.empty());

        auto value1 = req.get_header_value("connection1");
        CHECK(value1.empty());

        auto value2 = req.get_query_value("aa");
        CHECK(value2 == "1");

        auto value3 = req.get_query_value("bb");
        CHECK(value3 == "test");

        auto value4 = req.get_query_value("cc");
        CHECK(value4.empty());

        auto headers = req.get_headers();
        CHECK(!headers.empty());

        auto queries = req.get_queries();
        CHECK(!queries.empty());

        resp.set_keepalive(true);
        resp.set_status_and_content(cinatra::status_type::ok, "hello world");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/test1", [](coro_http_request &req, coro_http_response &resp) {
        resp.add_header("Host", "Cinatra");
        resp.set_status_and_content(cinatra::status_type::ok, "hello world");
      });

  server.set_http_handler<cinatra::GET>(
      "/test_coro",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        co_await coro_io::post([&] {
          resp.set_status(cinatra::status_type::ok);
          resp.set_content("hello world in coro");
        });
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/empty", [](coro_http_request &req, coro_http_response &resp) {
        resp.add_header("Host", "Cinatra");
        resp.set_status_and_content(cinatra::status_type::ok, "");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/close", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_keepalive(false);
        resp.set_status_and_content(cinatra::status_type::ok, "hello");
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client{};
  resp_data result;
  result = client.get("http://127.0.0.1:9001/test?aa=1&bb=test");
  CHECK(result.status == 200);
  CHECK(result.resp_body == "hello world");

  result =
      client.post("http://127.0.0.1:9001/test1", "", req_content_type::text);
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

  server.stop();
}

TEST_CASE("delay reply, server stop, form-urlencode, qureies, throw") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/delay", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_delay(true);
        std::thread([&resp] {
          std::this_thread::sleep_for(200ms);
          resp.set_status_and_content(status_type::ok, "delay reply");
          resp.get_conn()->sync_reply();
        }).detach();
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/delay2",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        resp.set_delay(true);
        std::this_thread::sleep_for(200ms);
        resp.set_status_and_content(status_type::ok, "delay reply in coro");
        co_await resp.get_conn()->reply();
      });

  server.set_http_handler<cinatra::POST>(
      "/form-urlencode", [](coro_http_request &req, coro_http_response &resp) {
        CHECK(req.get_body() == "theCityName=58367&aa=%22bbb%22");
        CHECK(req.get_query_value("theCityName") == "58367");
        CHECK(req.get_decode_query_value("aa") == "\"bbb\"");
        resp.set_status_and_content(status_type::ok, "form-urlencode");
      });

  server.set_http_handler<cinatra::GET>(
      "/throw", [](coro_http_request &req, coro_http_response &resp) {
        throw std::invalid_argument("invalid arguments");
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  resp_data result;
  {
    coro_http_client client{};
    result = client.get("http://127.0.0.1:9001/delay");
    CHECK(result.status == 200);
    CHECK(result.resp_body == "delay reply");
  }

  coro_http_client client1{};
  result = client1.get("http://127.0.0.1:9001/delay2");
  CHECK(result.status == 200);
  CHECK(result.resp_body == "delay reply in coro");

  result = client1.post("http://127.0.0.1:9001/form-urlencode",
                        "theCityName=58367&aa=%22bbb%22",
                        req_content_type::form_url_encode);
  CHECK(result.status == 200);
  CHECK(result.resp_body == "form-urlencode");

  result = client1.get("http://127.0.0.1:9001/throw");
  CHECK(result.status == 503);

  server.stop();
  std::cout << "ok\n";
}

TEST_CASE("chunked request") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/chunked",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        assert(req.get_content_type() == content_type::chunked);
        chunked_result result{};
        std::string content;

        while (true) {
          result = co_await req.get_conn()->read_chunked();
          if (result.ec) {
            co_return;
          }
          if (result.eof) {
            break;
          }

          content.append(result.data);
        }

        std::cout << content << "\n";
        resp.set_format_type(format_type::chunked);
        resp.set_status_and_content(status_type::ok, "chunked ok");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/write_chunked",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        resp.set_format_type(format_type::chunked);
        bool ok;
        if (ok = co_await resp.get_conn()->begin_chunked(); !ok) {
          co_return;
        }

        std::vector<std::string> vec{"hello", " world", " ok"};

        for (auto &str : vec) {
          if (ok = co_await resp.get_conn()->write_chunked(str); !ok) {
            co_return;
          }
        }

        ok = co_await resp.get_conn()->end_chunked();
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client{};
  auto ss = std::make_shared<std::stringstream>();
  *ss << "hello world";
  auto result = async_simple::coro::syncAwait(client.async_upload_chunked(
      "http://127.0.0.1:9001/chunked"sv, http_method::POST, ss));
  CHECK(result.status == 200);
  CHECK(result.resp_body == "chunked ok");

  result = client.get("http://127.0.0.1:9001/write_chunked");
  CHECK(result.status == 200);
  CHECK(result.resp_body == "hello world ok");
}

TEST_CASE("Test server stop function") {
  cinatra::coro_http_server server(1, 9001);

  server.set_http_handler<cinatra::GET>(
      "/test_stop", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(cinatra::status_type::ok,
                                    "Server is running");
      });

  server.async_start();
  cinatra::coro_http_client client;

  auto resp = client.get("http://localhost:9001/test_stop");
  CHECK(resp.status == static_cast<int>(cinatra::status_type::ok));
  CHECK(resp.resp_body == "Server is running");

  server.stop();

  // Attempt to make another request after server stopped. This should fail.
  auto resp_after_stop = client.get("http://localhost:9001/test_stop");
  CHECK(resp_after_stop.status ==
        static_cast<int>(cinatra::status_type::not_found));
}

TEST_CASE("Test invalid route") {
  cinatra::coro_http_server server(1,
                                   9003);

  server.set_http_handler<cinatra::GET>(
      "/valid_route", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(cinatra::status_type::ok, "Valid route");
      });

  server.async_start();
  cinatra::coro_http_client client;

  auto resp = client.get("http://localhost:9003/invalid_route");
  CHECK(resp.status == static_cast<int>(cinatra::status_type::not_found));

  server.stop();
}

TEST_CASE("Test coro_http_client with no server running") {
  cinatra::coro_http_client client;

  auto resp = client.get("http://localhost:9005/non_existent_route");
  CHECK(resp.status == static_cast<int>(cinatra::status_type::not_found));
}

TEST_CASE("Test coro_http_response functionalities") {
  cinatra::coro_http_server server(1,
                                   9006);

  server.set_http_handler<cinatra::GET>(
      "/test_response", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_status(cinatra::status_type::ok);
        resp.add_header("Test-Header", "Test-Value");
        resp.set_content("Test content");
      });

  server.async_start();
  cinatra::coro_http_client client;

  auto resp = client.get("http://localhost:9006/test_response");
  CHECK(resp.status == static_cast<int>(cinatra::status_type::ok));

  // Checking headers
  bool header_found = false;
  for (const auto &header : resp.resp_headers) {
    if (header.name == "Test-Header" && header.value == "Test-Value") {
      header_found = true;
      break;
    }
  }
  CHECK(header_found);

  CHECK(resp.resp_body == "Test content");

  server.stop();
}


TEST_CASE("Test custom HTTP handlers") {
  cinatra::coro_http_server server(1, 9007);

  bool custom_handler_executed = false;

  server.set_http_handler<cinatra::GET>(
      "/custom_handler", [&custom_handler_executed](coro_http_request &req,
                                                    coro_http_response &resp) {
        resp.set_status_and_content(cinatra::status_type::ok,
                                    "Custom handler executed");
        custom_handler_executed = true;
      });

  server.async_start();
  cinatra::coro_http_client client;

  auto resp = client.get("http://localhost:9007/custom_handler");
  CHECK(custom_handler_executed);
  CHECK(resp.resp_body == "Custom handler executed");

  server.stop();
}

TEST_CASE("Integrated server and client test") {
  const int server_port = 8085;
  cinatra::coro_http_server server(1, server_port);

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/test_client",
      [](cinatra::coro_http_request &req, cinatra::coro_http_response &resp) {
        resp.set_status_and_content(cinatra::status_type::ok,
                                    "Response from /test_client");
      });

  server.async_start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  cinatra::coro_http_client client;

  SUBCASE("GET request") {
    auto result = client.get("http://localhost:" + std::to_string(server_port) +
                             "/test_client");
    CHECK(!result.net_err);
    CHECK(result.status == static_cast<int>(cinatra::status_type::ok));
    CHECK(result.resp_body == "Response from /test_client");
  }

  SUBCASE("POST request") {
    auto result = client.post(
        "http://localhost:" + std::to_string(server_port) + "/test_client",
        "hello", cinatra::req_content_type::json);
    CHECK(!result.net_err);
    CHECK(result.status == static_cast<int>(cinatra::status_type::ok));
    CHECK(result.resp_body == "Response from /test_client");
  }

  // Asynchronous client tests
  SUBCASE("Async GET request") {
    auto result = async_simple::coro::syncAwait(client.async_get(
        "http://localhost:" + std::to_string(server_port) + "/test_client"));
    CHECK(!result.net_err);
    CHECK(result.status == static_cast<int>(cinatra::status_type::ok));
    CHECK(result.resp_body == "Response from /test_client");
  }

  SUBCASE("Async POST request") {
    auto result = async_simple::coro::syncAwait(client.async_post(
        "http://localhost:" + std::to_string(server_port) + "/test_client",
        "hello", cinatra::req_content_type::json));
    CHECK(!result.net_err);
    CHECK(result.status == static_cast<int>(cinatra::status_type::ok));
    CHECK(result.resp_body == "Response from /test_client");
  }

  server.stop();
}



#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test ssl server") {
  cinatra::coro_http_server server(1, 9001);

  server.init_ssl("../../include/cinatra/server.crt",
                  "../../include/cinatra/server.key", "test");
  server.set_http_handler<GET, POST>(
      "/ssl", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "ssl");
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client{};
  [[maybe_unused]] auto r = client.init_ssl();

  auto result = client.get("https://127.0.0.1:9001/ssl");
  CHECK(result.status == 200);
  CHECK(result.resp_body == "ssl");
  std::cout << "ssl ok\n";
}
#endif



DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP