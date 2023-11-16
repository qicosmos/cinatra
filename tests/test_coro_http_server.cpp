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
#include "cinatra/ylt/coro_io/coro_file_op.hpp"
#include "doctest/doctest.h"

using namespace cinatra;

using namespace std::chrono_literals;

TEST_CASE("coro_io post") {
  auto t1 = async_simple::coro::syncAwait(coro_io::post([] {
  }));
  CHECK(!t1.hasError());
  auto t2 = async_simple::coro::syncAwait(coro_io::post([] {
    throw std::invalid_argument("e");
  }));
  CHECK(t2.hasError());

  auto t3 = async_simple::coro::syncAwait(coro_io::post([] {
    return 1;
  }));
  int r3 = t3.value();
  CHECK(r3 == 1);

  auto t4 = async_simple::coro::syncAwait(coro_io::post([] {
    throw std::invalid_argument("e");
    return 1;
  }));
  CHECK(t4.hasError());

  try {
    std::rethrow_exception(t4.getException());
  } catch (const std::exception &e) {
    CHECK(e.what() == std::string("e"));
    std::cout << e.what() << "\n";
  }
}

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

class my_object {
 public:
  void normal(coro_http_request &req, coro_http_response &response) {
    response.set_status_and_content(status_type::ok, "ok");
  }

  async_simple::coro::Lazy<void> lazy(coro_http_request &req,
                                      coro_http_response &response) {
    response.set_status_and_content(status_type::ok, "ok");
    co_return;
  }
};

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

  my_object o;
  server.set_http_handler<cinatra::GET>("/normal", &my_object::normal, &o);
  server.set_http_handler<cinatra::GET>("/lazy", &my_object::lazy, &o);

  CHECK(router.get_handler("GET /normal") != nullptr);
  CHECK(router.get_handler("GET /lazy") == nullptr);
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

bool create_file(std::string_view filename, size_t file_size = 1024) {
  std::ofstream out(filename.data(), std::ios::binary);
  if (!out.is_open()) {
    return false;
  }

  std::string str(file_size, 'A');
  out.write(str.data(), str.size());
  return true;
}

struct read_result {
  std::string_view buf;
  bool eof;
  int err;
};

async_simple::coro::Lazy<resp_data> chunked_upload1(coro_http_client &client) {
  std::string filename = "test.txt";
  create_file(filename, 1010);

  auto fptr = coro_file_io::fopen_shared(filename, "rb");
  std::string buf;
  detail::resize(buf, 100);

  auto fn = [file = fptr.get(),
             &buf]() -> async_simple::coro::Lazy<read_result> {
    coro_file_io::file_result result =
        co_await coro_file_io::async_read(file, buf.data(), buf.size());
    co_return read_result{std::string_view(buf.data(), result.size), result.eof,
                          result.err};
  };

  auto result = co_await client.async_upload_chunked(
      "http://127.0.0.1:9001/chunked"sv, http_method::POST, std::move(fn));
  co_return result;
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

        std::cout << "content size: " << content.size() << "\n";
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
  auto r = async_simple::coro::syncAwait(chunked_upload1(client));
  CHECK(r.status == 200);
  CHECK(r.resp_body == "chunked ok");

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

TEST_CASE("test websocket") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET>(
      "/ws_echo",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        std::ofstream out_file("test.temp", std::ios::binary);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            out_file.close();
            break;
          }

          if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
            std::cout << "close frame\n";
            CHECK(result.data.empty());
            out_file.close();
            break;
          }

          if (result.type == ws_frame_type::WS_TEXT_FRAME) {
            CHECK(!result.data.empty());
            std::cout << result.data << "\n";
          }

          if (result.type == ws_frame_type::WS_BINARY_FRAME) {
            out_file << result.data;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();

  auto client = std::make_shared<coro_http_client>();
  client->on_ws_msg([](resp_data data) {
    if (data.net_err) {
      std::cout << "ws_msg net error " << data.net_err.message() << "\n";
      return;
    }

    std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
    CHECK(!data.resp_body.empty());
  });

  async_simple::coro::syncAwait(
      client->async_ws_connect("ws://127.0.0.1:9001/ws_echo"));
  async_simple::coro::syncAwait(
      client->async_send_ws("test2fdsaf", true, opcode::binary));
  async_simple::coro::syncAwait(client->async_send_ws("test_ws"));

  async_simple::coro::syncAwait(client->async_send_ws_close());
}

TEST_CASE("check small ws file") {
  std::string filename = "test.temp";
  std::error_code ec;
  size_t file_size = std::filesystem::file_size(filename, ec);
  if (ec) {
    return;
  }
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    return;
  }
  std::string str;
  str.resize(file_size);

  file.read(str.data(), str.size());
  CHECK(str == "test2fdsaf");
  std::filesystem::remove(filename, ec);
}

