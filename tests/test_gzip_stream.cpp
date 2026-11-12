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
concept response_accepts_content =
    requires(coro_http_response &response, Content content) {
      response.set_status_and_content(status_type::ok, content);
      response.set_status_and_content_view(status_type::ok, content);
    };

struct string_writer {
  void operator()(std::string &) const {}
};

static_assert(response_accepts_content<std::string>);
static_assert(response_accepts_content<std::string_view>);
template <typename Content>
concept response_accepts_explicit_lvalue =
    requires(coro_http_response &response, Content &content) {
      response.set_status_and_content_view<Content>(status_type::ok, content);
    };
static_assert(response_accepts_explicit_lvalue<std::string>);
static_assert(response_accepts_explicit_lvalue<const std::string>);
static_assert(response_accepts_explicit_lvalue<std::string_view>);
#ifdef CINATRA_ENABLE_GZIP
struct gzip_writer {
  void operator()(gzip_codec::compress_stream &) const {}
};
static_assert(!response_accepts_content<gzip_writer>);
static_assert(!response_accepts_content<string_writer>);
#else
static_assert(response_accepts_content<string_writer>);
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

TEST_CASE("gzip stream preserves overlapping input across output growth") {
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
    REQUIRE(gzip_codec::compress(data, data));
    CHECK(data.substr(0, prefix_size) == expected);
    CHECK(decode_gzip(std::string_view(data).substr(prefix_size)) == expected);
  }

  std::string data = "prefix:" + make_payload(65536);
  const auto prefix = data;
  auto input = std::string_view(data).substr(7);
  const std::string expected(input);
  REQUIRE(gzip_codec::compress(input, data));
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

TEST_CASE("response views borrow lvalues and retain temporary strings") {
  for (size_t size : {2u, 8192u}) {
    for (auto encoding : {content_encoding::none, content_encoding::gzip,
                          content_encoding::deflate}) {
      CAPTURE(size);
      CAPTURE(static_cast<int>(encoding));
      coro_http_response response(nullptr);
      std::string source(size, 'v');
      response.set_status_and_content_view(status_type::ok, source, encoding,
                                           true, "br");
      source.front() = 'z';
      std::vector<asio::const_buffer> buffers;
      std::string chunk_size;
      response.to_buffers(buffers, chunk_size);
      REQUIRE_FALSE(buffers.empty());
      CHECK(buffers.back().data() == source.data());
      CHECK(buffers.back().size() == source.size());
      CHECK(response_wire(response).ends_with(source));

      response.clear();
      const auto &const_source = source;
      response.set_status_and_content_view(status_type::ok, const_source);
      CHECK(response_wire(response).ends_with(source));

      response.clear();
      const auto expected = source;
      response.set_status_and_content_view(status_type::ok, source,
                                           content_encoding::none, false);
      CHECK(source == expected);  // Owning an lvalue must copy, not move it.
      CHECK(response.content() == expected);

      response.clear();
      response.set_status_and_content_view(status_type::ok,
                                           std::string(expected));
      CHECK(response.content() == expected);
      CHECK(response_wire(response).ends_with(expected));
    }
  }
}

