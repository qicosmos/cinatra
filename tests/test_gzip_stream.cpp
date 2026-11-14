#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "cinatra/coro_http_server.hpp"
#include "doctest/doctest.h"

#if __has_include("iguana/json_writer.hpp")
#include "iguana/json_writer.hpp"
#define CINATRA_TEST_IGUANA
struct gzip_json_record {
  int id;
  std::string text;
  std::vector<double> values;
  std::map<std::string, std::string> labels;
  bool active;
};
REFLECTION(gzip_json_record, id, text, values, labels, active);
#endif

using namespace cinatra;
using namespace std::chrono_literals;

namespace {
template <typename Content>
concept response_accepts_explicit_lvalue =
    requires(coro_http_response &response, Content &content) {
      response.set_status_and_content_view<Content>(
          status_type::ok, content, content_encoding::none, false);
    };
static_assert(response_accepts_explicit_lvalue<std::string>);
static_assert(response_accepts_explicit_lvalue<const std::string>);
static_assert(response_accepts_explicit_lvalue<std::string_view>);

struct string_writer {
  void operator()(std::string &) const {}
};
#ifdef CINATRA_ENABLE_GZIP
struct gzip_writer {
  void operator()(gzip_codec::compress_stream &) const {}
};
static_assert(!detail::response_writer<gzip_writer>);
static_assert(!detail::response_writer<string_writer>);
#else
static_assert(detail::response_writer<string_writer>);
#endif

std::string make_payload(size_t size) {
  std::mt19937 random(42);
  std::string data(size, '\0');
  for (auto &value : data) {
    value = 'a' + random() % 26;
  }
  return data;
}

std::string response_wire(coro_http_response &response) {
  std::vector<asio::const_buffer> buffers;
  std::string chunk_size;
  response.to_buffers(buffers, chunk_size);
  std::string wire;
  for (auto buffer : buffers) {
    wire.append(static_cast<const char *>(buffer.data()), buffer.size());
  }
  return wire;
}

#ifdef CINATRA_ENABLE_GZIP
// Check the trailer and consume the whole member, independently of the HTTP
// client's decompression helper (which also accepts incomplete gzip input).
std::string decode_gzip(std::string_view data) {
  struct inflater {
    z_stream state{};
    ~inflater() { inflateEnd(&state); }
  } stream;
  REQUIRE(inflateInit2(&stream.state, MAX_WBITS + 16) == Z_OK);
  stream.state.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
  stream.state.avail_in = static_cast<uInt>(data.size());
  std::array<char, 4096> buffer;
  std::string decoded;
  int result;
  do {
    stream.state.next_out = reinterpret_cast<Bytef *>(buffer.data());
    stream.state.avail_out = buffer.size();
    result = ::inflate(&stream.state, Z_NO_FLUSH);
    REQUIRE((result == Z_OK || result == Z_STREAM_END));
    decoded.append(buffer.data(), buffer.size() - stream.state.avail_out);
  } while (result != Z_STREAM_END);
  CHECK(stream.state.avail_in == 0);
  return decoded;
}
#endif
}  // namespace

#ifdef CINATRA_ENABLE_GZIP
TEST_CASE("response writer forwards the requested gzip level") {
  const auto data = make_payload(8192);
  auto writer = [&](auto &stream) {
    stream.append(data);
  };
  for (int level : {0, 3, 9}) {
    CAPTURE(level);
    std::string expected;
    REQUIRE(gzip_codec::compress(data, expected, level));
    for (int api : {0, 1, 2}) {
      CAPTURE(api);
      coro_http_response response(nullptr);
      if (api == 0) {
        response.set_content(writer, content_encoding::gzip, "gzip", level);
      }
      else if (api == 1) {
        response.set_status_and_content(status_type::ok, writer,
                                        content_encoding::gzip, "gzip", level);
      }
      else {
        response.set_status_and_content_view(status_type::ok, writer,
                                             content_encoding::gzip, true,
                                             "gzip", level);
      }
      CHECK(response.content() == expected);
      CHECK(decode_gzip(response.content()) == data);
    }
  }
}

