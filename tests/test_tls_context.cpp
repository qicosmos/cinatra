#include <atomic>
#include <barrier>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

#ifdef CINATRA_ENABLE_SSL
using namespace cinatra;

TEST_CASE("server keeps TLS credentials for concurrent later connections") {
  const auto source =
      std::filesystem::path(__FILE__).parent_path().parent_path() /
      "include/cinatra";
  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("cinatra-tls-context-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(temporary);
  struct cleanup {
    std::filesystem::path directory;
    ~cleanup() { std::filesystem::remove_all(directory); }
  } guard{temporary};
  const auto cert = temporary / "server.crt";
  const auto key = temporary / "server.key";
  std::filesystem::copy_file(source / "server.crt", cert);
  std::filesystem::copy_file(source / "server.key", key);

  coro_http_server server(2, 0, "127.0.0.1", false);
  server.init_ssl(cert.string(), key.string(), "test");
  std::atomic<size_t> replies = 0;
  server.set_http_handler<POST>(
      "/echo",
      [&](coro_http_request &request,
          coro_http_response &response) -> async_simple::coro::Lazy<void> {
        const auto executor = request.get_conn()->tcp_socket().get_executor();
        const auto in_executor = [&] {
          return static_cast<asio::io_context &>(executor.context())
              .get_executor()
              .running_in_this_thread();
        };
        CHECK(in_executor());
        co_await coro_io::post([] {
        });
        CHECK(in_executor());
        response.set_status_and_content_view(status_type::ok,
                                             request.get_body());
        ++replies;
      });
  auto serving = server.async_start();
  REQUIRE_FALSE(server.get_errc());
  REQUIRE(server.port() != 0);

  // async_start has configured the listener. Removing the files before the
  // barrier is an explicit ordering: all handshakes must use retained state.
  std::filesystem::remove(cert);
  std::filesystem::remove(key);
  constexpr size_t clients = 8;
  std::barrier launch(static_cast<std::ptrdiff_t>(clients + 1));
  std::vector<std::future<bool>> results;
  for (size_t i = 0; i < clients; ++i) {
    results.push_back(std::async(std::launch::async, [&, i] {
      launch.arrive_and_wait();
      try {
        asio::io_context context;
        asio::ssl::context tls(asio::ssl::context::tls_client);
        tls.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(context, tls);
        stream.next_layer().connect(
            {asio::ip::address_v4::loopback(), server.port()});
        stream.handshake(asio::ssl::stream_base::client);
        const std::string body(10240 + i, static_cast<char>('a' + i));
        const auto wire =
            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        asio::write(stream, asio::buffer(wire));
        asio::streambuf buffer;
        const auto header_size = asio::read_until(stream, buffer, "\r\n\r\n");
        const std::string_view head(
            static_cast<const char *>(buffer.data().data()), header_size);
        if (!head.starts_with("HTTP/1.1 200 ")) {
          return false;
        }
        buffer.consume(header_size);
        if (buffer.size() < body.size()) {
          asio::read(stream, buffer,
                     asio::transfer_exactly(body.size() - buffer.size()));
        }
        return std::string_view(static_cast<const char *>(buffer.data().data()),
                                buffer.size()) == body;
      } catch (const std::exception &) {
        return false;
      }
    }));
  }
  launch.arrive_and_wait();
  for (auto &result : results) {
    CHECK(result.get());
  }
  server.stop();
  CHECK(replies == clients);
}

TEST_CASE("client keeps TLS verification across new connections") {
  const auto cert_dir =
      std::filesystem::path(__FILE__).parent_path().parent_path() /
      "include/cinatra";
  const auto cert = (cert_dir / "server.crt").string();
  coro_http_server server(1, 0, "127.0.0.1", false);
  server.init_ssl(cert, (cert_dir / "server.key").string(), "test");
  server.set_http_handler<GET>(
      "/", [](coro_http_request &, coro_http_response &resp) {
        resp.add_header("Connection", "close");
        resp.set_status_and_content(status_type::ok, "ok");
      });
  auto serving = server.async_start();
  REQUIRE_FALSE(server.get_errc());

  const std::string uri =
      "https://127.0.0.1:" + std::to_string(server.port()) + "/";
  coro_http_client client;
  client.set_conn_timeout(std::chrono::seconds(3));
  client.set_req_timeout(std::chrono::seconds(3));
  const auto get = [&] {
    // Ask the server to close each connection so all requests require TLS
    // handshakes.
    return client.get(uri, {{"Connection", "close"}});
  };

  SUBCASE("trusted certificate matches the configured hostname") {
    REQUIRE(client.init_ssl(asio::ssl::verify_peer, cert_dir.string(),
                            "server.crt", "localhost"));
    for (int attempt = 0; attempt < 2; ++attempt) {
      auto result = get();
      CHECK_FALSE(result.net_err);
      CHECK(result.status == 200);
      CHECK(client.has_closed());
    }
    client.reset();
    auto result = get();
    CHECK_FALSE(result.net_err);
    CHECK(result.status == 200);
  }

  const auto check_hostname_rejected = [&] {
    const auto ec = get().net_err;
    CHECK(ec.category() == asio::error::get_ssl_category());
    CHECK(ERR_GET_REASON(ec.value()) == SSL_R_CERTIFICATE_VERIFY_FAILED);
  };
  SUBCASE("trusted certificate does not match the configured hostname") {
    REQUIRE(client.init_ssl(asio::ssl::verify_peer, cert_dir.string(),
                            "server.crt", "127.0.0.1"));
    check_hostname_rejected();
    client.reset();
    check_hostname_rejected();
  }

  SUBCASE("hostname defaults to the URL host") {
    REQUIRE(client.init_ssl(asio::ssl::verify_peer, cert_dir.string(),
                            "server.crt", ""));
    check_hostname_rejected();
  }

  SUBCASE("invalid SNI stops the TLS handshake") {
    REQUIRE(client.init_ssl(asio::ssl::verify_none, "", std::string(256, 'a')));
    const auto result = get();
    CHECK(result.net_err.category() == asio::error::get_ssl_category());
    CHECK(ERR_GET_REASON(result.net_err.value()) ==
          SSL_R_SSL3_EXT_INVALID_SERVERNAME);
    CHECK(result.status != 200);
    CHECK(client.has_closed());
  }
}

TEST_CASE("client scopes persistent connections to the request origin") {
  enum class check { reuse, verify_url, scope_override };
  check scenario = check::reuse;
  bool second_tls = true;
  SUBCASE("switch between TLS ports") {}
  SUBCASE("switch to HTTP") { second_tls = false; }
  SUBCASE("the next origin needs its own verified certificate") {
    scenario = check::verify_url;
  }
  SUBCASE("an explicit TLS hostname is scoped to the first target") {
    scenario = check::scope_override;
  }

  const auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() /
                   "include/cinatra";
  coro_http_server first(1, 0, "127.0.0.1", false);
  coro_http_server second(1, 0, "127.0.0.1", false);
  const auto setup = [&](coro_http_server &server, bool tls,
                         const std::string &body) {
    if (tls) {
      server.init_ssl((dir / "server.crt").string(),
                      (dir / "server.key").string(), "test");
    }
    server.set_http_handler<GET, POST>(
        "/",
        [body](coro_http_request &req,
               coro_http_response &resp) -> async_simple::coro::Lazy<void> {
          if (req.get_content_type() == content_type::chunked) {
            auto chunk = co_await req.get_conn()->read_chunked();
            while (!chunk.eof && !chunk.ec) {
              chunk = co_await req.get_conn()->read_chunked();
            }
            CHECK_FALSE(chunk.ec);
          }
          resp.set_status_and_content(status_type::ok, body);
        });
    return server.async_start();
  };
  auto first_running = setup(first, true, "first");
  auto second_running = setup(second, second_tls, "second");
  REQUIRE_FALSE(first.get_errc());
  REQUIRE_FALSE(second.get_errc());
  const auto first_url =
      "https://localhost:" + std::to_string(first.port()) + "/";
  const auto second_url = std::string(second_tls ? "https" : "http") +
                          "://localhost:" + std::to_string(second.port()) + "/";
  const auto unverified_host =
      "https://127.0.0.1:" + std::to_string(second.port()) + "/";

  coro_http_client client;
  client.set_conn_timeout(std::chrono::seconds(3));
  client.set_req_timeout(std::chrono::seconds(3));
  const auto trust_fixture = [&](std::string hostname = {}) {
    REQUIRE(client.init_ssl(asio::ssl::verify_peer, dir.string(), "server.crt",
                            hostname));
  };
  if (scenario == check::reuse) {
    trust_fixture();
    CHECK(client.get(first_url).resp_body == "first");
    CHECK_FALSE(client.has_closed());
    auto upload = async_simple::coro::syncAwait(client.async_post_chunked(
        second_url, "request", req_content_type::text));
    CHECK_FALSE(upload.net_err);
    CHECK(upload.resp_body == "second");
    CHECK(client.get(first_url).resp_body == "first");
    CHECK(client.get(second_url).resp_body == "second");
    CHECK(client.get("/").resp_body == "second");
  }
  else if (scenario == check::verify_url) {
    trust_fixture();
    CHECK(client.get(first_url).resp_body == "first");
    CHECK_FALSE(client.has_closed());
    const auto result = client.get(unverified_host);
    CHECK(result.net_err.category() == asio::error::get_ssl_category());
    CHECK(ERR_GET_REASON(result.net_err.value()) ==
          SSL_R_CERTIFICATE_VERIFY_FAILED);
    CHECK(client.has_closed());
  }
  else {
    trust_fixture("localhost");
    const auto first_ip =
        "https://127.0.0.1:" + std::to_string(first.port()) + "/";
    CHECK(client.get(first_ip).resp_body == "first");
    CHECK(client.get(second_url).net_err == std::errc::invalid_argument);
    client.reset();
    CHECK(client.get(second_url).net_err == std::errc::invalid_argument);
    trust_fixture();
    CHECK(client.get(second_url).resp_body == "second");
  }
}
#endif