TEST_CASE("serialization exceptions preserve the previous response") {
  for (auto encoding : {content_encoding::none, content_encoding::gzip,
                        content_encoding::deflate, content_encoding::br}) {
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

TEST_CASE("explicit content template arguments retain by-value compatibility") {
  for (bool is_view : {false, true}) {
    for (size_t size : {2u, 8192u}) {
      CAPTURE(is_view);
      CAPTURE(size);
      std::string source(size, 'v');
      const auto expected = source;
      coro_http_response response(nullptr);
      response.set_status_and_content_view<std::string>(
          status_type::ok, source, content_encoding::none, is_view);
      CHECK(source == expected);
      source.front() = 'x';
      CHECK(response.content() == expected);
      CHECK(response_wire(response).ends_with(expected));

      response.clear();
      const auto &constant = expected;
      response.set_status_and_content_view<std::string>(
          status_type::ok, constant, content_encoding::none, is_view);
      CHECK(response.content() == expected);

      response.clear();
      response.set_status_and_content_view<std::string_view>(status_type::ok,
                                                             constant);
      std::vector<asio::const_buffer> buffers;
      std::string chunk_size;
      response.to_buffers(buffers, chunk_size);
      REQUIRE_FALSE(buffers.empty());
      CHECK(buffers.back().data() == constant.data());

      response.clear();
      response.set_status_and_content_view<std::string>(status_type::ok,
                                                        std::string(expected));
      CHECK(response.content() == expected);
    }
  }
}

TEST_CASE("response serializes through the existing content APIs") {
  for (bool view_api : {false, true}) {
    for (auto accept : {"gzip", "br"}) {
      CAPTURE(view_api);
      CAPTURE(accept);
      coro_http_response response(nullptr);
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

#ifdef CINATRA_ENABLE_GZIP
TEST_CASE("response encoding negotiation honors tokens and quality values") {
  struct encoding_case {
    std::string_view accept;
    bool accepted;
  };
  for (auto encoding : {content_encoding::gzip, content_encoding::deflate}) {
    const auto name = encoding == content_encoding::gzip ? "gzip" : "deflate";
    const std::vector<encoding_case> cases{
        {"", true},
        {name, true},
        {"gzip, deflate", true},
        {"GZIP, DEFLATE", true},
        {"gzip;q=0, deflate;q=0", false},
        {"gzip;q=0.000, deflate;q=0.000", false},
        {"gzip;q=0.001, deflate;q=0.001", true},
        {"gzip;Q=1.000, deflate;Q=1.000", true},
        {" \t gzip ; q=0.5 , deflate ; q=0.5 \t", true},
        {"gzip;q=0., deflate;q=0.", false},
        {"gzip;q=1., deflate;q=1.", true},
        {"xgzip, xdeflate", false},
        {"x-gzip", encoding == content_encoding::gzip},
        {"X-GZIP;q=0", false},
        {"gzip2, deflate2", false},
        {"br", false},
        {"*;q=0.5", true},
        {"*;q=0", false},
        {"*;q=1, gzip;q=0, deflate;q=0", false},
        {"gzip;q=0, deflate;q=0, *;q=1", false},
        {"*;q=0, gzip;q=0.1, deflate;q=0.1", true},
        {"gzip;q=1, deflate;q=1, gzip;q=0, deflate;q=0", false},
        {"gzip;q=0, deflate;q=0, gzip;q=1, deflate;q=1", false},
        {"gzip;q=, deflate;q=", false},
        {"gzip;q=2, deflate;q=2", false},
        {"gzip;q=1.001, deflate;q=1.001", false},
        {"gzip;q=0.0001, deflate;q=0.0001", false},
        {"gzip;q=nan, deflate;q=nan", false},
        {"gzip;q=0;foo=1, deflate;q=0;foo=1", false},
        {"gzip;foo=1, deflate;foo=1", false},
        {" , \t, ", false},
    };
    for (const auto &[accept, accepted] : cases) {
      for (bool writer_api : {false, true}) {
        CAPTURE(name);
        CAPTURE(accept);
        CAPTURE(writer_api);
        coro_http_response response(nullptr);
        int calls = 0;
        if (writer_api) {
          response.set_status_and_content(
              status_type::ok,
              [&](auto &stream) {
                ++calls;
                stream.append("{\"answer\":42}");
              },
              encoding, accept);
          CHECK(calls == 1);
        }
        else {
          response.set_status_and_content(status_type::ok, "{\"answer\":42}",
                                          encoding, accept);
        }
        CHECK(response.status() == status_type::ok);
        const auto wire = response_wire(response);
        CHECK((wire.find(std::string("Content-Encoding: ") + name + "\r\n") !=
               std::string::npos) == accepted);
        if (!accepted) {
          CHECK(response.content() == "{\"answer\":42}");
        }
        else if (encoding == content_encoding::gzip) {
          CHECK(decode_gzip(response.content()) == "{\"answer\":42}");
        }
        else {
          std::string decoded;
          REQUIRE(gzip_codec::inflate(response.content(), decoded));
          CHECK(decoded == "{\"answer\":42}");
        }
      }
    }
  }
}
#endif

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
    response.set_status_and_content(
        status_type::ok,
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
  server.set_http_handler<GET>(
      "/json",
      [&](coro_http_request &request,
          coro_http_response &response) -> async_simple::coro::Lazy<void> {
        request.get_conn()->set_multi_buf(multi_buffer);
        {
          auto source = co_await coro_io::post([] {
            return make_payload(256 * 1024);
          });
          CHECK(context.get_executor().running_in_this_thread());
          response.set_status_and_content(
              status_type::ok,
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
  auto serving = server.async_start();
  REQUIRE(server.port() != 0);
  coro_http_client client(context.get_executor());
  const auto uri = std::string(tls ? "https" : "http") +
                   "://127.0.0.1:" + std::to_string(server.port()) + "/json";
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
    for (auto accept : {"gzip", "br"}) {
      client.add_header("Accept-Encoding", accept);
      auto result = co_await client.async_get(uri);
      REQUIRE_FALSE(result.net_err);
      CHECK(result.status == 200);
      CHECK(result.resp_body == expected);
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
  CHECK(handled == 2);
  if (error) {
    std::rethrow_exception(error);
  }
}
