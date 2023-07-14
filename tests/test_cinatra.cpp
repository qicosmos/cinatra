#include <async_simple/coro/Collect.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <system_error>
#include <vector>

#include "cinatra.hpp"
#include "cinatra/time_util.hpp"
#include "doctest/doctest.h"
using namespace std::chrono_literals;

using namespace cinatra;

#ifdef CINATRA_ENABLE_GZIP
std::string_view get_header_value(
    std::vector<std::pair<std::string, std::string>> &resp_headers,
    std::string_view key) {
  for (const auto &p : resp_headers) {
    if (p.first == key)
      return std::string_view(p.second.data(), p.second.size());
  }
  return {};
}

TEST_CASE("test for gzip") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }

  server.set_http_handler<GET, POST>("/gzip", [](request &req, response &res) {
    CHECK(req.get_header_value("Content-Encoding") == "gzip");
    res.set_status_and_content(status_type::ok, "hello world",
                               req_content_type::none, content_encoding::gzip);
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
  std::string uri = "http://127.0.0.1:8090/gzip";
  client.add_header("Content-Encoding", "gzip");
  auto result = async_simple::coro::syncAwait(client.async_get(uri));
  auto content = get_header_value(result.resp_headers, "Content-Encoding");
  CHECK(get_header_value(result.resp_headers, "Content-Encoding") == "gzip");
  std::string decompress_data;
  bool ret = gzip_codec::uncompress(result.resp_body, decompress_data);
  CHECK(ret == true);
  CHECK(decompress_data == "hello world");
  server.stop();
  server_thread.join();
}
#endif

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test ssl client") {
  {
    coro_http_client client{};
    bool ok = client.init_ssl("../../include/cinatra", "server.crt");
    REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");
    auto result = client.get("https://www.bing.com");
    CHECK(result.status >= 200);
  }

  {
    coro_http_client client{};
    client.enable_auto_redirect(true);
    bool ok = client.init_ssl("../../include/cinatra", "server.crt");
    REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");
    auto result = client.get("https://www.bing.com");
    CHECK(result.status >= 200);
  }

  {
    coro_http_client client{};
    client.enable_auto_redirect(true);
    client.init_ssl("../../include/cinatra", "notexistsserver.crt");
    auto result = client.get("https://www.bing.com");
    CHECK(result.status != 200);
  }

  {
    coro_http_client client{};
    client.enable_auto_redirect(true);
    client.init_ssl("../../include/cinatra", "fake_server.crt");
    auto result = client.get("https://www.bing.com");
    CHECK(result.status != 200);
  }

  {
    coro_http_client client{};
    client.set_req_timeout(8s);
    client.enable_auto_redirect(true);
    std::string uri = "https://www.bing.com";
    // Make sure the host and port are matching with your proxy server
    client.set_proxy("106.14.255.124", "80");
    resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
    if (!result.net_err)
      CHECK(result.status >= 200);
  }

  {
    coro_http_client client{};
    bool ok = client.init_ssl("../../include/cinatra/server.crt");
    REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");
    auto result = client.get("https://www.bing.com");
    CHECK(result.status >= 200);
  }
}

TEST_CASE("test ssl client") {
  coro_http_client client{};
  bool ok = client.init_ssl("../../include/cinatra", "server.crt");
  REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");
  client.set_sni_hostname("https://www.bing.com");
  auto result = client.get("https://www.bing.com");
  CHECK(result.status >= 200);
}
#endif

async_simple::coro::Lazy<void> test_collect_all() {
  asio::io_context ioc;
  std::thread thd([&] {
    ioc.run();
  });
  std::vector<std::shared_ptr<coro_http_client>> v;
  std::vector<async_simple::coro::Lazy<resp_data>> futures;
  for (int i = 0; i < 2; ++i) {
    auto client = std::make_shared<coro_http_client>();
    v.push_back(client);
    futures.push_back(client->async_get("http://www.baidu.com/"));
  }

  auto out = co_await async_simple::coro::collectAll(std::move(futures));

  for (auto &item : out) {
    auto result = item.value();
    CHECK(result.status >= 200);
  }
  thd.join();
}

TEST_CASE("test pass path not entire uri") {
  coro_http_client client{};
  auto r =
      async_simple::coro::syncAwait(client.async_get("http://www.baidu.com"));
  CHECK(r.status >= 200);

  r = async_simple::coro::syncAwait(client.async_get("http://www.baidu.com"));
  CHECK(r.status >= 200);

  r = async_simple::coro::syncAwait(client.async_get("/"));
  CHECK(r.status >= 200);
}

