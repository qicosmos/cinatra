#include <async_simple/coro/SyncAwait.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_server.hpp"

using namespace std::chrono_literals;

int main() {
#ifndef CINATRA_ENABLE_SSL
  std::cerr << "http2_example requires CINATRA_ENABLE_SSL\n";
  return 1;
#else
  cinatra::coro_http_server server(1, 0);
  server.set_http2_mode(cinatra::http2_mode::required);
  server.init_ssl("include/cinatra/server.crt", "include/cinatra/server.key",
                  "test");
  server.set_http_handler<cinatra::GET>(
      "/hello",
      [](cinatra::coro_http_request& req, cinatra::coro_http_response& resp) {
        resp.add_header("content-type", "text/plain");
        resp.set_status_and_content(
            cinatra::status_type::ok,
            req.get_url() == "/hello" ? "hello http2" : "not found");
      });

  server.async_start();
  auto port = server.port();
  std::this_thread::sleep_for(50ms);

  cinatra::coro_http_client client;
  auto url =
      std::string("https://127.0.0.1:") + std::to_string(port) + "/hello";
  auto resp = async_simple::coro::syncAwait(client.async_get(std::move(url)));
  if (resp.net_err) {
    std::cerr << "request failed: " << resp.net_err.message() << '\n';
    client.close();
    server.stop();
    return 1;
  }

  std::cout << "status: " << resp.status << '\n';
  std::cout << "body: " << resp.resp_body << '\n';

  client.close();
  server.stop();
  return 0;
#endif
}