TEST_CASE("test websocket binary data") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET>(
      "/short_binary",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
            std::cout << "close frame\n";
            CHECK(result.data.empty());
            break;
          }

          if (result.type == ws_frame_type::WS_BINARY_FRAME) {
            CHECK(result.data.size() == 127);
          }
        }
      });
  server.set_http_handler<cinatra::GET>(
      "/medium_binary",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
            std::cout << "close frame\n";
            CHECK(result.data.empty());
            break;
          }

          if (result.type == ws_frame_type::WS_BINARY_FRAME) {
            CHECK(result.data.size() == 65535);
          }
        }
      });
  server.set_http_handler<cinatra::GET>(
      "/long_binary",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
            std::cout << "close frame\n";
            CHECK(result.data.empty());
            break;
          }

          if (result.type == ws_frame_type::WS_BINARY_FRAME) {
            CHECK(result.data.size() == 65536);
          }
        }
      });
  server.async_start();

  auto client1 = std::make_shared<coro_http_client>();
  async_simple::coro::syncAwait(
      client1->async_ws_connect("ws://127.0.0.1:9001/short_binary"));

  std::string short_str(127, 'A');
  async_simple::coro::syncAwait(
      client1->async_send_ws(std::move(short_str), true, opcode::binary));

  auto client2 = std::make_shared<coro_http_client>();
  async_simple::coro::syncAwait(
      client2->async_ws_connect("ws://127.0.0.1:9001/medium_binary"));

  std::string medium_str(65535, 'A');
  async_simple::coro::syncAwait(
      client2->async_send_ws(std::move(medium_str), true, opcode::binary));

  auto client3 = std::make_shared<coro_http_client>();
  async_simple::coro::syncAwait(
      client3->async_ws_connect("ws://127.0.0.1:9001/long_binary"));

  std::string long_str(65536, 'A');
  async_simple::coro::syncAwait(
      client3->async_send_ws(std::move(long_str), true, opcode::binary));

  async_simple::coro::syncAwait(client1->async_send_ws_close());
  async_simple::coro::syncAwait(client2->async_send_ws_close());
  async_simple::coro::syncAwait(client3->async_send_ws_close());
}

TEST_CASE("test websocket with different message sizes") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET>(
      "/ws_echo1",
      [](cinatra::coro_http_request &req,
         cinatra::coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        REQUIRE(req.get_content_type() == cinatra::content_type::websocket);
        cinatra::websocket_result result{};

        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == cinatra::ws_frame_type::WS_CLOSE_FRAME) {
            REQUIRE(result.data.empty());
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();

  auto client = std::make_shared<cinatra::coro_http_client>();

  SUBCASE("medium message - 16 bit length") {
    std::string medium_message(
        65535, 'x');  // 65,535 'x' characters for the medium message test.

    client->on_ws_msg([medium_message](cinatra::resp_data data) {
      if (data.net_err) {
        std::cout << "ws_msg net error " << data.net_err.message() << "\n";
        return;
      }

      size_t size = data.resp_body.size();
      std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
      REQUIRE(data.resp_body == medium_message);
    });

    async_simple::coro::syncAwait(
        client->async_ws_connect("ws://127.0.0.1:9001/ws_echo1"));
    async_simple::coro::syncAwait(client->async_send_ws(medium_message));
    async_simple::coro::syncAwait(client->async_send_ws_close());
  }

  SUBCASE("large message - 64 bit length") {
    std::string large_message(
        70000, 'x');  // 70,000 'x' characters for the large message test.

    client->on_ws_msg([large_message](cinatra::resp_data data) {
      if (data.net_err) {
        std::cout << "ws_msg net error " << data.net_err.message() << "\n";
        return;
      }

      std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
      REQUIRE(data.resp_body == large_message);
    });

    async_simple::coro::syncAwait(
        client->async_ws_connect("ws://127.0.0.1:9001/ws_echo1"));
    async_simple::coro::syncAwait(client->async_send_ws(large_message));
    async_simple::coro::syncAwait(client->async_send_ws_close());
  }

  server.stop();
}

TEST_CASE("check connecton timeout") {
  cinatra::coro_http_server server(1, 9001);
  server.set_check_duration(std::chrono::microseconds(600));
  server.set_timeout_duration(std::chrono::microseconds(500));
  server.set_http_handler<cinatra::GET>(
      "/", [](coro_http_request &req, coro_http_response &response) {
        response.set_status_and_content(status_type::ok, "ok");
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client;
  client.get("http://127.0.0.1:9001/");

  // wait for timeout, the timeout connections will be removed by server.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  CHECK(server.connection_count() == 0);
}

TEST_CASE("test websocket with message max_size limit") {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET>(
      "/ws_echo1",
      [](cinatra::coro_http_request &req,
         cinatra::coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        REQUIRE(req.get_content_type() == cinatra::content_type::websocket);
        cinatra::websocket_result result{};

        while (true) {
          req.get_conn()->set_ws_max_size(65536);
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == cinatra::ws_frame_type::WS_CLOSE_FRAME) {
            REQUIRE(result.data.empty());
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();

  auto client = std::make_shared<cinatra::coro_http_client>();

  SUBCASE("medium message - 16 bit length") {
    std::string medium_message(
        65535, 'x');  // 65,535 'x' characters for the medium message test.

    client->on_ws_msg([medium_message](cinatra::resp_data data) {
      if (data.net_err) {
        std::cout << "ws_msg net error " << data.net_err.message() << "\n";
        return;
      }

      size_t size = data.resp_body.size();
      std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
      REQUIRE(data.resp_body == medium_message);
    });

    async_simple::coro::syncAwait(
        client->async_ws_connect("ws://127.0.0.1:9001/ws_echo1"));
    async_simple::coro::syncAwait(client->async_send_ws(medium_message));
    async_simple::coro::syncAwait(client->async_send_ws_close());
  }

  SUBCASE("large message - 64 bit length") {
    std::string large_message(
        70000, 'x');  // 70,000 'x' characters for the large message test.

    client->on_ws_msg([large_message](cinatra::resp_data data) {
      if (data.net_err) {
        std::cout << "ws_msg net error " << data.net_err.message() << "\n";
        return;
      }

      std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
    });

    client->on_ws_close([](std::string_view reason) {
      REQUIRE(reason.size() > 0);
    });

    async_simple::coro::syncAwait(
        client->async_ws_connect("ws://127.0.0.1:9001/ws_echo1"));
    async_simple::coro::syncAwait(client->async_send_ws(large_message));
    async_simple::coro::syncAwait(client->async_send_ws_close());
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