TEST_CASE("test coro_http_client connect/request timeout") {
  {
    coro_http_client client{};
    cinatra::coro_http_client::config conf{.conn_timeout_duration = 1ms};
    client.init_config(conf);
    auto r =
        async_simple::coro::syncAwait(client.async_get("http://www.baidu.com"));
    bool ret = (r.net_err == std::errc::timed_out ||
                r.net_err == asio::error::operation_aborted);
    CHECK(ret);
  }

  {
    coro_http_client client{};
    cinatra::coro_http_client::config conf{.conn_timeout_duration = 10s,
                                           .req_timeout_duration = 1ms};
    client.init_config(conf);
    auto r =
        async_simple::coro::syncAwait(client.async_get("http://www.baidu.com"));
    std::cout << r.net_err.message() << "\n";
    CHECK(r.net_err == std::errc::timed_out);
  }
}

TEST_CASE("test coro_http_client async_http_connect") {
  coro_http_client client{};
  cinatra::coro_http_client::config conf{.req_timeout_duration = 60s};
  client.init_config(conf);
  auto r = async_simple::coro::syncAwait(
      client.async_http_connect("http://www.baidu.com"));
  CHECK(r.status >= 200);
  for (auto [k, v] : r.resp_headers) {
    std::cout << k << ", " << v << "\n";
  }

  coro_http_client client1{};
  r = async_simple::coro::syncAwait(
      client1.async_http_connect("http//www.badurl.com"));
  CHECK(r.status != 200);

  r = async_simple::coro::syncAwait(client1.reconnect("http://cn.bing.com"));
  CHECK(r.status >= 200);

  r = async_simple::coro::syncAwait(client1.reconnect("http://www.baidu.com"));

  CHECK(r.status >= 200);
  r = async_simple::coro::syncAwait(client1.reconnect("http://www.purecpp.cn"));
  CHECK(r.status == 200);
}

TEST_CASE("test collect all") {
  async_simple::coro::syncAwait(test_collect_all());
}

TEST_CASE("test head put and some other request") {
  coro_http_client client{};

  auto f = client.async_head("http://httpbin.org/headers");
  auto result = async_simple::coro::syncAwait(f);
  for (auto [k, v] : result.resp_headers) {
    std::cout << k << ": " << v << "\n";
  }
  if (!result.net_err) {
    CHECK(result.status >= 200);
  }

  std::string json = R"({
"Id": 12345,
"Customer": "John Smith",
"Quantity": 1,
"Price": 10.00
})";

  coro_http_client client1{};
  result = async_simple::coro::syncAwait(client1.async_put(
      "http://reqbin.com/echo/put/json", json, req_content_type::json));
  for (auto [k, v] : result.resp_headers) {
    std::cout << k << ": " << v;
  }
  if (!result.net_err) {
    CHECK(result.status >= 200);
  }

  result = async_simple::coro::syncAwait(client1.async_delete(
      "http://reqbin.com/echo/delete/json.txt", json, req_content_type::json));
  for (auto [k, v] : result.resp_headers) {
    std::cout << k << ": " << v;
  }
  if (!result.net_err) {
    CHECK(result.status >= 200);
  }

  coro_http_client client2{};
  result = async_simple::coro::syncAwait(
      client2.async_options("http://httpbin.org"));
  for (auto [k, v] : result.resp_headers) {
    std::cout << k << ": " << v << std::endl;
  }
  if (!result.net_err) {
    CHECK(result.status >= 200);
  }

  result =
      async_simple::coro::syncAwait(client2.async_patch("http://httpbin.org"));
  for (auto [k, v] : result.resp_headers) {
    std::cout << k << ": " << v;
  }
  if (!result.net_err) {
    CHECK(result.status >= 200);
  }

  result =
      async_simple::coro::syncAwait(client2.async_trace("http://httpbin.org"));
  for (auto [k, v] : result.resp_headers) {
    std::cout << k << ": " << v;
  }
  if (!result.net_err) {
    CHECK(result.status >= 200);
  }
  std::cout << std::endl;
}

