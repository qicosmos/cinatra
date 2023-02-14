#include <future>

#include "async_simple/coro/SyncAwait.h"
#include "cinatra.hpp"
#include "cinatra/coro_http_client.hpp"
#include "doctest.h"

using namespace cinatra;
void print(const response_data &result) {
  print(result.ec, result.status, result.resp_body, result.resp_headers.second);
}

TEST_CASE("test coro_http_client quit") {
  std::promise<bool> promise;
  [&] {
    { coro_http_client client{}; }
    promise.set_value(true);
  }();

  CHECK(promise.get_future().get());
}

TEST_CASE("test coro_http_client async_connect") {
  coro_http_client client{};
  auto r = async_simple::coro::syncAwait(
      client.async_connect("http://www.purecpp.cn"));
  CHECK(r);
}

TEST_CASE("test basic http request") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }
  server.set_http_handler<GET, POST>(
      "/", [&server](request &, response &res) mutable {
        res.set_status_and_content(status_type::ok, "hello world");
        server.stop();
      });
  auto server_run = [&server]() {
    server.run();
  };
  auto thread = std::thread(std::move(server_run));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = cinatra::client_factory::instance().new_client();
  std::string uri = "http://127.0.0.1:8090";
  response_data result = client->get(uri);
  print(result);
  CHECK(result.resp_body == "hello world");

  thread.join();
}
