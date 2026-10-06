#include <cinatra.hpp>
#include <string_view>

using namespace cinatra;
using namespace std::chrono_literals;

int main(int argc, char *argv[]) {
  coro_http_server server(std::thread::hardware_concurrency(), 8090, "0.0.0.0",
                          true);
  if (argc > 1 && std::string_view(argv[1]) == "--multi-acceptor") {
    server.set_multi_acceptor(true);
  }
  server.set_http_handler<GET>(
      "/plaintext", [](coro_http_request& req, coro_http_response& resp) {
        resp.set_delay(false);
        resp.need_date_head(false);
        resp.set_status_and_content(status_type::ok, "Hello, world!");
      });
  server.sync_start();
}