TEST_CASE("test upload file") {
  http_server server(std::thread::hardware_concurrency());
  //  server.enable_timeout(false);
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }

  server.set_http_handler<POST>("/multipart", [](request &req, response &res) {
    assert(req.get_content_type() == content_type::multipart);
    auto &files = req.get_upload_files();
    for (auto &file : files) {
      std::cout << file.get_file_path() << " " << file.get_file_size()
                << std::endl;
    }
    std::cout << "multipart finished\n";
    res.render_string("multipart finished");
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
  std::string uri = "http://127.0.0.1:8090/multipart";
  resp_data result =
      async_simple::coro::syncAwait(client.async_upload_multipart(uri));
  CHECK(result.status == 404);

  client.add_str_part("hello", "world");
  client.add_str_part("key", "value");
  CHECK(!client.add_file_part("key", "value"));
  result = async_simple::coro::syncAwait(client.async_upload_multipart(uri));
  CHECK(!client.is_redirect(result));
  if (result.status == 200) {
    CHECK(result.resp_body == "multipart finished");
  }

  client.add_str_part("hello", "world");
  result = async_simple::coro::syncAwait(
      client.async_upload_multipart("http//badurl.com"));
  CHECK(result.status == 404);

  client.set_max_single_part_size(1024);
  std::string test_file_name = "test1.txt";
  std::ofstream test_file;
  test_file.open(test_file_name,
                 std::ios::binary | std::ios::out | std::ios::trunc);
  std::vector<char> test_file_data(2 * 1024 * 1024, '0');
  test_file.write(test_file_data.data(), test_file_data.size());
  test_file.close();
  result = async_simple::coro::syncAwait(
      client.async_upload_multipart(uri, "test", test_file_name));

  if (result.status == 200) {
    CHECK(result.resp_body == "multipart finished");
  }
  std::filesystem::remove(std::filesystem::path(test_file_name));

  std::string not_exist_file = "notexist.txt";
  result = async_simple::coro::syncAwait(client.async_upload_multipart(
      uri, "test_not_exist_file", not_exist_file));
  CHECK(result.status == 404);

  result = async_simple::coro::syncAwait(client.async_upload_multipart(
      "http//badurl.com", "test_not_exist_file", not_exist_file));
  CHECK(result.status == 404);

  client.async_close();

  server.stop();
  server_thread.join();
}

TEST_CASE("test bad uri") {
  coro_http_client client{};
  CHECK(client.add_header("hello", "cinatra"));
  CHECK(client.add_header("hello", "cinatra"));
  CHECK(!client.add_header("", "cinatra"));
  CHECK(!client.add_header("Host", "cinatra"));
  client.add_str_part("hello", "world");
  auto result = async_simple::coro::syncAwait(
      client.async_upload_multipart("http://www.badurlrandom.org"));
  CHECK(result.status == 404);
}

TEST_CASE("test ssl without init ssl") {
  {
    coro_http_client client{};
    client.add_str_part("hello", "world");
    auto result = async_simple::coro::syncAwait(
        client.async_upload_multipart("https://www.bing.com"));
    CHECK(result.status == 404);
  }

  {
    coro_http_client client{};
    auto result =
        async_simple::coro::syncAwait(client.async_get("https://www.bing.com"));
    CHECK(result.status == 404);
  }
}

TEST_CASE("test multiple ranges download") {
  coro_http_client client{};
  std::string uri = "http://uniquegoodshiningmelody.neverssl.com/favicon.ico";

  std::string filename = "test1.txt";
  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  resp_data result = async_simple::coro::syncAwait(
      client.async_download(uri, filename, "1-10,11-16"));
  if (result.status == 200 && !result.resp_body.empty()) {
    CHECK(std::filesystem::file_size(filename) == 16);
  }
}

TEST_CASE("test ranges download") {
  coro_http_client client{};
  client.set_req_timeout(std::chrono::seconds(8));
  std::string uri = "http://httpbin.org/range/32";

  std::string filename = "test1.txt";
  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  resp_data result = async_simple::coro::syncAwait(
      client.async_download(uri, filename, "1-10"));
  if (result.status == 200 && !result.resp_body.empty()) {
    CHECK(std::filesystem::file_size(filename) == 10);
  }

  filename = "test2.txt";
  std::filesystem::remove(filename, ec);
  result = async_simple::coro::syncAwait(
      client.async_download(uri, filename, "10-15"));
  if (result.status == 200 && !result.resp_body.empty()) {
    CHECK(std::filesystem::file_size(filename) == 6);
  }
  // multiple range test
  //  auto result =
  //      async_simple::coro::syncAwait(client.async_download(uri, "test2.txt",
  //      "1-10, 20-30"));
  //  if (result.resp_body.size() == 31)
  //    CHECK(result.resp_body == "bcdefghijklmnopqrstuvwxyzabcdef");
}

TEST_CASE("test ranges download with a bad filename") {
  coro_http_client client{};
  std::string uri = "http://uniquegoodshiningmelody.neverssl.com/favicon.ico";

  std::string filename = "";
  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  resp_data result = async_simple::coro::syncAwait(
      client.async_download(uri, filename, "1-10,11-16"));
  CHECK(result.status == 404);
  CHECK(result.net_err ==
        std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST_CASE("test coro_http_client quit") {
  std::promise<bool> promise;
  [&] {
    { coro_http_client client{}; }
    promise.set_value(true);
  }();

  CHECK(promise.get_future().get());
}

TEST_CASE("test coro_http_client chunked download") {
  coro_http_client client{};
  client.set_req_timeout(10s);
  std::string uri =
      "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
  std::string filename = "test.jpg";

  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  auto r = client.download(uri, filename);
  if (!r.net_err)
    CHECK(r.status >= 200);
}

TEST_CASE("test coro_http_client get") {
  coro_http_client client{};
  auto r = client.get("http://www.purecpp.cn");
  CHECK(!r.net_err);
  CHECK(r.status == 200);
}

TEST_CASE("test coro_http_client add header and url queries") {
  coro_http_client client{};
  client.add_header("Connection", "keep-alive");
  auto r =
      async_simple::coro::syncAwait(client.async_get("http://www.purecpp.cn"));
  CHECK(!r.net_err);
  CHECK(r.status == 200);

  auto r2 = async_simple::coro::syncAwait(
      client.async_get("http://www.baidu.com?name='tom'&age=20"));
  CHECK(!r2.net_err);
  CHECK(r2.status == 200);
}

TEST_CASE("test coro_http_client not exist domain and bad uri") {
  {
    coro_http_client client{};
    auto r = async_simple::coro::syncAwait(
        client.async_get("http://www.notexistwebsit.com"));
    CHECK(r.net_err);
    CHECK(r.status != 200);
    CHECK(client.has_closed());
  }

  {
    coro_http_client client{};
    auto r = async_simple::coro::syncAwait(
        client.async_get("http://www.baidu.com/><"));
    CHECK(r.net_err);
    CHECK(r.status != 200);
    CHECK(client.has_closed());
  }
}

TEST_CASE("test coro_http_client async_get") {
  coro_http_client client{};
  auto r =
      async_simple::coro::syncAwait(client.async_get("http://www.purecpp.cn"));
  CHECK(!r.net_err);
  CHECK(r.status == 200);

  auto r1 =
      async_simple::coro::syncAwait(client.async_get("http://www.baidu.com"));
  CHECK(!r.net_err);
  CHECK(r.status == 200);
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
      uri, "async post hello coro_http_client", req_content_type::string));
  CHECK(result.resp_body ==
        "async post hello coro_http_client reply from post");

  result = client.post(uri, "sync post hello coro_http_client",
                       req_content_type::string);
  CHECK(result.resp_body == "sync post hello coro_http_client reply from post");

  std::string_view uri1 = "http://127.0.0.1:8090";
  std::string_view post_str = "post hello coro_http_client";

  result = async_simple::coro::syncAwait(
      client.async_request(uri, http_method::POST,
                           req_context<std::string_view>{.content = post_str}));
  CHECK(result.resp_body == "post hello coro_http_client reply from post");

  result = async_simple::coro::syncAwait(
      client.async_request(uri1, http_method::POST,
                           req_context<std::string_view>{.content = post_str}));
  CHECK(result.resp_body == "post hello coro_http_client reply from post");

  result = client.post(uri, "", req_content_type::string);
  CHECK(result.status == 200);

  server.stop();
  server_thread.join();
}

#ifdef INJECT_FOR_HTTP_CLIENT_TEST
TEST_CASE("test inject failed") {
  {
    coro_http_client client{};
    inject_response_valid = ClientInjectAction::response_error;
    client.set_req_timeout(8s);
    auto result = client.get("http://purecpp.cn");
    CHECK(result.net_err == std::errc::protocol_error);

    inject_header_valid = ClientInjectAction::header_error;
    result = client.get("http://purecpp.cn");
    CHECK(result.net_err == std::errc::protocol_error);
  }

  {
    coro_http_client client{};
    client.set_req_timeout(10s);
    std::string uri =
        "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
    std::string filename = "test.jpg";

    std::error_code ec{};
    std::filesystem::remove(filename, ec);

    inject_read_failed = ClientInjectAction::read_failed;
    auto result = client.download(uri, filename);
    CHECK(result.net_err == std::make_error_code(std::errc::not_connected));
  }

  {
    coro_http_client client{};
    client.set_req_timeout(10s);
    std::string uri =
        "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
    std::string filename = "test.jpg";

    std::error_code ec{};
    std::filesystem::remove(filename, ec);

    inject_chunk_valid = ClientInjectAction::chunk_error;
    auto result = client.download(uri, filename);
    CHECK(result.status == 404);
  }

  {
    coro_http_client client{};
    client.add_str_part("hello", "world");
    inject_write_failed = ClientInjectAction::write_failed;
    auto result = async_simple::coro::syncAwait(
        client.async_upload_multipart("https://www.bing.com"));
    CHECK(result.status == 404);
  }
}
#endif

TEST_CASE("test coro http proxy request") {
  coro_http_client client{};
  client.set_req_timeout(8s);
  std::string uri = "http://www.baidu.com";
  // Make sure the host and port are matching with your proxy server
  client.set_proxy("106.14.255.124", "80");
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  if (!result.net_err)
    CHECK(result.status >= 200);

  client.set_proxy("106.14.255.124", "80");
  result = async_simple::coro::syncAwait(client.async_get(uri));
  if (!result.net_err)
    CHECK(result.status >= 200);
}

TEST_CASE("test coro http proxy request with port") {
  coro_http_client client{};
  client.set_req_timeout(8s);
  std::string uri = "http://www.baidu.com:80";
  // Make sure the host and port are matching with your proxy server
  client.set_proxy("106.14.255.124", "80");
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  if (!result.net_err)
    CHECK(result.status >= 200);  // maybe return 500 from that host.
}

TEST_CASE("test coro http basic auth request") {
  coro_http_client client{};
  std::string uri = "http://www.purecpp.cn";
  client.set_proxy_basic_auth("user", "pass");
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  CHECK(result.status == 200);
}

TEST_CASE("test coro http bearer token auth request") {
  coro_http_client client{};
  std::string uri = "http://www.purecpp.cn";
  client.set_proxy_bearer_token_auth("password");
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  CHECK(result.status == 200);
}

TEST_CASE("test coro http redirect request") {
  coro_http_client client{};
  client.set_req_timeout(8s);
  std::string uri = "http://httpbin.org/redirect-to?url=http://httpbin.org/get";
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  if (result.status != 404 && !result.net_err) {
    CHECK(!result.net_err);
    if (result.status != 502)
      CHECK(result.status == 302);

    if (client.is_redirect(result)) {
      std::string redirect_uri = client.get_redirect_uri();
      result = async_simple::coro::syncAwait(client.async_get(redirect_uri));
      if (result.status != 502 && result.status != 404)
        CHECK(result.status == 200);
    }

    client.enable_auto_redirect(true);
    result = async_simple::coro::syncAwait(client.async_get(uri));
    CHECK(result.status >= 200);
  }
}

TEST_CASE("test coro http request timeout") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }
  server.set_http_handler<GET, POST>(
      "/", [&server](request &, response &res) mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        res.set_status_and_content(status_type::ok, "hello world");
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
  CHECK(result.status == 200);

  client.set_req_timeout(500ms);
  result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(result.net_err == std::errc::timed_out);

  // after timeout, the socket in client has been closed, so use a new client
  // to test.
  coro_http_client client1{};
  result = async_simple::coro::syncAwait(client1.async_post(
      uri, "async post hello coro_http_client", req_content_type::string));
  CHECK(!result.net_err);

  server.stop();
  server_thread.join();
}