TEST_CASE("gzip stream handles input and output boundaries") {
  for (size_t size : {0u, 1u, 63u, 64u, 16383u, 16384u, 16385u, 262144u}) {
    CAPTURE(size);
    auto data = make_payload(size);
    for (int level : {-1, 0, 1, 9}) {
      CAPTURE(level);
      std::string encoded;
      REQUIRE(gzip_codec::compress(data, encoded, level));
      CHECK(decode_gzip(encoded) == data);

      encoded.clear();
      gzip_codec::compress_stream stream(encoded, level);
      for (char value : data) {
        stream.push_back(value);
      }
      REQUIRE(stream.finish());
      auto finished = encoded;
      CHECK(stream.finish());
      CHECK(encoded == finished);
      CHECK(decode_gzip(encoded) == data);
    }
  }
}

TEST_CASE(
    "gzip stream consumes temporary spans and emits output incrementally") {
  auto data = make_payload(256 * 1024);
  std::string encoded;
  std::string expected;
  gzip_codec::compress_stream stream(encoded);
  stream.append(std::string_view{});
  stream.push_back('[');
  expected.push_back('[');
  for (size_t offset = 0; offset < data.size(); offset += 777) {
    auto temporary = data.substr(offset, 777);
    stream.append(temporary.data(), temporary.size());
    expected.append(temporary);
  }
  CHECK(encoded.size() > 10);  // More than the gzip header, before finish().
  stream.append("]");
  expected.append("]");
  REQUIRE(stream.finish());
  CHECK(decode_gzip(encoded) == expected);
  stream.push_back('!');
  CHECK_FALSE(stream.good());
  CHECK_FALSE(stream.finish());
}

TEST_CASE(
    "gzip stream reports initialization errors and releases on exceptions") {
  std::string encoded;
  int calls = 0;
  auto writer = [&](auto &stream) {
    ++calls;
    stream.append(std::string(65536, 'x'));
    throw std::runtime_error("serialization failed");
  };
  CHECK_FALSE(gzip_codec::compress(writer, encoded, 10));
  CHECK(calls == 0);
  CHECK(encoded.empty());
  CHECK_THROWS_WITH_AS(gzip_codec::compress(writer, encoded),
                       "serialization failed", std::runtime_error);
  CHECK(calls == 1);
  encoded.clear();
  REQUIRE(gzip_codec::compress("after exception", encoded));
  CHECK(decode_gzip(encoded) == "after exception");
}

TEST_CASE("gzip writer preserves overlapping input across output growth") {
  for (size_t size : {0u, 1u, 16383u, 16384u, 262144u}) {
    CAPTURE(size);
    auto data = make_payload(size);
    if (!data.empty()) {
      // The next output write must grow the allocation, independently of the
      // string implementation's initial spare capacity.
      data.resize(data.capacity(), 'x');
    }
    const auto expected = data;
    const auto prefix_size = data.size();
    REQUIRE(gzip_codec::compress(
        [&](auto &stream) {
          stream.append(data);
        },
        data));
    CHECK(data.substr(0, prefix_size) == expected);
    CHECK(decode_gzip(std::string_view(data).substr(prefix_size)) == expected);
  }

  std::string data = "prefix:" + make_payload(65536);
  const auto prefix = data;
  auto input = std::string_view(data).substr(7);
  const std::string expected(input);
  REQUIRE(gzip_codec::compress(
      [&](auto &stream) {
        stream.append(input);
      },
      data));
  CHECK(data.substr(0, prefix.size()) == prefix);
  CHECK(decode_gzip(std::string_view(data).substr(prefix.size())) == expected);

  data = prefix;
  {
    gzip_codec::compress_stream stream(data);
    stream.push_back('[');
    stream.append(std::string_view(data));
    stream.push_back(']');
    REQUIRE(stream.finish());
  }
  CHECK(data.substr(0, prefix.size()) == prefix);
  CHECK(decode_gzip(std::string_view(data).substr(prefix.size())) ==
        "[" + prefix + "]");
}
#endif

