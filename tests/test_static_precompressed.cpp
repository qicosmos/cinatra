#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

using namespace cinatra;
using namespace std::string_view_literals;

TEST_CASE("static files can opt in to serving precompressed variants") {
  bool enabled = true;
  bool chunked = false;
  bool cached = false;
  bool tls = false;
  SUBCASE("disabled by default") { enabled = false; }
  SUBCASE("fixed length") {}
  SUBCASE("chunked") { chunked = true; }
  SUBCASE("cached") { cached = true; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS with a shared executor") { tls = true; }
#endif

  struct test_directory {
    std::filesystem::path path = "test_static_precompressed_files";
    ~test_directory() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } directory;
  std::filesystem::create_directory(directory.path);
  const auto asset = "abcdef1234567890"sv;
  const auto gzip =
      "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x03\x4b\x4c\x4a"
      "\x4e\x49\x4d\x33\x34\x32\x36\x31\x35\x33\xb7\xb0\x34"
      "\x00\x00\xfb\x38\xdb\x62\x10\x00\x00\x00"sv;
  const auto brotli =
      "\x8b\x07\x80"
      "abcdef1234567890"
      "\x03"sv;
  auto write = [&](const char *name, std::string_view bytes) {
    std::ofstream out(directory.path / name, std::ios::binary);
    out << bytes;
    REQUIRE(out.good());
  };
  write("asset.js", asset);
  write("asset.js.br", brotli);
  write("asset.js.gz", gzip);
  write("fallback.js", asset);
  write("fallback.js.gz", gzip);

  asio::io_context context(1);
  std::jthread runner;
  auto work = asio::make_work_guard(context);
  // Drain client closes before OpenSSL's process-wide cleanup can run.
  runner = std::jthread([&] {
    context.run();
  });
  auto server =
      tls ? std::make_unique<coro_http_server>(context, 0, "127.0.0.1")
          : std::make_unique<coro_http_server>(1, 0, "127.0.0.1");
  if (enabled) {
    server->enable_precompressed_static_files();
  }
  if (chunked) {
    server->set_file_resp_format_type(file_resp_format_type::chunked);
  }
  server->set_static_res_dir("assets", directory.path.string());
  if (cached) {
    server->set_max_size_of_cache_files();
  }
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    const auto cert_dir =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "include/cinatra";
    server->init_ssl((cert_dir / "server.crt").string(),
                     (cert_dir / "server.key").string(), "test");
  }
#endif
  auto started = server->async_start();
  REQUIRE_FALSE(server->get_errc());
  const auto base = std::string(tls ? "https://" : "http://") +
                    "127.0.0.1:" + std::to_string(server->port()) + "/assets/";

  const auto header_value = [](const resp_data &result, std::string_view name) {
    for (const auto &[key, value] : result.resp_headers) {
      if (key == name) {
        return value;
      }
    }
    return std::string_view{};
  };
  auto check = [&](const char *name, std::string accept,
                   std::string_view coding, std::string_view body) {
    coro_http_client client(context.get_executor());
    const auto result = client.get(base + name, {{"Accept-Encoding", accept}});
    REQUIRE_FALSE(result.net_err);
    CHECK(result.status == 200);
    CHECK(header_value(result, "Content-Encoding") == coding);
    CHECK(header_value(result, "Content-Type") == "application/javascript");
    CHECK(header_value(result, "Vary") == (enabled ? "Accept-Encoding" : ""));
    if (!chunked) {
      CHECK(header_value(result, "Content-Length") ==
            std::to_string(body.size()));
#ifdef CINATRA_ENABLE_GZIP
      if (coding == "gzip") {
        body = asset;
      }
#endif
#ifdef CINATRA_ENABLE_BROTLI
      if (coding == "br") {
        body = asset;
      }
#endif
    }
    CHECK(result.resp_body == body);
  };
  check("asset.js", "br,gzip", enabled ? "br" : "", enabled ? brotli : asset);
  if (enabled) {
    check("asset.js", "br;q=0.3, gzip;q=0.8", "gzip", gzip);
    check("fallback.js", "br,gzip", "gzip", gzip);
    check("fallback.js", "gzip;q=0, *;q=1", "", asset);

    coro_http_client client(context.get_executor());
    const auto result = client.get(
        base + "asset.js", {{"Accept-Encoding", "br"}, {"Range", "bytes=1-3"}});
    REQUIRE_FALSE(result.net_err);
    CHECK(result.status == 206);
    CHECK(header_value(result, "Vary") == "Accept-Encoding");
    CHECK(header_value(result, "Content-Encoding").empty());
    CHECK(header_value(result, "Content-Range") == "bytes 1-3/16");
    CHECK(result.resp_body == "bcd");
  }
  CHECK(get_mime_type(".WOFF2") == "font/woff2");
  CHECK(get_mime_type(".webp") == "image/webp");
}
