#include <iostream>

#include "../include/cinatra.hpp"
#include "cinatra/response_cv.hpp"

using namespace cinatra;

template <typename... Args>
inline void print(Args... args) {
  ((std::cout << args << ' '), ...);
  std::cout << "\n";
}

inline void print(const std::error_code &ec) {
  print(ec.value(), ec.message());
}

struct log_t {
  bool before(coro_http_request &, coro_http_response &) {
    std::cout << "before log" << std::endl;
    return true;
  }

  bool after(coro_http_request &, coro_http_response &res) {
    std::cout << "after log" << std::endl;
    res.add_header("aaaa", "bbcc");
    return true;
  }
};

struct person {
  void foo(coro_http_request &, coro_http_response &res) {
    std::cout << i << std::endl;
    res.set_status(status_type::ok);
  }

  void foo1(coro_http_request &, coro_http_response &res) {
    std::cout << i << std::endl;
    res.set_status(status_type::ok);
  }

  int i = 0;
};

void test_ssl_server() {
#ifdef CINATRA_ENABLE_SSL
  // you should open macro CINATRA_ENABLE_SSL at first
  http_ssl_server server(2);

  server.set_ssl_conf({"server.crt", "server.key"});
  int r = server.listen("0.0.0.0", "9001");
  if (r < 0) {
    return;
  }

  server.set_http_handler<GET, POST>(
      "/", [](coro_http_request &req, coro_http_response &res) {
        auto str = req.get_conn<cinatra::SSL>()->remote_address();
        res.set_status_and_content(status_type::ok, "hello world from 9001");
      });

  server.run();
#endif
}

#ifdef CINATRA_ENABLE_SSL
void test_coro_http_client() {
  using namespace cinatra;
  coro_http_client client{};
  client.init_ssl("bing.com", "../../include/cinatra", "server.crt");
  auto data = client.get("https://www.bing.com");
  std::cout << data.resp_body << "\n";
  data = client.get("https://www.bing.com");
  std::cout << data.resp_body << "\n";
}
#endif

void test_sync_client() {
  {
    std::string uri = "http://www.baidu.com";
    coro_http_client client{};
    auto result = client.get(uri);
    assert(!result.net_err);
    print(result.status);

    result = client.post(uri, "hello", req_content_type::json);
    print(result.status);
  }

  {
    coro_http_client client{};
    std::string uri = "http://cn.bing.com";
    auto result = client.get(uri);
    assert(!result.net_err);
    print(result.status);

    result = client.post(uri, "hello", req_content_type::json);
    print(result.status);
  }
}

async_simple::coro::Lazy<void> test_async_client(coro_http_client &client) {
  std::string uri = "http://www.baidu.com";
  auto data = co_await client.async_get(uri);
  print(data.status);

  data = co_await client.async_get(uri);
  print(data.status);

  data = co_await client.async_post(uri, "hello", req_content_type::string);
  print(data.status);
}

async_simple::coro::Lazy<void> test_async_ssl_client(coro_http_client &client) {
#ifdef CINATRA_ENABLE_SSL
  std::string uri2 = "https://www.baidu.com";
  std::string uri3 = "https://cn.bing.com";
  client.init_ssl("bing.com", "../../include/cinatra", "server.crt");
  auto data = co_await client.async_get(uri2);
  print(data.status);

  data = co_await client.async_get(uri3);
  print(data.status);
#endif
  co_return;
}

async_simple::coro::Lazy<void> test_download() {
  coro_http_client client{};
  std::string uri =
      "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
  std::string filename = "test.jpg";

  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  auto r = co_await client.async_download(uri, filename);
  assert(!r.net_err);
  assert(r.status == 200);
  std::cout << "download finished\n";
}

async_simple::coro::Lazy<void> test_upload() {
  std::string uri = "http://example.com/";
  coro_http_client client{};
  auto result =
      co_await client.async_upload_multipart(uri, "test", "yourfile.jpg");
  print(result.status);
  std::cout << "upload finished\n";

  client.add_str_part("hello", "coro_http_client");
  client.add_file_part("test", "yourfile.jpg");
  result = co_await client.async_upload_multipart(uri, "test", "yourfile.jpg");
  print(result.status);
  std::cout << "upload finished\n";
}

async_simple::coro::Lazy<void> test_websocket() {
  coro_http_client client{};
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
  });
  client.on_ws_msg([](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      return;
    }
    std::cout << data.resp_body << std::endl;
  });

  bool r = co_await client.async_ws_connect("ws://localhost:8090/ws");
  if (!r) {
    co_return;
  }

  auto result =
      co_await client.async_send_ws("hello websocket");  // mask as default.
  std::cout << result.status << "\n";
  result = co_await client.async_send_ws("test again", /*need_mask = */ false);
  std::cout << result.status << "\n";
  result = co_await client.async_send_ws_close("ws close");
  std::cout << result.status << "\n";
}

void test_smtp_client() {
  asio::io_context io_context;
#ifdef CINATRA_ENABLE_SSL
  auto client = cinatra::smtp::get_smtp_client<cinatra::SSL>(io_context);
#else
  auto client = cinatra::smtp::get_smtp_client<cinatra::NonSSL>(io_context);
#endif
  smtp::email_server server{};
  server.server = "smtp.163.com";
  server.port = client.IS_SSL ? "465" : "25";
  server.user = "your_email@163.com";
  server.password = "your_email_password";

  smtp::email_data data{};
  data.filepath = "";  // some file as attachment.
  data.from_email = "your_email@163.com";
  data.to_email.push_back("to_some_email@163.com");
  // data.to_email.push_back("to_more_email@example.com");
  data.subject = "it is a test from cinatra smtp";
  data.text = "Hello cinatra smtp client";

  client.set_email_server(server);
  client.set_email_data(data);

  client.start();

  std::error_code ec;
  io_context.run(ec);
}

class qps {
 public:
  void increase() { counter_.fetch_add(1, std::memory_order_release); }

  qps() : counter_(0) {
    thd_ = std::thread([this] {
      while (!stop_) {
        size_t current = counter_.load(std::memory_order_acquire);
        std::cout << "qps: " << current - last_ << '\n';
        last_ = current;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // counter_.store(0, std::memory_order_release);
      }
    });
  }

  ~qps() {
    stop_ = true;
    thd_.join();
  }

 private:
  bool stop_ = false;
  std::thread thd_;
  std::atomic<uint32_t> counter_;
  uint32_t last_ = 0;
};

int main() {
  // test_coro_http_client();
  // test_smtp_client();
  {
    test_sync_client();
    coro_http_client client{};
    async_simple::coro::syncAwait(test_async_client(client));

    coro_http_client ssl_client{};
    async_simple::coro::syncAwait(test_async_ssl_client(ssl_client));
  }

  // test_ssl_server();
  // test_download();
  coro_http_server server(std::thread::hardware_concurrency(), 8090);

  // server.on_connection([](auto conn) { return true; });
  server.set_http_handler<GET, POST>(
      "/", [](coro_http_request &, coro_http_response &res) mutable {
        res.set_status_and_content(status_type::ok, "hello world");
      });

  server.set_http_handler<GET>(
      "/plaintext", [](coro_http_request &, coro_http_response &res) {
        res.set_status_and_content(status_type::ok, "Hello, World!");
      });

  server.async_start();

  return 0;
}