TEST_CASE("test coro_http_client using external io_context") {
  asio::io_context io_context;
  std::promise<void> promise;
  auto future = promise.get_future();
  auto work = std::make_unique<asio::io_context::work>(io_context);
  std::thread io_thd([&io_context, &promise] {
    promise.set_value();
    io_context.run();
  });
  future.wait();

  coro_http_client client(io_context.get_executor());
  auto r =
      async_simple::coro::syncAwait(client.async_get("http://www.purecpp.cn"));
  CHECK(!r.net_err);
  CHECK(r.status == 200);
  work.reset();
  io_context.run();
  io_thd.join();
}

async_simple::coro::Lazy<resp_data> simulate_self_join() {
  coro_http_client client{};
  co_return co_await client.async_get("http://www.purecpp.cn");
}

TEST_CASE("test coro_http_client dealing with self join") {
  auto r = async_simple::coro::syncAwait(simulate_self_join());
  CHECK(!r.net_err);
  CHECK(r.status == 200);
}

TEST_CASE("test coro_http_client no scheme still send request check") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }
  server.set_http_handler<GET, POST>(
      "/", [&server](request &, response &res) mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        res.set_status_and_content(status_type::ok, "hello world");
      });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string uri = "http://127.0.0.1:8090";

  coro_http_client client{};
  auto resp = async_simple::coro::syncAwait(client.async_get("127.0.0.1:8090"));
  CHECK(!resp.net_err);
  CHECK(resp.status == 200);
  resp = async_simple::coro::syncAwait(
      client.async_get("127.0.0.1:8090/ref='http://www.baidu.com'"));
  CHECK(resp.status == 404);

  server.stop();
  server_thread.join();
}

