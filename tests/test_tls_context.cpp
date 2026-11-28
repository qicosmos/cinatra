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
    CHECK(client.has_closed());
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

  SUBCASE("automatic HTTPS verifies the peer") { check_hostname_rejected(); }

  SUBCASE("init_ssl verifies the peer by default") {
    REQUIRE(client.init_ssl());
    check_hostname_rejected();
  }

  SUBCASE("verification can be disabled explicitly") {
    REQUIRE(client.init_ssl(asio::ssl::verify_none));
    const auto result = get();
    CHECK_FALSE(result.net_err);
    CHECK(result.status == 200);
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
#endif
