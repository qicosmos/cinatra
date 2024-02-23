#include <cinatra.hpp>

using namespace cinatra;
using namespace std::chrono_literals;

int main() {
  coro_http_server server(std::thread::hardware_concurrency(), 8090, true);
  server.set_http_handler<GET>(
      "/plaintext", [](coro_http_request& req, coro_http_response& resp) {
        resp.get_conn()->set_multi_buf(false);
        resp.set_content_type<resp_content_type::txt>();
        resp.set_status_and_content(status_type::ok, "Hello, world!");
      });
  server.sync_start();
}