TEST_CASE("test conversion between unix time and gmt time, http format") {
  std::chrono::microseconds time_cost{0};
  std::ifstream file("../../tests/files_for_test_time_parse/http_times.txt");
  if (!file) {
    std::cout << "open file failed" << std::endl;
  }
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream iss(line);
    std::string time_to_parse;
    std::string timestamp;
    if (std::getline(iss, time_to_parse, '#') && std::getline(iss, timestamp)) {
      std::pair<bool, std::time_t> result;
      auto start = std::chrono::system_clock::now();
      for (int i = 0; i < 100; i++) {
        result = get_timestamp(time_to_parse);
      }
      auto end = std::chrono::system_clock::now();
      auto duration = duration_cast<std::chrono::microseconds>(end - start);
      time_cost += duration;
      if (result.first == true) {
        CHECK(timestamp != "invalid");
        if (timestamp != "invalid") {
          CHECK(result.second == std::stoll(timestamp));
        }
      }
      else {
        CHECK(timestamp == "invalid");
      }
    }
  }
  file.close();
  std::cout << double(time_cost.count()) *
                   std::chrono::microseconds::period::num /
                   std::chrono::microseconds::period::den
            << "s" << std::endl;
}

TEST_CASE("test conversion between unix time and gmt time, utc format") {
  std::chrono::microseconds time_cost{0};
  std::ifstream file("../../tests/files_for_test_time_parse/utc_times.txt");
  if (!file) {
    std::cout << "open file failed" << std::endl;
  }
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream iss(line);
    std::string time_to_parse;
    std::string timestamp;
    if (std::getline(iss, time_to_parse, '#') && std::getline(iss, timestamp)) {
      std::pair<bool, std::time_t> result;
      auto start = std::chrono::system_clock::now();
      for (int i = 0; i < 100; i++) {
        result = get_timestamp<time_format::utc_format>(time_to_parse);
      }
      auto end = std::chrono::system_clock::now();
      auto duration = duration_cast<std::chrono::microseconds>(end - start);
      time_cost += duration;
      if (result.first == true) {
        CHECK(timestamp != "invalid");
        if (timestamp != "invalid") {
          CHECK(result.second == std::stoll(timestamp));
        }
      }
      else {
        CHECK(timestamp == "invalid");
      }
    }
  }
  file.close();
  std::cout << double(time_cost.count()) *
                   std::chrono::microseconds::period::num /
                   std::chrono::microseconds::period::den
            << "s" << std::endl;
}

