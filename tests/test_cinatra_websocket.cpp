#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <system_error>

#include "cinatra.hpp"
#include "cinatra/websocket.hpp"
#include "doctest/doctest.h"

using namespace std::chrono_literals;

using namespace cinatra;

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test wss client") {
  cinatra::coro_http_server server(1, 9001);
  server.init_ssl("../../include/cinatra/server.crt",
                  "../../include/cinatra/server.key", "test");
  server.set_http_handler<cinatra::GET>(
      "/",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client{};
  bool ok = client.init_ssl(asio::ssl::verify_peer,
                            "../../include/cinatra/server.crt");
  REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");

  async_simple::coro::syncAwait(client.connect("wss://localhost:9001"));

  async_simple::coro::syncAwait(client.write_websocket("hello"));
  auto data = async_simple::coro::syncAwait(client.read_websocket());
  CHECK(data.resp_body == "hello");

  client.close();

  server.stop();
}
#endif

async_simple::coro::Lazy<void> test_websocket(coro_http_client &client) {
  auto r = co_await client.connect("ws://localhost:8090/ws");
  if (r.net_err) {
    co_return;
  }

  auto result = co_await client.write_websocket("hello websocket");
  auto data = co_await client.read_websocket();
  CHECK(data.resp_body == "hello websocket");
  co_await client.write_websocket("test again", /*need_mask = */
                                  false);
  data = co_await client.read_websocket();
  CHECK(data.resp_body == "test again");
  co_await client.write_websocket_close("ws close");
  data = co_await client.read_websocket();
  CHECK(data.resp_body == "ws close");
  CHECK(data.net_err == asio::error::eof);
}

TEST_CASE("test websocket") {
  cinatra::coro_http_server server(1, 8090);
  server.set_http_handler<cinatra::GET>(
      "/ws",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http_client client{};
  client.set_ws_sec_key("s//GYHa/XO7Hd2F2eOGfyA==");

  async_simple::coro::syncAwait(test_websocket(client));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // client->async_close();
}

void test_websocket_content(size_t len) {
  cinatra::coro_http_server server(1, 8090);
  server.set_http_handler<cinatra::GET>(
      "/",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();

  auto lazy = [len]() -> async_simple::coro::Lazy<void> {
    coro_http_client client{};
    co_await client.connect("ws://localhost:8090");
    std::string send_str(len, 'a');
    co_await client.write_websocket(std::string(send_str));
    auto data = co_await client.read_websocket();
    REQUIRE(data.resp_body.size() == send_str.size());
    CHECK(data.resp_body == send_str);
  };

  async_simple::coro::syncAwait(lazy());

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  server.stop();
}

TEST_CASE("test websocket content lt 126") {
  test_websocket_content(1);
  test_websocket_content(125);
}

TEST_CASE("test websocket content ge 126") {
  test_websocket_content(126);
  test_websocket_content(127);
}

TEST_CASE("test websocket content ge 65535") {
  test_websocket_content(65535);
  test_websocket_content(65536);
}

TEST_CASE("test send after server stop") {
  cinatra::coro_http_server server(1, 8090);
  server.async_start();

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  coro_http_client client{};
  async_simple::coro::syncAwait(client.connect("ws://127.0.0.1:8090"));

  server.stop();

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  async_simple::coro::syncAwait(client.write_websocket(""));
  auto data = async_simple::coro::syncAwait(client.read_websocket());
  CHECK(data.net_err);
}

TEST_CASE("test read write in different threads") {
  cinatra::coro_http_server server(1, 8090);
  server.set_http_handler<cinatra::GET>(
      "/",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        CHECK(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();

  auto client = std::make_shared<coro_http_client>();
  std::string send_str(100, 'a');
  std::weak_ptr weak = client;
  auto another_thread_lazy = [client,
                              send_str]() -> async_simple::coro::Lazy<void> {
    for (int i = 0; i < 100; i++) {
      auto data = co_await client->read_websocket();
      if (data.net_err) {
        co_return;
      }
      REQUIRE(data.resp_body.size() == send_str.size());
      CHECK(data.resp_body == send_str);
    }
  };
  another_thread_lazy().via(coro_io::get_global_executor()).start([](auto &&) {
  });

  auto lazy = [client, weak, &send_str]() -> async_simple::coro::Lazy<void> {
    co_await client->connect("ws://localhost:8090");
    for (int i = 0; i < 100; i++) {
      auto data = co_await client->write_websocket(std::string(send_str));
      if (data.net_err) {
        co_return;
      }
    }
  };

  async_simple::coro::syncAwait(lazy());

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  server.stop();
}

async_simple::coro::Lazy<void> test_websocket() {
  coro_http_client client{};
  auto r = co_await client.connect("ws://127.0.0.1:8089/ws_echo");
  if (r.net_err) {
    co_return;
  }

  co_await client.write_websocket("test2fdsaf", true, opcode::binary);
  auto data = co_await client.read_websocket();
  CHECK(data.resp_body == "test2fdsaf");

  co_await client.write_websocket_close("ws close");
  data = co_await client.read_websocket();
  CHECK(data.net_err == asio::error::eof);
  CHECK(data.resp_body == "ws close");
}

TEST_CASE("test client quit after send msg") {
  coro_http_server server(1, 8089);
  server.set_http_handler<cinatra::GET>(
      "/ws_echo",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        websocket_result result{};

        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
            break;
          }

          co_await resp.get_conn()->write_websocket(result.data);
        }
      });
  server.async_start();

  async_simple::coro::syncAwait(test_websocket());
}
