#include <future>

#include "cinatra.hpp"
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

TEST_CASE("test coro_http_client get") {
  coro_http_client client{};
  auto r = client.get("http://www.purecpp.cn");
  CHECK(!r.net_err);
  CHECK(r.status == status_type::ok);
}

TEST_CASE("test coro_http_client add header and url queries") {
  coro_http_client client{};
  client.add_header("Connection", "keep-alive");
  auto r =
      async_simple::coro::syncAwait(client.async_get("http://www.purecpp.cn"));
  CHECK(!r.net_err);
  CHECK(r.status == status_type::ok);

  auto r2 = async_simple::coro::syncAwait(
      client.async_get("http://www.baidu.com?name='tom'&age=20"));
  CHECK(!r2.net_err);
  CHECK(r2.status == status_type::ok);
}

TEST_CASE("test coro_http_client not exist domain and bad uri") {
  {
    coro_http_client client{};
    auto r = async_simple::coro::syncAwait(
        client.async_get("http://www.notexistwebsit.com"));
    CHECK(r.net_err);
    CHECK(r.status != status_type::ok);
    CHECK(client.has_closed());
  }

  {
    coro_http_client client{};
    auto r = async_simple::coro::syncAwait(client.async_get("www.purecpp.cn"));
    CHECK(r.net_err);
    CHECK(r.status != status_type::ok);
    CHECK(client.has_closed());
  }
}

TEST_CASE("test coro_http_client async_get") {
  coro_http_client client{};
  auto r =
      async_simple::coro::syncAwait(client.async_get("http://www.purecpp.cn"));
  CHECK(!r.net_err);
  CHECK(r.status == status_type::ok);

  auto r1 =
      async_simple::coro::syncAwait(client.async_get("http://www.baidu.com"));
  CHECK(!r.net_err);
  CHECK(r.status == status_type::ok);
}

TEST_CASE("test coro_http_client async_connect") {
  coro_http_client client{};
  auto r =
      async_simple::coro::syncAwait(client.async_ping("http://www.purecpp.cn"));
  CHECK(r);
}

TEST_CASE("test basic http request") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }

  server.set_http_handler<GET>(
      "/", [&server](request &, response &res) mutable {
        res.set_status_and_content(status_type::ok, "hello world");
      });
  server.set_http_handler<POST>(
      "/", [&server](request &req, response &res) mutable {
        std::string str(req.body());
        str.append(" reply from post");
        res.set_status_and_content(status_type::ok, std::move(str));
      });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http_client client{};
  std::string uri = "http://127.0.0.1:8090";
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(result.resp_body == "hello world");

  result = async_simple::coro::syncAwait(client.async_post(
      uri, "hello coro_http_client", req_content_type::string));
  CHECK(result.resp_body == "hello coro_http_client reply from post");

  server.stop();
  server_thread.join();
}

TEST_CASE("test basic http request") {
  fs::path path{"./www"};
  path /= "new_file.txt";
  fs::create_directories(path.parent_path());

  std::ofstream ofs(path);
  ofs << "this is some text in the new file\n";
  ofs.close();
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }
  server.set_http_handler<GET>(
    "/", [&server](request &, response &res) mutable {
      res.set_status_and_content(status_type::ok, "hello world");
    });
  server.set_http_handler<POST>(
      "/", [&server](request &req, response &res) mutable {
        std::string str(req.body());
        str.append(" reply from post");
        res.set_status_and_content(status_type::ok, std::move(str));
      });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http_client client{};
  std::string uri =
      "http://127.0.0.1:8090/new_file.txt";
  auto ret =
      async_simple::coro::syncAwait(client.async_download(uri, "download.txt", 100));
  CHECK(!ret.net_err);
  server.stop();
  server_thread.join();
}