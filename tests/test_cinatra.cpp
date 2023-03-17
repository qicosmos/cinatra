#include <filesystem>
#include <future>
#include <system_error>

#include "cinatra.hpp"
#include "doctest.h"
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
    client.set_timeout(8s);
    client.enable_auto_redirect(true);
    std::string uri = "https://www.bing.com";
    // Make sure the host and port are matching with your proxy server
    client.set_proxy("106.14.255.124", "80");
    resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
    if (!result.net_err)
      CHECK(result.status >= 200);
  }
}
#endif

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
  resp_data result = async_simple::coro::syncAwait(client.async_upload(uri));
  CHECK(result.status == 404);

  client.add_str_part("hello", "world");
  client.add_str_part("key", "value");
  CHECK(!client.add_file_part("key", "value"));
  result = async_simple::coro::syncAwait(client.async_upload(uri));
  CHECK(!client.is_redirect(result));
  if (result.status == 200) {
    CHECK(result.resp_body == "multipart finished");
  }

  client.add_str_part("hello", "world");
  result =
      async_simple::coro::syncAwait(client.async_upload("http//badurl.com"));
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
      client.async_upload(uri, "test", test_file_name));

  if (result.status == 200) {
    CHECK(result.resp_body == "multipart finished");
  }
  std::filesystem::remove(std::filesystem::path(test_file_name));

  std::string not_exist_file = "notexist.txt";
  result = async_simple::coro::syncAwait(
      client.async_upload(uri, "test_not_exist_file", not_exist_file));
  CHECK(result.status == 404);

  result = async_simple::coro::syncAwait(client.async_upload(
      "http//badurl.com", "test_not_exist_file", not_exist_file));
  CHECK(result.status == 404);

  server.stop();
  server_thread.join();
}

TEST_CASE("test bad uri") {
  coro_http_client client{};
  CHECK(client.add_header("hello", "cinatra"));
  CHECK(!client.add_header("hello", "cinatra"));
  CHECK(!client.add_header("", "cinatra"));
  CHECK(!client.add_header("Host", "cinatra"));
  client.add_str_part("hello", "world");
  auto result = async_simple::coro::syncAwait(
      client.async_upload("http://www.badurlrandom.org"));
  CHECK(result.status == 404);
}

TEST_CASE("test ssl without init ssl") {
  {
    coro_http_client client{};
    client.add_str_part("hello", "world");
    auto result = async_simple::coro::syncAwait(
        client.async_upload("https://www.bing.com"));
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
  client.set_timeout(std::chrono::seconds(8));
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
  client.set_timeout(10s);
  std::string uri =
      "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
  std::string filename = "test.jpg";

  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  auto r = client.download(uri, filename);
  CHECK(!r.net_err);
  CHECK(r.status == 200);
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

TEST_CASE("test coro_http_client async_connect") {
  coro_http_client client{};
  auto r = async_simple::coro::syncAwait(
      client.async_connect("http://www.purecpp.cn"));
  CHECK(r);

  r = async_simple::coro::syncAwait(
      client.async_connect("http//www.badurl.com"));
  CHECK(!r);
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
    client.set_timeout(8s);
    auto result = client.get("http://purecpp.cn");
    CHECK(result.net_err == std::errc::protocol_error);

    inject_header_valid = ClientInjectAction::header_error;
    result = client.get("http://purecpp.cn");
    CHECK(result.net_err == std::errc::protocol_error);
  }

  {
    coro_http_client client{};
    client.set_timeout(10s);
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
    client.set_timeout(10s);
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
        client.async_upload("https://www.bing.com"));
    CHECK(result.status == 404);
  }
}
#endif

TEST_CASE("test coro http proxy request") {
  coro_http_client client{};
  client.set_timeout(8s);
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
  client.set_timeout(8s);
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
  client.set_timeout(8s);
  std::string uri = "http://httpbin.org/redirect-to?url=http://httpbin.org/get";
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  if (result.status != 502)
    CHECK(result.status == 302);

  if (client.is_redirect(result)) {
    std::string redirect_uri = client.get_redirect_uri();
    result = async_simple::coro::syncAwait(client.async_get(redirect_uri));
    CHECK(!result.net_err);
    if (result.status != 502)
      CHECK(result.status == 200);
  }

  client.enable_auto_redirect(true);
  result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  if (result.status != 502)
    CHECK(result.status == 200);
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

  client.set_timeout(500ms);
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