TEST_CASE("legacy string calls retain their existing signatures") {
  std::string source(8192, 'v');
  const auto expected = source;
  const auto &constant = source;
  coro_http_response response(nullptr);
  response.set_status_and_content_view<std::string>(
      status_type::ok, source, content_encoding::none, false);
  CHECK(source == expected);
  CHECK(response.content() == expected);
  response.clear();
  response.set_status_and_content_view<const std::string>(
      status_type::ok, constant, content_encoding::none, false);
  CHECK(response.content() == expected);
  response.clear();
  response.set_status_and_content_view<std::string_view>(status_type::ok,
                                                         source);
  std::vector<asio::const_buffer> buffers;
  std::string chunk_size;
  response.to_buffers(buffers, chunk_size);
  REQUIRE_FALSE(buffers.empty());
  CHECK(buffers.back().data() == source.data());
}

TEST_CASE("response header defaults survive reuse and upgrades omit length") {
  coro_http_response response(nullptr, false);
  for (int request = 0; request < 2; ++request) {
    response.set_status_and_content(status_type::ok, "ok");
    CHECK_FALSE(response.need_date());
    auto wire = response_wire(response);
    CHECK(wire.find("Date:") == std::string::npos);
    CHECK(wire.find("Server:") == std::string::npos);
    response.clear();
  }
  response.set_status_and_content(status_type::switching_protocols, "");
  auto wire = response_wire(response);
  CHECK(wire.find("Content-Length:") == std::string::npos);
  wire.clear();
  response.build_resp_str(wire);
  CHECK(wire.find("Content-Length:") == std::string::npos);
  CHECK(wire.find("Date:") == std::string::npos);
  CHECK(wire.find("Server:") == std::string::npos);
}

TEST_CASE("serialization exceptions preserve the previous response") {
  for (auto encoding : {content_encoding::none, content_encoding::gzip}) {
    for (auto accept : {"gzip", "br"}) {
      coro_http_response response(nullptr);
      response.need_date_head(false);
      response.set_status_and_content(status_type::accepted, "previous body");
      const auto previous = response_wire(response);
      auto writer = [](auto &stream) {
        stream.append(std::string(65536, 'x'));
        throw std::runtime_error("serialization failed");
      };
      CHECK_THROWS_WITH_AS(response.set_status_and_content(
                               status_type::ok, writer, encoding, accept),
                           "serialization failed", std::runtime_error);
      CHECK(response.status() == status_type::accepted);
      CHECK(response_wire(response) == previous);
    }
  }
}

TEST_CASE("writer content defaults to OK with optional client encoding") {
  for (auto accept : {"gzip", "br", ""}) {
    CAPTURE(accept);
    coro_http_response response(nullptr);
    auto writer = [](auto &stream) {
      stream.append("{\"answer\":42}");
    };
    if (std::string_view(accept).empty()) {
      response.set_content(writer, content_encoding::gzip);
    }
    else {
      response.set_content(writer, content_encoding::gzip, accept);
    }
    CHECK(response.status() == status_type::ok);
#ifdef CINATRA_ENABLE_GZIP
    if (std::string_view(accept) != "br") {
      CHECK(decode_gzip(response.content()) == "{\"answer\":42}");
    }
    else
#endif
    {
      CHECK(response.content() == "{\"answer\":42}");
    }
    response.clear();
    // The existing explicit-status API remains available.
    response.set_status_and_content(status_type::created, writer,
                                    content_encoding::gzip, "br");
    CHECK(response.status() == status_type::created);
    CHECK(response.content() == "{\"answer\":42}");
  }
  coro_http_response standalone(nullptr);
  standalone.set_content([](auto &stream) {
    stream.append("plain");
  });
  CHECK(standalone.status() == status_type::ok);
  CHECK(standalone.content() == "plain");
}

