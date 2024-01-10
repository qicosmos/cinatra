#include <filesystem>
#include <future>
#include <memory>
#include <system_error>

#include "cinatra.hpp"
#include "cinatra/websocket.hpp"
#include "doctest/doctest.h"

using namespace cinatra;

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test wss client") {
  http_ssl_server server(std::thread::hardware_concurrency());
  server.set_ssl_conf(
      {"../../include/cinatra/server.crt", "../../include/cinatra/server.key"});
  REQUIRE(server.listen("0.0.0.0", "9001"));

  server.set_http_handler<GET>("/", [](request &req, response &res) {
    assert(req.get_content_type() == content_type::websocket);

    req.on(ws_message, [](request &req) {
      auto part_data = req.get_part_data();
      req.get_conn<cinatra::SSL>()->send_ws_string(
          std::string(part_data.data(), part_data.length()));
    });
  });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  coro_http_client client{};
  bool ok = client.init_ssl(asio::ssl::verify_peer,
                            "../../include/cinatra/server.crt");
  REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");

  std::promise<void> promise;
  client.on_ws_msg([&promise](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      promise.set_value();
      return;
    }

    CHECK(data.resp_body == "hello");
    promise.set_value();
  });

  REQUIRE(async_simple::coro::syncAwait(
      client.async_ws_connect("wss://localhost:9001")));

  auto result = async_simple::coro::syncAwait(client.async_send_ws("hello"));
  std::cout << result.net_err << "\n";

  promise.get_future().wait();

  client.close();

  server.stop();
  server_thread.join();
}
#endif

async_simple::coro::Lazy<void> test_websocket(coro_http_client &client) {
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
    CHECK(reason == "ws close");
  });
  client.on_ws_msg([&](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      return;
    }

    std::cout << "client get ws msg: " << data.resp_body << "\n";

    bool r = data.resp_body.find("hello websocket") != std::string::npos ||
             data.resp_body.find("test again") != std::string::npos;
    CHECK(r);

    std::cout << data.resp_body << std::endl;
  });
  bool r = co_await client.async_ws_connect("ws://localhost:8090/ws");
  if (!r) {
    co_return;
  }

  auto result = co_await client.async_send_ws("hello websocket");
  std::cout << result.net_err << "\n";
  result = co_await client.async_send_ws("test again", /*need_mask = */
                                         false);
  std::cout << result.net_err << "\n";
  result = co_await client.async_send_ws_close("ws close");
  std::cout << result.net_err << "\n";
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

  coro_http_client client{};
  REQUIRE(async_simple::coro::syncAwait(
      client.async_ws_connect("ws://localhost:8090")));

  std::string send_str(len, 'a');

  client.on_ws_msg([&, send_str](resp_data data) {
    if (data.net_err) {
      std::cout << "ws_msg net error " << data.net_err.message() << "\n";
      return;
    }

    std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
    REQUIRE(data.resp_body.size() == send_str.size());
    CHECK(data.resp_body == send_str);
  });

  async_simple::coro::syncAwait(client.async_send_ws(send_str));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  server.stop();
  client.close();
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
  REQUIRE(async_simple::coro::syncAwait(
      client.async_ws_connect("ws://127.0.0.1:8090")));

  client.on_ws_msg([](resp_data data) {
    std::cout << data.net_err.message() << "\n";
  });

  server.stop();

  std::this_thread::sleep_for(std::chrono::milliseconds(600));

  auto result = async_simple::coro::syncAwait(client.async_send_ws(""));
  CHECK(result.net_err);
}

async_simple::coro::Lazy<void> test_websocket() {
  coro_http_client client{};
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
  });
  client.on_ws_msg([](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      return;
    }
    std::cout << data.resp_body << std::endl;
  });

  bool r = co_await client.async_ws_connect("ws://127.0.0.1:8089/ws_echo");
  if (!r) {
    co_return;
  }

  auto data = co_await client.async_send_ws("test2fdsaf", true, opcode::binary);
  // auto data = co_await client.async_send_ws_chuncked(tmp, 3145728, false,
  // opcode::binary);
  if (data.eof) {
    std::cout << "data complete" << std::endl;
  }

  auto result = co_await client.async_send_ws_close("ws close");
  std::cout << "close socket!\n";
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

          if (result.type == ws_frame_type::WS_TEXT_FRAME) {
          }
          if (result.type == ws_frame_type::WS_BINARY_FRAME) {
            auto part_data = result.data;
            std::ofstream out("output.iso",
                              std::ios_base::app | std::ios_base::binary);
            out << part_data;
            out.close();
          }
        }
      });
  server.async_start();

  async_simple::coro::syncAwait(test_websocket());
}
