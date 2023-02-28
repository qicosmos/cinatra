#include <filesystem>
#include <future>
#include <system_error>

#include "cinatra.hpp"
#include "cinatra/client_factory.hpp"
#include "cinatra/http_client.hpp"
#include "cinatra/websocket.hpp"
#include "doctest.h"

using namespace cinatra;

TEST_CASE("test websocket simple server") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    // LOG_INFO << "listen failed";
    return;
  }

  // web socket
  server.set_http_handler<GET, POST>("/ws", [](request& req, response& res) {
    assert(req.get_content_type() == content_type::websocket);

    req.on(ws_open, [](request& req) {
      std::cout << "websocket start" << std::endl;
    });

    req.on(ws_message, [](request& req) {
      auto part_data = req.get_part_data();
      // echo
      std::string str = std::string(part_data.data(), part_data.length());
      req.get_conn<cinatra::NonSSL>()->send_ws_string(std::move(str));
      std::cout << part_data.data() << std::endl;
    });

    req.on(ws_error, [](request& req) {
      std::cout << "websocket pack error or network error" << std::endl;
    });
  });
}