TEST_CASE("response serializes through the existing content APIs") {
  for (bool view_api : {false, true}) {
    for (auto accept : {"gzip", "br"}) {
      CAPTURE(view_api);
      CAPTURE(accept);
      coro_http_response response(nullptr);
      // A new writer result must replace an old borrowed response body.
      response.set_status_and_content_view(status_type::accepted,
                                           std::string_view("old body"));
      int calls = 0;
      bool string_stream = false;
      auto writer = [&](auto &stream) {
        ++calls;
        string_stream =
            std::is_same_v<std::remove_cvref_t<decltype(stream)>, std::string>;
        stream.append("{\"answer\":");
        stream.push_back('4');
        stream.append("2}");
      };
      if (view_api) {
        response.set_status_and_content_view(
            status_type::ok, writer, content_encoding::gzip, true, accept);
      }
      else {
        response.set_status_and_content(status_type::ok, writer,
                                        content_encoding::gzip, accept);
      }
      CHECK(calls == 1);
      CHECK(response.status() == status_type::ok);
      auto wire = response_wire(response);
      CHECK(wire.ends_with(response.content()));
      CHECK(wire.find(
                "Content-Length: " + std::to_string(response.content_size()) +
                "\r\n") != std::string::npos);
#ifdef CINATRA_ENABLE_GZIP
      if (std::string_view(accept) == "gzip") {
        CHECK_FALSE(string_stream);
        CHECK(decode_gzip(response.content()) == "{\"answer\":42}");
        CHECK(wire.find("Content-Encoding: gzip\r\n") != std::string::npos);
        continue;
      }
#endif
      CHECK(string_stream);
      CHECK(response.content() == "{\"answer\":42}");
      CHECK(wire.find("Content-Encoding:") == std::string::npos);
    }
  }
}

#ifdef CINATRA_TEST_IGUANA
TEST_CASE("iguana writes escaped and unescaped JSON directly to gzip") {
  std::string text = "quote\" slash\\ newline\n tab\t 中文 ";
  text.append("\0\x01\x1f", 3);
  text += make_payload(65536);
  std::vector<gzip_json_record> records{
      {42, text, {0, -1.5, 3.1415926}, {{"key\"", "value\n"}}, true},
      {0, "", {}, {}, false}};
  auto check = [&]<bool escape>() {
    std::string expected;
    iguana::to_json<escape>(records, expected);
    coro_http_response response(nullptr);
    response.set_content(
        [&](auto &stream) {
          iguana::to_json<escape>(records, stream);
        },
        content_encoding::gzip);
#ifdef CINATRA_ENABLE_GZIP
    CHECK(decode_gzip(response.content()) == expected);
#else
    CHECK(response.content() == expected);
#endif
  };
  check.template operator()<true>();
  check.template operator()<false>();
  records.clear();
  check.template operator()<true>();
}
#endif

