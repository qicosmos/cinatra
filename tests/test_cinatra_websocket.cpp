#include <filesystem>
#include <future>
#include <system_error>

#include "cinatra.hpp"
#include "cinatra/client_factory.hpp"
#include "cinatra/http_client.hpp"
#include "cinatra/websocket.hpp"
#include "doctest.h"

using namespace cinatra;

async_simple::coro::Lazy<void> test_websocket(coro_http_client &client) {
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
  });
  client.on_ws_msg([](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      return;
    }

    bool r = data.resp_body.find("hello websocket") != std::string::npos ||
             data.resp_body.find("test again") != std::string::npos;
    CHECK(r);

    std::cout << data.resp_body << std::endl;
  });
  bool r = co_await client.async_connect("ws://localhost:8090/ws");
  if (!r) {
    co_return;
  }

  auto result = co_await client.async_send_ws("hello websocket");
  std::cout << result.net_err << "\n";
  result = co_await client.async_send_ws("test again", /*need_mask = */ false);
  std::cout << result.net_err << "\n";
}

TEST_CASE("test websocket") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }
  server.enable_timeout(false);
  server.set_http_handler<GET, POST>("/ws", [](request &req, response &res) {
    assert(req.get_content_type() == content_type::websocket);

    req.on(ws_open, [](request &req) {
      std::cout << "websocket start" << std::endl;
    });

    req.on(ws_message, [](request &req) {
      auto part_data = req.get_part_data();
      // echo
      std::string str = std::string(part_data.data(), part_data.length());
      req.get_conn<cinatra::NonSSL>()->send_ws_string(str);
      std::cout << part_data.data() << std::endl;
    });

    req.on(ws_error, [](request &req) {
      std::cout << "websocket pack error or network error" << std::endl;
    });
  });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http_client client;
  async_simple::coro::syncAwait(test_websocket(client));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  server.stop();
  server_thread.join();
}