TEST_CASE(
    "test conversion between unix time and gmt time, utc without punctuation "
    "format") {
  std::chrono::microseconds time_cost{0};
  std::ifstream file(
      "../../tests/files_for_test_time_parse/"
      "utc_without_punctuation_times.txt");
  if (!file) {
    std::cout << "open file failed" << std::endl;
  }
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream iss(line);
    std::string time_to_parse;
    std::string timestamp;
    if (std::getline(iss, time_to_parse, '#') && std::getline(iss, timestamp)) {
      std::pair<bool, std::time_t> result;
      auto start = std::chrono::system_clock::now();
      for (int i = 0; i < 100; i++) {
        result = get_timestamp<time_format::utc_without_punctuation_format>(
            time_to_parse);
      }
      auto end = std::chrono::system_clock::now();
      auto duration = duration_cast<std::chrono::microseconds>(end - start);
      time_cost += duration;
      if (result.first == true) {
        CHECK(timestamp != "invalid");
        if (timestamp != "invalid") {
          CHECK(result.second == std::stoll(timestamp));
        }
      }
      else {
        CHECK(timestamp == "invalid");
      }
    }
  }
  file.close();
  std::cout << double(time_cost.count()) *
                   std::chrono::microseconds::period::num /
                   std::chrono::microseconds::period::den
            << "s" << std::endl;
}