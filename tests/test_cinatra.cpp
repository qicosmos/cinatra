#include <filesystem>
#include <future>
#include <system_error>

#include "cinatra.hpp"
#include "cinatra/client_factory.hpp"
#include "cinatra/http_client.hpp"
#include "doctest.h"

using namespace cinatra;
void print(const response_data &result) {
  print(result.ec, result.status, result.resp_body, result.resp_headers.second);
}

std::string_view get_header_value(
    std::vector<std::pair<std::string, std::string>> &resp_headers,
    std::string_view key) {
  for (const auto &p : resp_headers) {
    if (p.first == key)
      return std::string_view(p.second.data(), p.second.size());
  }
  return {};
}
#ifdef CINATRA_ENABLE_GZIP
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

TEST_CASE("test upload file") {
  http_server server(std::thread::hardware_concurrency());
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
  client.add_str_part("hello", "world");
  client.add_str_part("key", "value");
  resp_data result = async_simple::coro::syncAwait(client.async_upload(uri));
  if (result.status == 200) {
    CHECK(result.resp_body == "multipart finished");
  }

  std::string test_file_name = "test1.txt";
  std::ofstream test_file;
  test_file.open(test_file_name, std::ios::binary);
  std::vector<char> test_file_data(1024 * 1024, '0');
  test_file.write(test_file_data.data(), test_file_data.size());
  result = async_simple::coro::syncAwait(
      client.async_upload(uri, "test", test_file_name));
  if (result.status == status_type::ok) {
    CHECK(result.resp_body == "multipart finished");
  }
  test_file.close();
  std::filesystem::remove(std::filesystem::path(test_file_name));

  server.stop();
  server_thread.join();
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
  std::string uri =
      "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
  std::string filename = "test.jpg";

  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  auto r = client.download(uri, filename);
  CHECK(!r.net_err);
  CHECK(r.status == 200);

  filename = "test2.jpg";
  std::filesystem::remove(filename, ec);
  r = client.download(uri, filename);
  CHECK(!r.net_err);
  CHECK(r.status == 200);

  SUBCASE("test the correctness of the downloaded file") {
    auto self_http_client = client_factory::instance().new_client();
    std::string self_filename = "_" + filename;

    std::promise<bool> pro;
    auto fu = pro.get_future();

    std::error_code ec;
    std::filesystem::remove(self_filename, ec);
    self_http_client->download(uri, self_filename, [&](response_data data) {
      if (data.ec) {
        std::cout << data.ec.message() << "\n";
        pro.set_value(false);
        return;
      }

      std::cout << "finished download\n";
      pro.set_value(true);
    });

    REQUIRE(fu.get());

    auto read_file = [](const std::string &filename) {
      std::string file_content;
      std::ifstream ifs(filename, std::ios::binary);
      std::array<char, 1024> buff;
      while (ifs.read(buff.data(), buff.size())) {
        file_content.append(std::string_view{
            buff.data(),
            static_cast<std::string_view::size_type>(ifs.gcount())});
      }
      return file_content;
    };
    auto f1 = read_file(filename);
    auto f2 = read_file(self_filename);

    REQUIRE(f1.size() == f2.size());
    CHECK(f1 == f2);
  }
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
    auto r = async_simple::coro::syncAwait(client.async_get("www.purecpp.cn"));
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

#if ENABLE_TEST_PROXY_REQUEST
TEST_CASE("test coro http proxy request") {
  coro_http_client client{};
  std::string uri = "http://www.baidu.com";
  // Make sure the host and port are matching with your proxy server
  client.set_proxy("192.168.102.1", 7890);
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  CHECK(result.status == 200);
}
#endif

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
  std::string uri = "http://httpbin.org/redirect-to?url=http://httpbin.org/get";
  resp_data result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  CHECK(result.status == 302);

  if (client.is_redirect(result)) {
    std::string redirect_uri = client.get_redirect_uri();
    result = async_simple::coro::syncAwait(client.async_get(redirect_uri));
    CHECK(!result.net_err);
    if (result.status != 502)
      CHECK(result.status == 200);
  }

  client.enable_auto_location(true);
  result = async_simple::coro::syncAwait(client.async_get(uri));
  CHECK(!result.net_err);
  CHECK(result.status == 200);
}
