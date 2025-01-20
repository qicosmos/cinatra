#include <cinatra.hpp>

using namespace cinatra;
using namespace std::chrono_literals;

int main() {
  coro_http_server server(std::thread::hardware_concurrency(), 8090, "0.0.0.0",
                          true);
  server.set_http_handler<GET>(
      "/plaintext", [](coro_http_request& req, coro_http_response& resp) {
        resp.set_delay(false);
        resp.need_date_head(false);
        resp.set_status_and_content(status_type::ok, "Hello, world!");
      });
  server.sync_start();
}