TEST_CASE("streamed responses own their buffers across coroutine suspension") {
  bool tls = false;
  bool multi_buffer = true;
  SUBCASE("TCP buffers") {}
  SUBCASE("TCP string") { multi_buffer = false; }
#ifdef CINATRA_ENABLE_SSL
  SUBCASE("TLS buffers") { tls = true; }
  SUBCASE("TLS string") {
    tls = true;
    multi_buffer = false;
  }
#endif
  asio::io_context context(1);
  coro_io::ExecutorWrapper<> executor(context.get_executor());
  coro_http_server server(context, 0, "127.0.0.1");
  server.set_default_response_headers(false);
#ifdef CINATRA_ENABLE_SSL
  if (tls) {
    const auto dir =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "include/cinatra";
    server.init_ssl((dir / "server.crt").string(),
                    (dir / "server.key").string(), "test");
  }
#endif
  int handled = 0;
  coro_http_connection *first_connection = nullptr;
  std::array<http_header, 2> json_headers{
      {{"Content-Type", "application/json"}, {"Vary", "Accept-Encoding"}}};
  server.set_http_handler<GET>(
      "/json",
      [&](coro_http_request &request,
          coro_http_response &response) -> async_simple::coro::Lazy<void> {
        if (first_connection == nullptr) {
          first_connection = request.get_conn();
        }
        CHECK(request.get_conn() == first_connection);
        request.get_conn()->set_multi_buf(multi_buffer);
        response.add_header_span(json_headers);
        {
          auto source = co_await coro_io::post([] {
            return make_payload(256 * 1024);
          });
          CHECK(context.get_executor().running_in_this_thread());
          response.set_content(
              [&](auto &stream) {
                stream.append("{\"data\":\"");
                stream.append(source.value());
                stream.append("\"}");
              },
              content_encoding::gzip, request.get_accept_encoding());
        }
        // The source and writer have been destroyed before another suspension
        // and before the connection starts sending the response.
        co_await coro_io::post([] {
        });
        CHECK(context.get_executor().running_in_this_thread());
        ++handled;
      });
  server.set_http_handler<POST>(
      "/echo",
      [&](coro_http_request &request,
          coro_http_response &response) -> async_simple::coro::Lazy<void> {
        CHECK(request.get_conn() == first_connection);
        request.get_conn()->set_multi_buf(multi_buffer);
        co_await coro_io::post([] {
        });
        CHECK(context.get_executor().running_in_this_thread());
        response.add_header("Content-Type", "application/octet-stream");
        response.set_status_and_content_view(status_type::ok,
                                             request.get_body());
        ++handled;
      });
  auto serving = server.async_start();
  REQUIRE(server.port() != 0);
  coro_http_client client(context.get_executor());
  const auto base_uri = std::string(tls ? "https" : "http") +
                        "://127.0.0.1:" + std::to_string(server.port());
  const auto uri = base_uri + "/json";
  const auto echo_uri = base_uri + "/echo";
  const std::string echo_body("echo\0payload", 12);
  const auto expected = "{\"data\":\"" + make_payload(256 * 1024) + "\"}";
  bool completed = false;
  bool timed_out = false;
  std::exception_ptr error;
  std::promise<void> completion;
  auto completed_request = completion.get_future();
  asio::steady_timer deadline(context);
  deadline.expires_after(10s);
  deadline.async_wait([&](std::error_code ec) {
    if (!ec) {
      timed_out = true;
      client.close();
    }
  });
  auto requests = [&]() -> async_simple::coro::Lazy<void> {
    for (auto accept : {"gzip", "br", "gzip"}) {
      client.add_header("Accept-Encoding", accept);
      auto result = co_await client.async_get(uri);
      REQUIRE_FALSE(result.net_err);
      CHECK(result.status == 200);
      CHECK(result.resp_body == expected);
      int json_content_types = 0;
      int json_vary = 0;
      for (const auto &[key, value] : result.resp_headers) {
        CHECK_FALSE((iequal0(key, "Date") || iequal0(key, "Server")));
        if (iequal0(key, "Content-Type")) {
          ++json_content_types;
          CHECK(value == "application/json");
        }
        if (iequal0(key, "Vary")) {
          ++json_vary;
          CHECK(value == "Accept-Encoding");
        }
      }
      CHECK(json_content_types == 1);
      CHECK(json_vary == 1);
#ifdef CINATRA_ENABLE_GZIP
      bool compressed = false;
      for (const auto &[key, value] : result.resp_headers) {
        if (iequal0(key, "Content-Encoding")) {
          compressed = value == "gzip";
        }
      }
      CHECK(compressed == (std::string_view(accept) == "gzip"));
#endif
      auto echo = co_await client.async_post(echo_uri, echo_body,
                                             req_content_type::string);
      REQUIRE_FALSE(echo.net_err);
      CHECK(echo.status == 200);
      CHECK(echo.resp_body == echo_body);
      int echo_content_types = 0;
      int echo_vary = 0;
      for (const auto &[key, value] : echo.resp_headers) {
        CHECK_FALSE((iequal0(key, "Date") || iequal0(key, "Server")));
        if (iequal0(key, "Content-Type")) {
          ++echo_content_types;
          CHECK(value == "application/octet-stream");
        }
        if (iequal0(key, "Vary")) {
          ++echo_vary;
        }
      }
      CHECK(echo_content_types == 1);
      CHECK(echo_vary == 0);
    }
  };
  requests().via(&executor).start([&](auto &&result) {
    if (result.hasError()) {
      error = result.getException();
    }
    completed = true;
    client.close();
    deadline.cancel();
    completion.set_value();
  });
  std::thread runner([&] {
    context.run();
  });
  completed_request.wait();
  // stop() waits for the accept loop, so invoke it outside the I/O thread.
  server.stop();
  runner.join();
  CHECK_FALSE(timed_out);
  CHECK(completed);
  CHECK(handled == 6);
  if (error) {
    std::rethrow_exception(error);
  }
}
