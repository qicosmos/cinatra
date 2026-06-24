#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <async_simple/coro/Collect.h>
#include <async_simple/coro/SyncAwait.h>

#include <algorithm>
#include <array>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cinatra/coro_http_server.hpp"
#include "cinatra/http2/frame.hpp"
#include "cinatra/http2/h2_client.hpp"
#include "cinatra/http2/h2_connection.hpp"
#include "cinatra/http2/hpack.hpp"
#include "doctest/doctest.h"

using namespace cinatra::http2;

static void set_test_socket_timeouts(asio::ip::tcp::socket& sock,
                                     int timeout_ms = 2000);
static void connect_with_retry(asio::ip::tcp::socket& sock, uint16_t port,
                               int retries = 10);
static void connect_direct(asio::ip::tcp::socket& sock, uint16_t port);

#ifdef CINATRA_ENABLE_SSL
static std::string resolve_test_tls_asset(std::string_view filename) {
  const std::array<std::filesystem::path, 4> candidates{
      std::filesystem::path("include/cinatra") / filename,
      std::filesystem::path("../include/cinatra") / filename,
      std::filesystem::path("../../include/cinatra") / filename,
      std::filesystem::path("../../../include/cinatra") / filename,
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate.lexically_normal().string();
    }
  }
  return std::string((std::filesystem::path("include/cinatra") / filename)
                         .lexically_normal()
                         .string());
}

static std::string test_tls_cert_path() {
  static const std::string path = resolve_test_tls_asset("server.crt");
  return path;
}

static std::string test_tls_key_path() {
  static const std::string path = resolve_test_tls_asset("server.key");
  return path;
}

static std::unique_ptr<asio::ssl::context> make_test_server_ssl_context(
    bool enable_h2_alpn = true) {
  auto ctx = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
  ctx->set_options(
      asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
      asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
      asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
  ctx->set_password_callback([](auto, auto) {
    return std::string("test");
  });
  ctx->use_certificate_chain_file(test_tls_cert_path());
  ctx->use_private_key_file(test_tls_key_path(), asio::ssl::context::pem);
  if (enable_h2_alpn) {
    SSL_CTX_set_alpn_select_cb(ctx->native_handle(), select_h2_alpn_callback,
                               nullptr);
  }
  return ctx;
}
#endif

// ---------------------------------------------------------------------------
// Frame tests
// ---------------------------------------------------------------------------

TEST_CASE("frame header roundtrip") {
  frame_header orig{
      .length = 0x123456,
      .type = frame_type::headers,
      .flags = flags::END_HEADERS | flags::END_STREAM,
      .stream_id = 0x7FFFFFFF,
  };
  auto bytes = serialize_frame_header(orig);
  CHECK(bytes.size() == 9);

  auto parsed = parse_frame_header(bytes);
  CHECK(parsed.length == orig.length);
  CHECK(parsed.type == orig.type);
  CHECK(parsed.flags == orig.flags);
  CHECK(parsed.stream_id == orig.stream_id);
}

TEST_CASE("frame header R-bit is masked") {
  // Bit 31 of stream_id is reserved and must be ignored on receive
  frame_header h{
      .length = 0, .type = frame_type::data, .flags = 0, .stream_id = 1};
  auto bytes = serialize_frame_header(h);
  // Force R-bit on in raw bytes
  bytes[5] |= 0x80;
  auto parsed = parse_frame_header(bytes);
  CHECK(parsed.stream_id == 1);  // R-bit masked out
}

TEST_CASE("make_frame produces correct bytes") {
  std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
  auto frame = make_frame(frame_type::data, flags::END_STREAM, 1, payload);

  CHECK(frame.size() == 9 + 4);
  // Length = 4
  CHECK((uint8_t)frame[0] == 0);
  CHECK((uint8_t)frame[1] == 0);
  CHECK((uint8_t)frame[2] == 4);
  // Type = DATA
  CHECK((uint8_t)frame[3] == 0x0);
  // Flags = END_STREAM
  CHECK((uint8_t)frame[4] == flags::END_STREAM);
  // Stream id = 1
  CHECK((uint8_t)frame[8] == 1);
  // Payload
  CHECK((uint8_t)frame[9] == 0xDE);
  CHECK((uint8_t)frame[12] == 0xEF);
}

TEST_CASE("SETTINGS frame build and parse") {
  std::array<settings_entry, 2> entries{
      settings_entry{settings_param::max_frame_size, 65535},
      settings_entry{settings_param::initial_window_size, 131072},
  };
  auto frame = make_settings_frame(entries);

  // Frame header checks
  auto hdr = parse_frame_header(std::span<const uint8_t, 9>(
      reinterpret_cast<const uint8_t*>(frame.data()), 9));
  CHECK(hdr.type == frame_type::settings);
  CHECK(hdr.flags == 0);
  CHECK(hdr.stream_id == 0);
  CHECK(hdr.length == 12);  // 2 entries x 6 bytes

  // Payload parsing
  auto payload = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(frame.data()) + 9, hdr.length);
  auto parsed = parse_settings_payload(payload);
  REQUIRE(parsed.size() == 2);
  CHECK(parsed[0].id == settings_param::max_frame_size);
  CHECK(parsed[0].value == 65535);
  CHECK(parsed[1].id == settings_param::initial_window_size);
  CHECK(parsed[1].value == 131072);
}

TEST_CASE("SETTINGS ACK is empty with ACK flag") {
  auto frame = make_settings_frame({}, true);
  auto hdr = parse_frame_header(std::span<const uint8_t, 9>(
      reinterpret_cast<const uint8_t*>(frame.data()), 9));
  CHECK(hdr.length == 0);
  CHECK(hdr.flags == flags::ACK);
}

TEST_CASE("make_rst_stream") {
  auto frame = make_rst_stream(5, h2_error_code::cancel);
  auto hdr = parse_frame_header(std::span<const uint8_t, 9>(
      reinterpret_cast<const uint8_t*>(frame.data()), 9));
  CHECK(hdr.type == frame_type::rst_stream);
  CHECK(hdr.stream_id == 5);
  CHECK(hdr.length == 4);
  auto* p = reinterpret_cast<const uint8_t*>(frame.data()) + 9;
  uint32_t code = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                  (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  CHECK(code == uint32_t(h2_error_code::cancel));
}

TEST_CASE("CLIENT_PREFACE is 24 bytes") {
  CHECK(CLIENT_PREFACE.size() == 24);
  CHECK(CLIENT_PREFACE.substr(0, 3) == "PRI");
}

// ---------------------------------------------------------------------------
// HPACK tests
// ---------------------------------------------------------------------------

TEST_CASE("hpack decode: indexed field from static table") {
  // Index 2 = :method GET  (0x82 = 0b10000010)
  std::array<uint8_t, 1> block{0x82};
  hpack_decoder dec;
  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == ":method");
  CHECK(hdrs[0].value == "GET");
}

TEST_CASE("hpack decode: multiple static indexed fields") {
  // 0x82 = :method GET, 0x84 = :path /, 0x87 = :scheme https
  std::array<uint8_t, 3> block{0x82, 0x84, 0x87};
  hpack_decoder dec;
  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 3);
  CHECK(hdrs[0].name == ":method");
  CHECK(hdrs[0].value == "GET");
  CHECK(hdrs[1].name == ":path");
  CHECK(hdrs[1].value == "/");
  CHECK(hdrs[2].name == ":scheme");
  CHECK(hdrs[2].value == "https");
}

TEST_CASE("hpack decode: literal with incremental indexing, new name+value") {
  // 0x40 = literal with incremental indexing, index=0 (new name)
  // name  = "x-custom"  (length 8, no huffman)
  // value = "hello"     (length 5, no huffman)
  std::vector<uint8_t> block;
  block.push_back(0x40);  // literal, incremental, new name
  block.push_back(0x08);  // name length = 8
  for (char c : std::string("x-custom")) block.push_back(uint8_t(c));
  block.push_back(0x05);  // value length = 5
  for (char c : std::string("hello")) block.push_back(uint8_t(c));

  hpack_decoder dec;
  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "x-custom");
  CHECK(hdrs[0].value == "hello");
}

TEST_CASE("hpack decode: literal incremental indexing, indexed name") {
  // 0x5C = 0b01011100 = literal incr. indexing, name index 28 (:content-length)
  // Actually index 28 is "content-length"
  // 0x40 | 28 = 0x5C
  std::vector<uint8_t> block;
  block.push_back(0x40 |
                  28);    // literal incr., name = static[28] = content-length
  block.push_back(0x02);  // value length = 2
  block.push_back('4');
  block.push_back('2');

  hpack_decoder dec;
  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "content-length");
  CHECK(hdrs[0].value == "42");
}

TEST_CASE("hpack decode: literal without indexing") {
  // 0x00 = literal without indexing, index=0 (new name)
  std::vector<uint8_t> block;
  block.push_back(0x00);  // no indexing, new name
  block.push_back(0x04);
  for (char c : std::string("host")) block.push_back(uint8_t(c));
  block.push_back(0x09);
  for (char c : std::string("localhost")) block.push_back(uint8_t(c));

  hpack_decoder dec;
  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "host");
  CHECK(hdrs[0].value == "localhost");
}

TEST_CASE("hpack dynamic table is populated by incremental indexing") {
  hpack_decoder dec;
  // First add "x-foo: bar" via literal with incremental indexing
  std::vector<uint8_t> block;
  block.push_back(0x40);
  block.push_back(0x05);
  for (char c : std::string("x-foo")) block.push_back(uint8_t(c));
  block.push_back(0x03);
  for (char c : std::string("bar")) block.push_back(uint8_t(c));

  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "x-foo");

  // Now reference it via dynamic table index = 62
  std::array<uint8_t, 1> block2{0x80 | 62};  // indexed, index 62
  auto hdrs2 = dec.decode(block2);
  REQUIRE(hdrs2.size() == 1);
  CHECK(hdrs2[0].name == "x-foo");
  CHECK(hdrs2[0].value == "bar");
}

TEST_CASE("hpack Huffman codec matches RFC example") {
  const std::string text = "www.example.com";
  const std::vector<uint8_t> expected{
      0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
  };

  auto encoded = huffman_encode(text);
  CHECK(encoded == expected);
  CHECK(huffman_decode(encoded) == text);
}

TEST_CASE("hpack decode_string accepts Huffman-encoded strings") {
  std::span<const uint8_t> buf;
  std::vector<uint8_t> encoded = {
      0x8c,  // Huffman flag + length 12
      0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
  };
  buf = encoded;

  CHECK(decode_string(buf) == "www.example.com");
  CHECK(buf.empty());
}

TEST_CASE("hpack encode_string prefers Huffman when shorter") {
  std::vector<uint8_t> out;
  encode_string(out, "www.example.com");

  REQUIRE(out.size() == 13);
  CHECK(out[0] == 0x8c);
  CHECK(std::vector<uint8_t>(out.begin() + 1, out.end()) ==
        std::vector<uint8_t>{
            0xf1,
            0xe3,
            0xc2,
            0xe5,
            0xf2,
            0x3a,
            0x6b,
            0xa0,
            0xab,
            0x90,
            0xf4,
            0xff,
        });
}

TEST_CASE(
    "hpack encode_string keeps plain literal when Huffman is not smaller") {
  std::vector<uint8_t> out;
  encode_string(out, "a");

  REQUIRE(out.size() == 2);
  CHECK(out[0] == 0x01);
  CHECK(std::string(out.begin() + 1, out.end()) == "a");
}

TEST_CASE("hpack integer encoding: single byte") {
  std::vector<uint8_t> out;
  encode_integer(out, 10, 5, 0);
  REQUIRE(out.size() == 1);
  CHECK(out[0] == 10);
}

TEST_CASE("hpack integer encoding: multi-byte") {
  // Encode 1337 with 5-bit prefix (RFC 7541 section C.1.2 example)
  std::vector<uint8_t> out;
  encode_integer(out, 1337, 5, 0);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == 31);   // 2^5 - 1 = prefix exhausted
  CHECK(out[1] == 154);  // (1337 - 31) & 0x7f | 0x80
  CHECK(out[2] == 10);   // remainder
}

TEST_CASE("hpack integer decode roundtrip") {
  for (uint32_t val : {0u, 1u, 30u, 31u, 127u, 128u, 255u, 1337u, 65535u}) {
    std::vector<uint8_t> encoded;
    encode_integer(encoded, val, 5, 0);
    std::span<const uint8_t> sp(encoded);
    uint32_t decoded = decode_integer(sp, 5);
    CHECK(decoded == val);
    CHECK(sp.empty());
  }
}

TEST_CASE("hpack encode + decode roundtrip") {
  std::vector<header_field> input{
      {":method", "GET"},         {":path", "/hello"},
      {":scheme", "https"},       {":authority", "example.com"},
      {"user-agent", "test/1.0"},
  };

  hpack_encoder enc;
  auto block = enc.encode(input);

  hpack_decoder dec;
  auto output = dec.decode(block);

  REQUIRE(output.size() == input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(output[i].name == input[i].name);
    CHECK(output[i].value == input[i].value);
  }
}

TEST_CASE("hpack encoder uses static table indexed form") {
  hpack_encoder enc;
  // :method GET = static index 2 -> should encode as single byte 0x82
  std::vector<header_field> input{{":method", "GET"}};
  auto block = enc.encode(input);
  REQUIRE(block.size() == 1);
  CHECK(block[0] == 0x82);
}

TEST_CASE("hpack encoder uses indexed name from static table") {
  hpack_encoder enc;
  // :method has static index 2 (GET) but value is POST = static index 3
  std::vector<header_field> input{{":method", "POST"}};
  auto block = enc.encode(input);
  REQUIRE(block.size() == 1);
  CHECK(block[0] == 0x83);  // index 3 = :method POST
}

TEST_CASE("nghttp2-inspired frame: PING ACK payload roundtrip") {
  std::array<uint8_t, 8> ping_data{1, 2, 3, 4, 5, 6, 7, 8};
  auto frame = make_ping_ack(ping_data);
  auto hdr = parse_frame_header(std::span<const uint8_t, 9>(
      reinterpret_cast<const uint8_t*>(frame.data()), 9));

  CHECK(hdr.type == frame_type::ping);
  CHECK(hdr.flags == flags::ACK);
  CHECK(hdr.stream_id == 0);
  CHECK(hdr.length == 8);

  auto payload = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(frame.data()) + 9, 8);
  CHECK(std::vector<uint8_t>(payload.begin(), payload.end()) ==
        std::vector<uint8_t>(ping_data.begin(), ping_data.end()));
}

TEST_CASE(
    "nghttp2-inspired frame: GOAWAY payload encodes last stream and error") {
  auto frame = make_goaway(0x7fffffffu, h2_error_code::protocol_error);
  auto hdr = parse_frame_header(std::span<const uint8_t, 9>(
      reinterpret_cast<const uint8_t*>(frame.data()), 9));
  REQUIRE(hdr.type == frame_type::goaway);
  REQUIRE(hdr.length == 8);

  auto payload = reinterpret_cast<const uint8_t*>(frame.data()) + 9;
  uint32_t last_stream_id = ((uint32_t(payload[0]) & 0x7f) << 24) |
                            (uint32_t(payload[1]) << 16) |
                            (uint32_t(payload[2]) << 8) | uint32_t(payload[3]);
  uint32_t error_code = (uint32_t(payload[4]) << 24) |
                        (uint32_t(payload[5]) << 16) |
                        (uint32_t(payload[6]) << 8) | uint32_t(payload[7]);
  CHECK(last_stream_id == 0x7fffffffu);
  CHECK(error_code == uint32_t(h2_error_code::protocol_error));
}

TEST_CASE("nghttp2-inspired frame: WINDOW_UPDATE payload encodes increment") {
  auto frame = make_window_update(13, 4096);
  auto hdr = parse_frame_header(std::span<const uint8_t, 9>(
      reinterpret_cast<const uint8_t*>(frame.data()), 9));
  REQUIRE(hdr.type == frame_type::window_update);
  REQUIRE(hdr.stream_id == 13);
  REQUIRE(hdr.length == 4);

  auto payload = reinterpret_cast<const uint8_t*>(frame.data()) + 9;
  uint32_t increment = ((uint32_t(payload[0]) & 0x7f) << 24) |
                       (uint32_t(payload[1]) << 16) |
                       (uint32_t(payload[2]) << 8) | uint32_t(payload[3]);
  CHECK(increment == 4096);
}

TEST_CASE("nghttp2-inspired hpack: indexed index 0 is invalid") {
  hpack_decoder dec;
  std::array<uint8_t, 1> block{0x80};
  CHECK_THROWS(dec.decode(block));
}

TEST_CASE(
    "nghttp2-inspired hpack: literal without indexing does not populate "
    "dynamic table") {
  hpack_decoder dec;
  std::vector<uint8_t> block;
  encode_integer(block, 58, 4,
                 0x00);  // user-agent from static table, no indexing
  encode_string(block, "nghttp2");

  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "user-agent");
  CHECK(hdrs[0].value == "nghttp2");

  std::array<uint8_t, 1> dyn_idx{0x80 | 62};
  CHECK_THROWS(dec.decode(dyn_idx));
}

TEST_CASE(
    "nghttp2-inspired hpack: new name without indexing does not populate "
    "dynamic table") {
  hpack_decoder dec;
  std::vector<uint8_t> block;
  block.push_back(0x00);
  encode_string(block, "x");
  encode_string(block, "y");

  auto hdrs = dec.decode(block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "x");
  CHECK(hdrs[0].value == "y");

  std::array<uint8_t, 1> dyn_idx{0x80 | 62};
  CHECK_THROWS(dec.decode(dyn_idx));
}

TEST_CASE(
    "nghttp2-inspired hpack: sequential deflate inflate roundtrip preserves "
    "header sets") {
  hpack_encoder enc;
  hpack_decoder dec;

  const std::vector<std::vector<header_field>> sets{
      {
          {":status", "200"},
          {"cache-control", "private, max-age=0, must-revalidate"},
          {"content-length", "76073"},
          {"content-type", "text/html"},
          {"server", "Apache"},
      },
      {
          {":status", "304"},
          {"age", "0"},
          {"cache-control", "max-age=56682045"},
          {"content-type", "text/css"},
          {"vary", "Accept-Encoding"},
      },
      {
          {":status", "304"},
          {"age", "0"},
          {"cache-control", "max-age=31536000"},
          {"content-type", "application/javascript"},
          {"etag", "\"6807-4dc5b54e0dcc0\""},
      },
  };

  for (auto& input : sets) {
    auto block = enc.encode(input);
    auto output = dec.decode(block);
    REQUIRE(output.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
      CHECK(output[i].name == input[i].name);
      CHECK(output[i].value == input[i].value);
    }
  }
}

TEST_CASE(
    "nghttp2-inspired hpack: zero-length Huffman string decodes to empty") {
  std::span<const uint8_t> buf;
  std::array<uint8_t, 1> encoded{0x80};
  buf = encoded;

  CHECK(decode_string(buf).empty());
  CHECK(buf.empty());
}

TEST_CASE(
    "nghttp2-inspired hpack: table size update to zero evicts dynamic "
    "entries") {
  hpack_decoder dec;

  std::vector<uint8_t> add_block;
  add_block.push_back(0x40);
  encode_string(add_block, "x-foo");
  encode_string(add_block, "bar");
  auto hdrs = dec.decode(add_block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "x-foo");

  std::vector<uint8_t> resize_block;
  encode_integer(resize_block, 0, 5, 0x20);
  auto resized = dec.decode(resize_block);
  CHECK(resized.empty());

  std::array<uint8_t, 1> dyn_idx{0x80 | 62};
  CHECK_THROWS(dec.decode(dyn_idx));
}

TEST_CASE(
    "nghttp2-inspired hpack: oversized indexed entry is not inserted after "
    "table shrink") {
  hpack_decoder dec;
  std::vector<uint8_t> resize_block;
  encode_integer(resize_block, 32, 5, 0x20);
  auto resized = dec.decode(resize_block);
  CHECK(resized.empty());

  std::vector<uint8_t> add_block;
  add_block = {0x40, 0x05, 'a', 'l', 'p', 'h', 'a', 0x01, '0'};
  auto hdrs = dec.decode(add_block);
  REQUIRE(hdrs.size() == 1);
  CHECK(hdrs[0].name == "alpha");

  std::array<uint8_t, 1> dyn_idx{0x80 | 62};
  CHECK_THROWS(dec.decode(dyn_idx));
}

TEST_CASE("nghttp2-inspired hpack: table size update after header is invalid") {
  hpack_decoder dec;
  std::array<uint8_t, 2> block{0x82, 0x20};
  CHECK_THROWS(dec.decode(block));
}

TEST_CASE(
    "nghttp2-inspired hpack: table size update beyond configured limit is "
    "invalid") {
  hpack_decoder dec;
  dec.set_max_dynamic_table_size(111);

  std::vector<uint8_t> block;
  encode_integer(block, 112, 5, 0x20);
  CHECK_THROWS(dec.decode(block));
}

TEST_CASE("nghttp2-inspired hpack: encoder emits pending table size update") {
  hpack_encoder enc;
  enc.set_max_dynamic_table_size(0);

  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", "/"},
  };
  auto block = enc.encode(hdrs);

  REQUIRE(!block.empty());
  CHECK((block[0] & 0xe0) == 0x20);
}

// ---------------------------------------------------------------------------
// Integration test helpers
// ---------------------------------------------------------------------------

// Server runner: ioc_thread drives ASIO; conn_thread runs the connection
// coroutine via syncAwait (blocks until the connection closes).
struct server_runner {
  asio::io_context ioc;
  std::unique_ptr<coro_io::ExecutorWrapper<>> exec;
  asio::executor_work_guard<asio::io_context::executor_type> work{
      asio::make_work_guard(ioc)};
  std::thread ioc_thread;
  std::thread conn_thread;
  std::shared_ptr<asio::ip::tcp::acceptor> acceptor;
  std::shared_ptr<coro_http2_connection> active_conn;
  std::mutex active_conn_mtx;
  std::atomic<bool> ioc_done = false;
  bool enable_connect_protocol = false;

  server_runner() {
    exec = std::make_unique<coro_io::ExecutorWrapper<>>(ioc.get_executor());
    ioc_thread = std::thread([this] {
      ioc.run();
      ioc_done = true;
    });
  }

  void launch(h2_handler handler) {
    acceptor = std::make_shared<asio::ip::tcp::acceptor>(
        ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
    port_ = acceptor->local_endpoint().port();

    auto exec_ptr = exec.get();
    auto acc = acceptor;
    conn_thread = std::thread(
        [this, acc, handler = std::move(handler), exec_ptr]() mutable {
          asio::ip::tcp::socket sock(ioc);
          std::error_code ec;
          acc->accept(sock, ec);
          if (ec)
            return;
          set_test_socket_timeouts(sock);
          auto conn = std::make_shared<coro_http2_connection>(
              std::move(sock), std::move(handler), exec_ptr);
          conn->set_enable_connect_protocol(enable_connect_protocol);
          {
            std::scoped_lock lock(active_conn_mtx);
            active_conn = conn;
          }
          async_simple::coro::syncAwait(conn->start().via(exec_ptr));
        });
  }

  uint16_t port() const { return port_; }

  void set_enable_connect_protocol(bool enabled) {
    enable_connect_protocol = enabled;
  }

  void stop() {
    if (acceptor) {
      std::error_code ignored;
      acceptor->cancel(ignored);
      acceptor->close(ignored);
    }
    for (int i = 0; i < 20; ++i) {
      std::shared_ptr<coro_http2_connection> conn;
      {
        std::scoped_lock lock(active_conn_mtx);
        conn = active_conn;
      }
      if (conn) {
        conn->close();
        break;
      }
      if (!conn_thread.joinable())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (conn_thread.joinable())
      conn_thread.join();
    work.reset();
    for (int i = 0; i < 400 && !ioc_done.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!ioc_done.load())
      ioc.stop();
    if (ioc_thread.joinable())
      ioc_thread.join();
  }
  ~server_runner() { stop(); }

 private:
  uint16_t port_ = 0;
};

// RAII wrapper: starts io_context on a background thread.
// exec lives as long as the runner (safe for via() pointer).
struct ioc_runner {
  asio::io_context ioc;
  std::unique_ptr<coro_io::ExecutorWrapper<>> exec;
  asio::executor_work_guard<asio::io_context::executor_type> work{
      asio::make_work_guard(ioc)};
  std::thread thread;
  std::vector<std::thread> workers;
  std::vector<std::shared_ptr<asio::ip::tcp::acceptor>> acceptors;
  std::vector<std::shared_ptr<coro_http2_connection>> http2_connections;
  std::mutex mtx;
  std::atomic<bool> stopping = false;
  std::atomic<bool> ioc_done = false;

  ioc_runner() {
    exec = std::make_unique<coro_io::ExecutorWrapper<>>(ioc.get_executor());
    thread = std::thread([this] {
      ioc.run();
      ioc_done = true;
    });
  }

  void stop() {
    stopping = true;
    std::vector<std::shared_ptr<asio::ip::tcp::acceptor>> acceptors_to_close;
    std::vector<std::shared_ptr<coro_http2_connection>> conns_to_close;
    {
      std::scoped_lock lock(mtx);
      acceptors_to_close = acceptors;
      conns_to_close = http2_connections;
    }

    for (auto& acceptor : acceptors_to_close) {
      if (!acceptor)
        continue;
      std::error_code ignored;
      acceptor->cancel(ignored);
      acceptor->close(ignored);
    }
    for (auto& conn : conns_to_close) {
      if (conn)
        conn->close();
    }

    for (auto& worker : workers) {
      if (worker.joinable())
        worker.join();
    }
    work.reset();
    for (int i = 0; i < 400 && !ioc_done.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!ioc_done.load())
      ioc.stop();
    if (thread.joinable())
      thread.join();
  }

  ~ioc_runner() { stop(); }
};

static void set_test_socket_timeouts(asio::ip::tcp::socket& sock,
                                     int timeout_ms) {
#ifdef _WIN32
  DWORD timeout = static_cast<DWORD>(timeout_ms);
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Build minimal HTTP/2 GET request bytes:
//   client preface + SETTINGS + HEADERS(END_HEADERS|END_STREAM)
static std::string build_get_frames(const std::string& path) {
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);
  frames +=
      make_frame(frame_type::headers, flags::END_HEADERS | flags::END_STREAM, 1,
                 std::span<const uint8_t>(block));
  return frames;
}

static std::string build_request_frames(
    const std::vector<header_field>& hdrs,
    uint8_t header_flags = flags::END_HEADERS | flags::END_STREAM,
    uint32_t stream_id = 1) {
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  auto block = enc.encode(hdrs);
  for (auto& frame : make_header_block_frames(
           stream_id, std::span<const uint8_t>(block), header_flags,
           coro_http2_connection::MAX_FRAME_SIZE)) {
    frames += frame;
  }
  return frames;
}

static std::string build_header_frame(
    const std::vector<header_field>& hdrs,
    uint8_t header_flags = flags::END_HEADERS | flags::END_STREAM,
    uint32_t stream_id = 1) {
  hpack_encoder enc;
  auto block = enc.encode(hdrs);
  std::string frames;
  for (auto& frame : make_header_block_frames(
           stream_id, std::span<const uint8_t>(block), header_flags,
           coro_http2_connection::MAX_FRAME_SIZE)) {
    frames += frame;
  }
  return frames;
}

static std::string make_large_header_value(size_t size) {
  constexpr std::string_view alphabet =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string out(size, '\0');
  for (size_t i = 0; i < size; ++i) out[i] = alphabet[i % alphabet.size()];
  return out;
}

static std::vector<uint8_t> make_priority_payload(uint32_t stream_dependency,
                                                  uint8_t weight = 0,
                                                  bool exclusive = false) {
  return {
      static_cast<uint8_t>(
          static_cast<uint8_t>((stream_dependency >> 24) & 0x7f) |
          static_cast<uint8_t>(exclusive ? 0x80 : 0x00)),
      static_cast<uint8_t>(stream_dependency >> 16),
      static_cast<uint8_t>(stream_dependency >> 8),
      static_cast<uint8_t>(stream_dependency),
      weight,
  };
}

static std::string build_priority_header_frame(
    const std::vector<header_field>& hdrs, uint32_t stream_dependency,
    uint8_t header_flags = flags::END_HEADERS | flags::END_STREAM |
                           flags::PRIORITY,
    uint32_t stream_id = 1, uint8_t weight = 0, bool exclusive = false) {
  hpack_encoder enc;
  auto block = enc.encode(hdrs);
  auto payload = make_priority_payload(stream_dependency, weight, exclusive);
  payload.insert(payload.end(), block.begin(), block.end());
  return make_frame(frame_type::headers, header_flags, stream_id,
                    std::span<const uint8_t>(payload));
}

static bool read_exact_with_timeout(asio::ip::tcp::socket& sock,
                                    std::span<uint8_t> buf,
                                    std::chrono::milliseconds timeout);

// Read frames from a blocking socket until END_STREAM.
// Sends SETTINGS ACK automatically. Returns {status, body}.
static std::pair<int, std::string> read_h2_response(
    asio::ip::tcp::socket& sock) {
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  std::string body;
  int status = 0;
  hpack_decoder dec;
  std::vector<uint8_t> header_block;
  uint32_t header_stream_id = 0;
  bool header_end_stream = false;

  for (;;) {
    std::error_code ec;
    if (!read_exact_with_timeout(
            sock, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
            std::chrono::milliseconds(2000))) {
      break;
    }

    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0) {
      if (!read_exact_with_timeout(
              sock, std::span<uint8_t>(payload.data(), payload.size()),
              std::chrono::milliseconds(2000))) {
        break;
      }
    }

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      auto ack = make_settings_frame({}, true);
      asio::write(sock, asio::buffer(ack), ec);
    }
    else if (hdr.type == frame_type::headers) {
      header_block.assign(payload.begin(), payload.end());
      header_stream_id = hdr.stream_id;
      header_end_stream = (hdr.flags & flags::END_STREAM) != 0;
      if (!(hdr.flags & flags::END_HEADERS))
        continue;

      auto decoded = dec.decode(std::span<const uint8_t>(header_block));
      header_block.clear();
      for (auto& h : decoded)
        if (h.name == ":status")
          status = std::stoi(h.value);
      if (header_end_stream)
        break;
    }
    else if (hdr.type == frame_type::continuation) {
      if (hdr.stream_id != header_stream_id)
        break;
      header_block.insert(header_block.end(), payload.begin(), payload.end());
      if (!(hdr.flags & flags::END_HEADERS))
        continue;

      auto decoded = dec.decode(std::span<const uint8_t>(header_block));
      header_block.clear();
      for (auto& h : decoded)
        if (h.name == ":status")
          status = std::stoi(h.value);
      if (header_end_stream)
        break;
    }
    else if (hdr.type == frame_type::data) {
      body.append(reinterpret_cast<const char*>(payload.data()),
                  payload.size());
      if (hdr.flags & flags::END_STREAM)
        break;
    }
  }
  return {status, body};
}

struct h2_frame_event {
  frame_type type;
  uint32_t stream_id;
  uint8_t flags;
  std::vector<header_field> headers;
  std::string body;
};

static std::optional<h2_frame_event> read_one_frame_event(
    asio::ip::tcp::socket& sock, hpack_decoder& dec) {
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  std::error_code ec;
  if (!read_exact_with_timeout(
          sock, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
          std::chrono::milliseconds(2000))) {
    return std::nullopt;
  }

  auto hdr = parse_frame_header(hdr_buf);
  payload.resize(hdr.length);
  if (hdr.length > 0) {
    if (!read_exact_with_timeout(
            sock, std::span<uint8_t>(payload.data(), payload.size()),
            std::chrono::milliseconds(2000))) {
      return std::nullopt;
    }
  }

  h2_frame_event evt{
      .type = hdr.type,
      .stream_id = hdr.stream_id,
      .flags = hdr.flags,
  };
  if (hdr.type == frame_type::headers) {
    std::vector<uint8_t> header_block(payload.begin(), payload.end());
    while (!(hdr.flags & flags::END_HEADERS)) {
      std::array<uint8_t, 9> cont_hdr_buf;
      if (!read_exact_with_timeout(
              sock,
              std::span<uint8_t>(cont_hdr_buf.data(), cont_hdr_buf.size()),
              std::chrono::milliseconds(2000))) {
        return std::nullopt;
      }
      hdr = parse_frame_header(cont_hdr_buf);
      if (hdr.type != frame_type::continuation ||
          hdr.stream_id != evt.stream_id)
        return std::nullopt;

      payload.resize(hdr.length);
      if (hdr.length > 0) {
        if (!read_exact_with_timeout(
                sock, std::span<uint8_t>(payload.data(), payload.size()),
                std::chrono::milliseconds(2000))) {
          return std::nullopt;
        }
      }
      header_block.insert(header_block.end(), payload.begin(), payload.end());
      evt.flags |= hdr.flags;
    }
    evt.headers = dec.decode(std::span<const uint8_t>(header_block));
  }
  else if (hdr.type == frame_type::data) {
    evt.body.assign(reinterpret_cast<const char*>(payload.data()),
                    payload.size());
  }
  return evt;
}

static bool read_exact_with_timeout(asio::ip::tcp::socket& sock,
                                    std::span<uint8_t> buf,
                                    std::chrono::milliseconds timeout) {
  std::error_code ec;
  bool was_non_blocking = sock.non_blocking();
  sock.non_blocking(true, ec);
  if (ec)
    return false;

  auto restore = [&]() {
    std::error_code ignored;
    sock.non_blocking(was_non_blocking, ignored);
  };

  size_t offset = 0;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (offset < buf.size()) {
    size_t n = sock.read_some(
        asio::buffer(buf.data() + offset, buf.size() - offset), ec);
    if (!ec) {
      offset += n;
      continue;
    }

    if (ec != asio::error::would_block && ec != asio::error::try_again) {
      restore();
      return false;
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      restore();
      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ec.clear();
  }

  restore();
  return true;
}

static bool read_until_frame_type(asio::ip::tcp::socket& sock,
                                  frame_type target, int max_frames = 20) {
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < max_frames; ++i) {
    std::error_code ec;
    if (!read_exact_with_timeout(
            sock, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
            std::chrono::milliseconds(2000))) {
      return false;
    }

    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0) {
      if (!read_exact_with_timeout(
              sock, std::span<uint8_t>(payload.data(), payload.size()),
              std::chrono::milliseconds(2000))) {
        return false;
      }
    }

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      asio::write(sock, asio::buffer(make_settings_frame({}, true)), ec);
      if (ec)
        return false;
      continue;
    }

    if (hdr.type == target)
      return true;
  }
  return false;
}

static bool read_until_frame_type_on_stream(asio::ip::tcp::socket& sock,
                                            frame_type target,
                                            uint32_t stream_id,
                                            int max_frames = 20) {
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < max_frames; ++i) {
    std::error_code ec;
    if (!read_exact_with_timeout(
            sock, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
            std::chrono::milliseconds(2000))) {
      return false;
    }

    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0) {
      if (!read_exact_with_timeout(
              sock, std::span<uint8_t>(payload.data(), payload.size()),
              std::chrono::milliseconds(2000))) {
        return false;
      }
    }

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      asio::write(sock, asio::buffer(make_settings_frame({}, true)), ec);
      if (ec)
        return false;
      continue;
    }

    if (hdr.type == target && hdr.stream_id == stream_id)
      return true;
  }
  return false;
}

static bool send_request_and_expect_goaway(
    const std::vector<header_field>& hdrs, bool enable_connect_protocol = false,
    uint8_t header_flags = flags::END_HEADERS | flags::END_STREAM,
    uint32_t stream_id = 1) {
  server_runner srv;
  srv.set_enable_connect_protocol(enable_connect_protocol);
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());
  asio::write(client, asio::buffer(
                          build_request_frames(hdrs, header_flags, stream_id)));
  bool got_goaway = read_until_frame_type(client, frame_type::goaway);
  client.close();
  return got_goaway;
}

static std::pair<int, std::string> send_request_and_read_response(
    const std::vector<header_field>& hdrs, h2_handler handler,
    bool enable_connect_protocol = false,
    uint8_t header_flags = flags::END_HEADERS | flags::END_STREAM,
    uint32_t stream_id = 1) {
  server_runner srv;
  srv.set_enable_connect_protocol(enable_connect_protocol);
  srv.launch(std::move(handler));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());
  asio::write(client, asio::buffer(
                          build_request_frames(hdrs, header_flags, stream_id)));
  auto result = read_h2_response(client);
  client.close();
  return result;
}

// Launch a coro_http2_connection on `runner` and return its port.
// All I/O (accept + serve) runs on runner.ioc via via(exec).detach().
static uint16_t start_h2_server(ioc_runner& runner, h2_handler handler) {
  auto acc = std::make_shared<asio::ip::tcp::acceptor>(
      runner.ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  acc->set_option(asio::ip::tcp::acceptor::reuse_address(true));
  uint16_t port = acc->local_endpoint().port();
  {
    std::scoped_lock lock(runner.mtx);
    runner.acceptors.push_back(acc);
  }

  auto exec_ptr = runner.exec.get();
  runner.workers.emplace_back(
      [&runner, acc, handler = std::move(handler), exec_ptr]() mutable {
        asio::ip::tcp::socket sock(runner.ioc);
        std::error_code ec;
        acc->accept(sock, ec);
        if (ec)
          return;
        set_test_socket_timeouts(sock);
        auto conn = std::make_shared<coro_http2_connection>(
            std::move(sock), std::move(handler), exec_ptr);
        {
          std::scoped_lock lock(runner.mtx);
          runner.http2_connections.push_back(conn);
        }
        if (runner.stopping.load())
          conn->close();
        async_simple::coro::syncAwait(conn->start().via(exec_ptr));
      });

  return port;
}

static bool read_raw_frame(asio::ip::tcp::socket& sock, frame_header& hdr,
                           std::vector<uint8_t>& payload) {
  std::array<uint8_t, 9> hdr_buf;
  if (!read_exact_with_timeout(
          sock, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
          std::chrono::milliseconds(2000))) {
    return false;
  }

  hdr = parse_frame_header(hdr_buf);
  payload.resize(hdr.length);
  if (hdr.length > 0) {
    if (!read_exact_with_timeout(
            sock, std::span<uint8_t>(payload.data(), payload.size()),
            std::chrono::milliseconds(2000))) {
      return false;
    }
  }
  return true;
}

static bool read_client_preface_and_settings(asio::ip::tcp::socket& sock) {
  std::array<char, CLIENT_PREFACE.size()> preface{};
  if (!read_exact_with_timeout(
          sock,
          std::span<uint8_t>(reinterpret_cast<uint8_t*>(preface.data()),
                             preface.size()),
          std::chrono::milliseconds(2000))) {
    return false;
  }
  if (std::string_view(preface.data(), preface.size()) != CLIENT_PREFACE)
    return false;

  frame_header hdr{};
  std::vector<uint8_t> payload;
  if (!read_raw_frame(sock, hdr, payload))
    return false;
  return hdr.type == frame_type::settings && !(hdr.flags & flags::ACK) &&
         hdr.stream_id == 0;
}

static bool wait_for_client_request_headers(asio::ip::tcp::socket& sock,
                                            uint32_t stream_id = 1,
                                            int max_frames = 20) {
  frame_header hdr{};
  std::vector<uint8_t> payload;
  for (int i = 0; i < max_frames; ++i) {
    if (!read_raw_frame(sock, hdr, payload))
      return false;
    if (hdr.type == frame_type::settings ||
        hdr.type == frame_type::window_update) {
      continue;
    }
    if (hdr.type == frame_type::headers && hdr.stream_id == stream_id)
      return true;
  }
  return false;
}

static bool saw_client_request_headers_within(asio::ip::tcp::socket& sock,
                                              std::chrono::milliseconds timeout,
                                              uint32_t stream_id = 1) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  frame_header hdr{};
  std::vector<uint8_t> payload;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code ec;
    if (sock.available(ec) < 9) {
      if (ec)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (!read_raw_frame(sock, hdr, payload))
      return false;
    if (hdr.type == frame_type::settings ||
        hdr.type == frame_type::window_update) {
      continue;
    }
    return hdr.type == frame_type::headers && hdr.stream_id == stream_id;
  }
  return false;
}

struct raw_h2_server_runner {
  asio::io_context ioc;
  asio::ip::tcp::acceptor acceptor{
      ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)};
  std::thread server_thread;
  std::shared_ptr<asio::ip::tcp::socket> active_socket;
  std::mutex active_socket_mtx;

  explicit raw_h2_server_runner(
      std::function<void(asio::ip::tcp::socket&)> script) {
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    port_ = acceptor.local_endpoint().port();
    server_thread = std::thread([this, script = std::move(script)]() mutable {
      try {
        auto sock = std::make_shared<asio::ip::tcp::socket>(ioc);
        std::error_code ec;
        acceptor.accept(*sock, ec);
        if (ec)
          return;
        set_test_socket_timeouts(*sock);
        {
          std::scoped_lock lock(active_socket_mtx);
          active_socket = sock;
        }
        script(*sock);
      } catch (...) {
      }
    });
  }

  uint16_t port() const { return port_; }

  void stop() {
    std::error_code ignored;
    acceptor.cancel(ignored);
    acceptor.close(ignored);
    std::shared_ptr<asio::ip::tcp::socket> sock;
    {
      std::scoped_lock lock(active_socket_mtx);
      sock = active_socket;
    }
    if (sock) {
      sock->cancel(ignored);
      sock->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      sock->close(ignored);
    }
    if (server_thread.joinable())
      server_thread.join();
  }

  ~raw_h2_server_runner() { stop(); }

 private:
  uint16_t port_ = 0;
};

static h2_client_response run_client_with_raw_response(
    std::string response_frames, std::string path = "/") {
  raw_h2_server_runner srv([response_frames = std::move(response_frames)](
                               asio::ip::tcp::socket& sock) mutable {
    if (!read_client_preface_and_settings(sock))
      return;

    std::error_code ec;
    auto settings = make_settings_frame({});
    asio::write(sock, asio::buffer(settings), ec);
    if (ec)
      return;

    if (!wait_for_client_request_headers(sock))
      return;

    asio::write(sock, asio::buffer(response_frames), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get(std::move(path)));
  client.close();
  srv.stop();
  return resp;
}

// ---------------------------------------------------------------------------
// Integration tests
// ---------------------------------------------------------------------------

TEST_CASE("integration: HTTP/2 GET returns 200 with body") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(
            req.path == "/hello" ? 200 : 404,
            req.path == "/hello" ? "hello http2" : "not found");
        co_return;
      });
  uint16_t port = srv.port();

  // Client uses a separate blocking io_context (no async needed)
  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  asio::write(client, asio::buffer(build_get_frames("/hello")));
  auto [status, body] = read_h2_response(client);

  CHECK(status == 200);
  CHECK(body == "hello http2");
  client.close();
}

TEST_CASE("integration: unknown path returns 404") {
  server_runner srv;
  srv.launch([](h2_request& req,
                h2_response& resp) -> async_simple::coro::Lazy<void> {
    resp.set_status_and_body(req.path == "/exists" ? 200 : 404, "not found");
    co_return;
  });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  asio::write(client, asio::buffer(build_get_frames("/missing")));
  auto [status, body] = read_h2_response(client);
  CHECK(status == 404);
  client.close();
}

TEST_CASE("integration: PING is answered with ACK") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  // Send preface + SETTINGS + PING
  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  asio::write(client, asio::buffer(init));

  std::array<uint8_t, 8> ping_data{1, 2, 3, 4, 5, 6, 7, 8};
  asio::write(client,
              asio::buffer(make_frame(frame_type::ping, 0, 0, ping_data)));

  // Scan frames for PING ACK
  bool got_ack = false;
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < 10 && !got_ack; ++i) {
    std::error_code ec;
    asio::read(client, asio::buffer(hdr_buf), ec);
    if (ec)
      break;
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0)
      asio::read(client, asio::buffer(payload));

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
    }
    else if (hdr.type == frame_type::ping && (hdr.flags & flags::ACK)) {
      CHECK(payload ==
            std::vector<uint8_t>(ping_data.begin(), ping_data.end()));
      got_ack = true;
    }
  }
  CHECK(got_ack);
  client.close();
}

TEST_CASE("integration: invalid PING payload length triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  asio::write(client, asio::buffer(init));

  std::array<uint8_t, 7> bad_ping{1, 2, 3, 4, 5, 6, 7};
  asio::write(client,
              asio::buffer(make_frame(frame_type::ping, 0, 0, bad_ping)));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("preface: client magic not followed by SETTINGS triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<uint8_t, 8> ping_data{1, 2, 3, 4, 5, 6, 7, 8};
  frames += make_frame(frame_type::ping, 0, 0, ping_data);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

// Connect with retries to avoid race between accept_loop scheduling and
// client connecting before the async accept is posted.
static void connect_with_retry(asio::ip::tcp::socket& sock, uint16_t port,
                               int retries) {
  asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
  for (int i = 0; i < retries; ++i) {
    std::error_code ec;
    sock.connect(ep, ec);
    if (!ec) {
      set_test_socket_timeouts(sock);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  sock.connect(ep);  // final attempt, let it throw
  set_test_socket_timeouts(sock);
}

static void connect_direct(asio::ip::tcp::socket& sock, uint16_t port) {
  sock.connect(
      asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  set_test_socket_timeouts(sock);
}

static std::string base64url_encode(std::span<const uint8_t> data) {
  std::string input(reinterpret_cast<const char*>(data.data()), data.size());
  auto encoded = cinatra::base64_encode(input);
  for (auto& ch : encoded) {
    if (ch == '+')
      ch = '-';
    else if (ch == '/')
      ch = '_';
  }
  while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
  return encoded;
}

static std::string h2c_settings_header(
    std::span<const settings_entry> settings) {
  auto frame = make_settings_frame(settings);
  auto payload = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(frame.data()) + 9, frame.size() - 9);
  return base64url_encode(payload);
}

static std::string read_http1_response_headers(asio::ip::tcp::socket& sock) {
  std::string response;
  response.reserve(256);
  std::array<char, 1> ch{};
  std::error_code ec;
  while (response.find("\r\n\r\n") == std::string::npos &&
         response.size() < 8192) {
    asio::read(sock, asio::buffer(ch), ec);
    if (ec)
      break;
    response.push_back(ch[0]);
  }
  return response;
}

static std::string read_http1_response_body(asio::ip::tcp::socket& sock,
                                            std::string_view headers) {
  auto lower = std::string(headers);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  auto pos = lower.find("content-length:");
  if (pos == std::string::npos)
    return {};
  pos += std::string_view("content-length:").size();
  while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) {
    ++pos;
  }
  auto end = lower.find("\r\n", pos);
  auto len_text = lower.substr(
      pos, end == std::string::npos ? std::string::npos : end - pos);
  size_t len = 0;
  auto [ptr, ec] =
      std::from_chars(len_text.data(), len_text.data() + len_text.size(), len);
  if (ec != std::errc{} || ptr == len_text.data())
    return {};

  std::string body(len, '\0');
  if (len == 0)
    return body;
  auto body_span =
      std::span<uint8_t>(reinterpret_cast<uint8_t*>(body.data()), body.size());
  if (!read_exact_with_timeout(sock, body_span,
                               std::chrono::milliseconds(2000)))
    return {};
  return body;
}

#ifdef CINATRA_ENABLE_SSL
class test_http2_required_server {
 public:
  test_http2_required_server(asio::io_context& ioc, uint16_t port)
      : server_(ioc, port) {
    server_.set_http2_mode(cinatra::http2_mode::required);
  }

  template <cinatra::http_method... methods, typename Func>
  void set_http_handler(std::string path, Func handler) {
    using ret_t =
        std::invoke_result_t<std::decay_t<Func>, h2_request&, h2_response&>;
    auto adapted = [handler = std::move(handler)](
                       cinatra::coro_http_request& req,
                       cinatra::coro_http_response& resp) mutable
        -> async_simple::coro::Lazy<void> {
      h2_request h2_req = to_h2_request(req);
      h2_response h2_resp;
      if constexpr (coro_io::is_lazy_v<ret_t>) {
        co_await handler(h2_req, h2_resp);
      }
      else {
        handler(h2_req, h2_resp);
      }
      apply_h2_response(h2_resp, resp);
      co_return;
    };
    server_.set_http_handler<methods...>(std::move(path), std::move(adapted));
  }

  void set_default_handler(h2_handler handler) {
    auto adapted = [handler = std::move(handler)](
                       cinatra::coro_http_request& req,
                       cinatra::coro_http_response& resp)
        -> async_simple::coro::Lazy<void> {
      h2_request h2_req = to_h2_request(req);
      h2_response h2_resp;
      co_await handler(h2_req, h2_resp);
      apply_h2_response(h2_resp, resp);
      co_return;
    };
    server_.set_default_handler(std::move(adapted));
  }

  void set_enable_connect_protocol(bool enabled) {
    enable_connect_protocol_ = enabled;
    server_.set_enable_http2_connect_protocol(enabled);
  }

  void init_ssl(const std::string& cert_file, const std::string& key_file,
                std::string passwd = {}) {
    server_.init_ssl(cert_file, key_file, std::move(passwd));
  }

  uint16_t start(coro_io::ExecutorWrapper<>& exec) {
    (void)exec;
    server_.set_http2_mode(cinatra::http2_mode::required);
    server_.set_enable_http2_connect_protocol(enable_connect_protocol_);
    start_future_.emplace(server_.async_start());
    if (start_future_->hasResult() && start_future_->value()) {
      start_future_.reset();
      return 0;
    }
    return server_.port();
  }

  uint16_t port() const { return server_.port(); }

  void stop() {
    server_.stop();
    start_future_.reset();
  }

 private:
  static cinatra::status_type to_status_type(int code) {
    if (code > 0) {
      return static_cast<cinatra::status_type>(code);
    }
    return cinatra::status_type::not_implemented;
  }

  static h2_request to_h2_request(cinatra::coro_http_request& req) {
    h2_request converted;
    converted.method = std::string(req.get_method());
    converted.path = std::string(req.full_url());
    converted.scheme = std::string(req.get_scheme());
    converted.authority = std::string(req.get_authority());
    converted.protocol = std::string(req.get_protocol());
    converted.body = std::string(req.get_body());
    for (auto& header : req.get_headers()) {
      converted.headers.push_back(
          {std::string(header.name), std::string(header.value)});
    }
    for (auto& trailer : req.get_trailers()) {
      converted.trailers.push_back(
          {std::string(trailer.name), std::string(trailer.value)});
    }
    if (auto metadata = req.get_user_data(); metadata.has_value()) {
      if (auto* http2_metadata =
              std::any_cast<common_request_metadata>(&metadata)) {
        converted.needs_flow_control_probe_body =
            http2_metadata->needs_flow_control_probe_body;
      }
    }
    converted.params_ = req.params_;
    converted.matches_ = req.matches_;
    return converted;
  }

  static void apply_headers(const std::vector<header_field>& headers,
                            auto append) {
    for (auto& header : headers) {
      append(header.name, header.value);
    }
  }

  static void apply_h2_response(const h2_response& source,
                                cinatra::coro_http_response& target) {
    target.set_status_and_content(to_status_type(source.status_code),
                                  source.body);

    apply_headers(source.headers,
                  [&target](std::string_view name, std::string_view value) {
                    target.add_header(std::string(name), std::string(value));
                  });
    apply_headers(source.trailers,
                  [&target](std::string_view name, std::string_view value) {
                    target.add_trailer(std::string(name), std::string(value));
                  });

    for (auto& push : source.pushes) {
      auto& target_push = cinatra::http2::add_push(
          target, push.path, push.body, to_status_type(push.status_code));
      target_push.method = push.method;
      target_push.scheme = push.scheme;
      target_push.authority = push.authority;
      for (auto& header : push.request_headers) {
        target_push.add_request_header(header.name, header.value);
      }
      for (auto& header : push.response_headers) {
        target_push.add_response_header(header.name, header.value);
      }
      for (auto& trailer : push.response_trailers) {
        target_push.add_response_trailer(trailer.name, trailer.value);
      }
    }
  }

  cinatra::coro_http_server server_;
  bool enable_connect_protocol_ = false;
  std::optional<async_simple::Future<std::error_code>> start_future_;
};
#endif

// ---------------------------------------------------------------------------
// HTTP/2 server integration tests
// ---------------------------------------------------------------------------

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test_http2_required_server: GET /hello returns 200" *
          doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/hello", [](h2_request&, h2_response& resp) {
        resp.set_status_and_body(200, "hello from server");
      });
  uint16_t port = srv.start(*runner.exec);

  // Give the accept loop time to post its first async_accept.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  asio::write(client, asio::buffer(build_get_frames("/hello")));
  auto [status, body] = read_h2_response(client);

  CHECK(status == 200);
  CHECK(body == "hello from server");
  client.close();
  // Wait for server connection coroutine to finish before destroying srv.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  srv.stop();
}

TEST_CASE("test_http2_required_server: unknown route returns 404" *
          doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>("/only",
                                     [](h2_request&, h2_response& resp) {
                                       resp.set_status_and_body(200, "ok");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  asio::write(client, asio::buffer(build_get_frames("/other")));
  auto [status, body] = read_h2_response(client);

  CHECK(status == 404);
  client.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  srv.stop();
}

TEST_CASE("test_http2_required_server: parameter route dispatches correctly" *
          doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/users/:id", [](h2_request& req, h2_response& resp) {
        resp.set_status_and_body(200, req.params_["id"]);
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  asio::write(client, asio::buffer(build_get_frames("/users/42")));
  auto [status, body] = read_h2_response(client);

  CHECK(status == 200);
  CHECK(body == "42");
  client.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  srv.stop();
}
#endif

TEST_CASE("coro_http_server: default mode serves HTTP/1.1") {
  ioc_runner runner;
  cinatra::coro_http_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/hello",
      [](cinatra::coro_http_request& req, cinatra::coro_http_response& resp) {
        auto url = std::string(req.get_url());
        auto query = std::string(req.get_query_value("name"));
        resp.add_header("x-method", std::string(req.get_method()));
        resp.set_status_and_content(cinatra::status_type::ok,
                                    url + ":" + query);
      });
  auto server_future = srv.async_start();

  asio::io_context http1_ioc;
  asio::ip::tcp::socket http1_client(http1_ioc);
  connect_direct(http1_client, srv.port());
  std::string request =
      "GET /hello?name=http1 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n\r\n";
  asio::write(http1_client, asio::buffer(request));
  auto headers = read_http1_response_headers(http1_client);
  CAPTURE(headers);
  CHECK(headers.find("200 OK") != std::string::npos);
  CHECK(read_http1_response_body(http1_client, headers) == "/hello:http1");
  http1_client.close();

  srv.stop();
  server_future.wait();
  CHECK(server_future.value() == asio::error::operation_aborted);
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("coro_http_server: cleartext required mode still serves HTTP/1.1") {
  ioc_runner runner;
  cinatra::coro_http_server srv(runner.ioc, 0);
  srv.set_http2_mode(cinatra::http2_mode::required);
  srv.set_http_handler<cinatra::GET>(
      "/hello",
      [](cinatra::coro_http_request&, cinatra::coro_http_response& resp) {
        resp.set_status_and_content(cinatra::status_type::ok, "http1");
      });
  auto server_future = srv.async_start();

  asio::io_context http1_ioc;
  asio::ip::tcp::socket http1_client(http1_ioc);
  connect_direct(http1_client, srv.port());
  std::string request =
      "GET /hello HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n\r\n";
  asio::write(http1_client, asio::buffer(request));
  auto headers = read_http1_response_headers(http1_client);
  CAPTURE(headers);
  CHECK(headers.find("200 OK") != std::string::npos);
  CHECK(read_http1_response_body(http1_client, headers) == "http1");
  http1_client.close();

  srv.stop();
  server_future.wait();
  CHECK(server_future.value() == asio::error::operation_aborted);
}
#endif

TEST_CASE("coro_http_server: stop closes idle HTTP/1.1 sockets") {
  ioc_runner runner;
  cinatra::coro_http_server srv(runner.ioc, 0);
  auto server_future = srv.async_start();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());
  set_test_socket_timeouts(client, 1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  srv.stop();

  std::array<char, 1> data{};
  asio::error_code ec;
  auto n = client.read_some(asio::buffer(data), ec);
  CHECK((ec || n == 0));
  CHECK(ec != asio::error::timed_out);

  client.close();
  server_future.wait();
  CHECK(server_future.value() == asio::error::operation_aborted);
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("coro_http_server: TLS HTTP/2 preserves trailers in common objects") {
  ioc_runner runner;
  cinatra::coro_http_server srv(runner.ioc, 0);
  srv.set_http2_mode(cinatra::http2_mode::required);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::POST>(
      "/trailers",
      [](cinatra::coro_http_request& req,
         cinatra::coro_http_response& resp) -> async_simple::coro::Lazy<void> {
        std::string trailer_value;
        for (auto& trailer : req.get_trailers()) {
          if (trailer.name == "x-tail") {
            trailer_value = std::string(trailer.value);
          }
        }

        resp.set_status_and_content(trailer_value == "ok"
                                        ? cinatra::status_type::ok
                                        : cinatra::status_type::bad_request,
                                    std::string(req.get_body()));
        resp.add_trailer("x-ack", trailer_value);
        co_return;
      });
  auto server_future = srv.async_start();

  coro_http2_client client(runner.exec.get());
  auto connect_ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port()), "https"));
  REQUIRE(!connect_ec);

  h2_client_request request;
  request.method = "POST";
  request.path = "/trailers";
  request.body = "payload";
  request.add_trailer("x-tail", "ok");
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(request)));

  CHECK(resp.net_err.value() == 0);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "payload");
  CHECK(resp.trailers.size() == 1);
  CHECK(resp.trailers[0].name == "x-ack");
  CHECK(resp.trailers[0].value == "ok");

  client.close();
  srv.stop();
  server_future.wait();
  CHECK(server_future.value() == asio::error::operation_aborted);
}
#endif

#ifdef CINATRA_ENABLE_SSL
TEST_CASE(
    "test_http2_required_server: h2c upgrade dispatches HTTP/1.1 request as "
    "stream 1" *
    doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/upgrade", [](h2_request& req, h2_response& resp) {
        resp.set_status_and_body(
            200, req.path == "/upgrade" && req.authority == "localhost"
                     ? "upgraded"
                     : "bad upgrade");
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  std::array<settings_entry, 1> settings{
      settings_entry{settings_param::initial_window_size,
                     coro_http2_connection::DEFAULT_WINDOW_SIZE}};
  std::string request =
      "GET /upgrade HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade, HTTP2-Settings\r\n"
      "Upgrade: h2c\r\n"
      "HTTP2-Settings: " +
      h2c_settings_header(settings) + "\r\n\r\n";
  asio::write(client, asio::buffer(request));

  auto upgrade_response = read_http1_response_headers(client);
  CAPTURE(upgrade_response);
  REQUIRE(upgrade_response.find("101 Switching Protocols") !=
          std::string::npos);

  std::string preface(CLIENT_PREFACE);
  preface += make_settings_frame({});
  asio::write(client, asio::buffer(preface));

  auto [status, body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(body == "upgraded");

  client.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server: h2c upgrade missing HTTP2-Settings is "
    "rejected" *
    doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/upgrade", [](h2_request&, h2_response& resp) {
        resp.set_status_and_body(200, "unexpected");
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  std::string request =
      "GET /upgrade HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: h2c\r\n\r\n";
  asio::write(client, asio::buffer(request));

  auto response = read_http1_response_headers(client);
  CHECK(response.find("101 Switching Protocols") == std::string::npos);
  CHECK(response.find("400 Bad Request") != std::string::npos);

  client.close();
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server: h2c upgrade invalid HTTP2-Settings is "
    "rejected" *
    doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/upgrade", [](h2_request&, h2_response& resp) {
        resp.set_status_and_body(200, "unexpected");
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  std::string request =
      "GET /upgrade HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade, HTTP2-Settings\r\n"
      "Upgrade: h2c\r\n"
      "HTTP2-Settings: !not-base64!\r\n\r\n";
  asio::write(client, asio::buffer(request));

  auto response = read_http1_response_headers(client);
  CHECK(response.find("101 Switching Protocols") == std::string::npos);
  CHECK(response.find("400 Bad Request") != std::string::npos);

  client.close();
  srv.stop();
}

TEST_CASE(
    "coro_http2_client: h2c upgrade carries first request body and keeps later "
    "requests on HTTP/2" *
    doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::POST>(
      "/upgrade-upload", [](h2_request& req, h2_response& resp) {
        resp.set_status_and_body(req.body == "payload" ? 200 : 400, req.body);
      });
  srv.set_http_handler<cinatra::GET>("/after-upgrade",
                                     [](h2_request&, h2_response& resp) {
                                       resp.set_status_and_body(200, "after");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  client.set_use_h2c_upgrade(true);
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port)));
  REQUIRE(!ec);

  auto first = async_simple::coro::syncAwait(
      client.async_post("/upgrade-upload", "payload"));
  CHECK(!first.net_err);
  CHECK(first.status_code == 200);
  CHECK(first.body == "payload");

  auto second =
      async_simple::coro::syncAwait(client.async_get("/after-upgrade"));
  CHECK(!second.net_err);
  CHECK(second.status_code == 200);
  CHECK(second.body == "after");

  client.close();
  srv.stop();
}
#endif

TEST_CASE("coro_http2_client: h2c upgrade rejects non-101 response") {
  asio::io_context server_ioc;
  asio::ip::tcp::acceptor acceptor(
      server_ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  uint16_t port = acceptor.local_endpoint().port();

  std::thread server_thread([&acceptor, &server_ioc]() {
    try {
      asio::ip::tcp::socket sock(server_ioc);
      std::error_code ec;
      acceptor.accept(sock, ec);
      if (ec)
        return;
      set_test_socket_timeouts(sock);
      auto request = read_http1_response_headers(sock);
      if (request.find("Upgrade: h2c") == std::string::npos)
        return;
      static constexpr std::string_view response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 0\r\n\r\n";
      asio::write(sock, asio::buffer(response), ec);
      sock.close(ec);
    } catch (...) {
    }
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  client.set_use_h2c_upgrade(true);
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port)));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/bad-upgrade"));
  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));

  client.close();
  std::error_code ignored;
  acceptor.close(ignored);
  if (server_thread.joinable())
    server_thread.join();
}

TEST_CASE("nghttp2-inspired request validation: regular CONNECT is accepted") {
  auto [status, body] = send_request_and_read_response(
      {{":method", "CONNECT"}, {":authority", "upstream.example:443"}},
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(
            req.method == "CONNECT" &&
                    req.authority == "upstream.example:443" &&
                    req.path.empty() && req.scheme.empty() &&
                    req.protocol.empty()
                ? 200
                : 400,
            "connect");
        co_return;
      });

  CHECK(status == 200);
  CHECK(body == "connect");
}

TEST_CASE(
    "nghttp2-inspired request validation: regular CONNECT with path and scheme "
    "triggers RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "unexpected");
        co_return;
      });

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());

  asio::write(client, asio::buffer(build_request_frames(
                          {{":method", "CONNECT"},
                           {":path", "/tunnel"},
                           {":scheme", "https"},
                           {":authority", "upstream.example:443"}})));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: extended CONNECT is accepted when "
    "enabled") {
  auto [status, body] = send_request_and_read_response(
      {{":method", "CONNECT"},
       {":protocol", "websocket"},
       {":path", "/chat"},
       {":scheme", "https"},
       {":authority", "upstream.example"}},
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(
            req.method == "CONNECT" && req.protocol == "websocket" &&
                    req.path == "/chat" && req.scheme == "https" &&
                    req.authority == "upstream.example"
                ? 200
                : 400,
            "extended");
        co_return;
      },
      true);

  CHECK(status == 200);
  CHECK(body == "extended");
}

TEST_CASE(
    "nghttp2-inspired request validation: extended CONNECT without setting "
    "triggers RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "unexpected");
        co_return;
      });

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());

  asio::write(
      client,
      asio::buffer(build_request_frames({{":method", "CONNECT"},
                                         {":protocol", "websocket"},
                                         {":path", "/chat"},
                                         {":scheme", "https"},
                                         {":authority", "upstream.example"}})));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "SETTINGS_MAX_CONCURRENT_STREAMS rejects pipelined stream above advertised "
    "limit") {
  auto release_responses = std::make_shared<std::atomic<bool>>(false);
  server_runner srv;
  srv.launch(
      [release_responses](h2_request&,
                          h2_response& resp) -> async_simple::coro::Lazy<void> {
        auto* executor = co_await async_simple::CurrentExecutor{};
        while (!release_responses->load()) {
          co_await async_simple::coro::sleep(executor,
                                             std::chrono::milliseconds(1));
        }
        resp.set_status_and_body(204, "");
        co_return;
      });

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});
  for (uint32_t stream_id = 1; stream_id <= 201; stream_id += 2) {
    frames +=
        build_header_frame({{":method", "GET"},
                            {":path", "/" + std::to_string(stream_id)},
                            {":scheme", "http"},
                            {":authority", "localhost"}},
                           flags::END_HEADERS | flags::END_STREAM, stream_id);
  }
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 201,
                                        260));
  release_responses->store(true);

  int response_count = 0;
  for (int i = 0; i < 260 && response_count < 100; ++i) {
    frame_header hdr{};
    std::vector<uint8_t> payload;
    if (!read_raw_frame(client, hdr, payload))
      break;
    if (hdr.type == frame_type::headers && hdr.stream_id != 201) {
      ++response_count;
    }
  }
  CHECK(response_count == 100);

  asio::write(client, asio::buffer(build_header_frame(
                          {{":method", "GET"},
                           {":path", "/after-limit"},
                           {":scheme", "http"},
                           {":authority", "localhost"}},
                          flags::END_HEADERS | flags::END_STREAM, 203)));

  bool got_after_limit_response = false;
  for (int i = 0; i < 20 && !got_after_limit_response; ++i) {
    frame_header hdr{};
    std::vector<uint8_t> payload;
    if (!read_raw_frame(client, hdr, payload))
      break;
    got_after_limit_response =
        hdr.type == frame_type::headers && hdr.stream_id == 203;
  }
  CHECK(got_after_limit_response);
  client.close();
}

TEST_CASE("coro_http2_client: regular CONNECT request is encoded correctly") {
  server_runner srv;
  srv.launch([](h2_request& req,
                h2_response& resp) -> async_simple::coro::Lazy<void> {
    resp.set_status_and_body(
        req.method == "CONNECT" && req.authority == "upstream.example:443" &&
                req.path.empty() && req.scheme.empty() && req.protocol.empty()
            ? 200
            : 400,
        "connect");
    co_return;
  });
  uint16_t port = srv.port();

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port)));
  REQUIRE(!ec);

  h2_client_request req;
  req.method = "CONNECT";
  req.authority = "upstream.example:443";
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "connect");

  client.close();
  srv.stop();
}

TEST_CASE("coro_http2_client: extended CONNECT request is encoded correctly") {
  server_runner srv;
  srv.set_enable_connect_protocol(true);
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(
            req.method == "CONNECT" && req.protocol == "websocket" &&
                    req.path == "/chat" && req.scheme == "https" &&
                    req.authority == "upstream.example"
                ? 200
                : 400,
            "extended");
        co_return;
      });
  uint16_t port = srv.port();

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port)));
  REQUIRE(!ec);

  h2_client_request req;
  req.method = "CONNECT";
  req.protocol = "websocket";
  req.path = "/chat";
  req.scheme = "https";
  req.authority = "upstream.example";
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "extended");

  client.close();
  srv.stop();
}

TEST_CASE("coro_http2_client: GET /hello returns 200") {
  ioc_runner runner;
  uint16_t port = start_h2_server(
      runner,
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(
            req.path == "/hello" ? 200 : 404,
            req.path == "/hello" ? "hello from client" : "not found");
        co_return;
      });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port)));
  CHECK(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/hello"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "hello from client");
  client.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("coro_http2_client/server: TLS with ALPN h2 request succeeds") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>(
      "/hello",
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "hello over tls");
        co_return;
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  CHECK(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/hello"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "hello over tls");

  client.close();
  srv.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE("coro_http_client: HTTPS ALPN h2 request succeeds") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>(
      "/hello",
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "hello via generic client");
        co_return;
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  cinatra::coro_http_client client(runner.ioc.get_executor());
  auto resp = async_simple::coro::syncAwait(
      client.async_get("https://127.0.0.1:" + std::to_string(port) + "/hello"));
  CHECK(!resp.net_err);
  CHECK(resp.status == 200);
  CHECK(resp.resp_body == "hello via generic client");

  client.close();
  srv.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE("coro_http2_client: TLS server without ALPN h2 is rejected") {
  asio::io_context server_ioc;
  asio::ip::tcp::acceptor acceptor(
      server_ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  uint16_t port = acceptor.local_endpoint().port();

  std::thread server_thread([&acceptor, &server_ioc]() {
    try {
      asio::ip::tcp::socket sock(server_ioc);
      std::error_code ec;
      acceptor.accept(sock, ec);
      if (ec)
        return;
      set_test_socket_timeouts(sock);
      auto ssl_ctx = make_test_server_ssl_context(false);
      asio::ssl::stream<asio::ip::tcp::socket&> stream(sock, *ssl_ctx);
      stream.handshake(asio::ssl::stream_base::server, ec);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      sock.close(ec);
    } catch (...) {
    }
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  CHECK(ec == std::make_error_code(std::errc::protocol_error));

  client.close();
  std::error_code ignored;
  acceptor.close(ignored);
  if (server_thread.joinable())
    server_thread.join();
}

TEST_CASE(
    "test_http2_required_server: TLS handshake with non-h2 ALPN is rejected") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>(
      "/hello",
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "unused");
        co_return;
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  asio::io_context client_ioc;
  asio::ip::tcp::socket sock(client_ioc);
  connect_with_retry(sock, port);

  asio::ssl::context ssl_ctx(asio::ssl::context::sslv23);
  ssl_ctx.set_verify_mode(asio::ssl::verify_none);
  asio::ssl::stream<asio::ip::tcp::socket&> stream(sock, ssl_ctx);
  static constexpr unsigned char HTTP11_ALPN[] = {8,   'h', 't', 't', 'p',
                                                  '/', '1', '.', '1'};
  REQUIRE(SSL_set_alpn_protos(stream.native_handle(), HTTP11_ALPN,
                              static_cast<unsigned int>(sizeof(HTTP11_ALPN))) ==
          0);
  std::error_code ec;
  stream.handshake(asio::ssl::stream_base::client, ec);
  CHECK(static_cast<bool>(ec));

  sock.close(ec);
  srv.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
#endif

#ifdef CINATRA_ENABLE_SSL
TEST_CASE(
    "coro_http2_client/server: server push callback receives promised "
    "response") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>(
      "/index", [](h2_request&, h2_response& resp) {
        auto& push = resp.add_push("/style.css", "body{}");
        push.add_response_header("content-type", "text/css");
        resp.set_status_and_body(200, "<html/>");
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::mutex mtx;
  std::condition_variable cv;
  std::optional<h2_pushed_response> pushed;

  coro_http2_client client(runner.exec.get());
  client.set_enable_push(true);
  client.set_push_handler([&](h2_pushed_response value) {
    {
      std::lock_guard lock(mtx);
      pushed = std::move(value);
    }
    cv.notify_one();
  });

  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/index"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "<html/>");

  std::unique_lock lock(mtx);
  REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
    return pushed.has_value();
  }));
  REQUIRE(pushed.has_value());
  CHECK(pushed->request.method == "GET");
  CHECK(pushed->request.path == "/style.css");
  CHECK(pushed->response.status_code == 200);
  CHECK(pushed->response.body == "body{}");
  bool saw_content_type = false;
  for (auto& hf : pushed->response.headers) {
    if (hf.name == "content-type" && hf.value == "text/css")
      saw_content_type = true;
  }
  CHECK(saw_content_type);

  client.close();
  srv.stop();
}
#endif

TEST_CASE("coro_http2_client: multiplexed requests share one connection") {
  ioc_runner runner;
  uint16_t port = start_h2_server(
      runner,
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        if (req.path == "/slow")
          co_await coro_io::sleep_for(std::chrono::milliseconds(200));
        resp.set_status_and_body(200, req.path == "/slow" ? "slow" : "fast");
        co_return;
      });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port)));
  CHECK(!ec);

  auto results = async_simple::coro::syncAwait(
      [&client]() -> async_simple::coro::Lazy<
                      std::vector<async_simple::Try<h2_client_response>>> {
        std::vector<async_simple::coro::Lazy<h2_client_response>> reqs;
        reqs.push_back(client.async_get("/slow"));
        reqs.push_back(client.async_get("/fast"));
        co_return co_await async_simple::coro::collectAll(std::move(reqs));
      }());

  REQUIRE(results.size() == 2);
  CHECK(!results[0].hasError());
  CHECK(!results[1].hasError());
  CHECK(results[0].value().status_code == 200);
  CHECK(results[1].value().status_code == 200);
  CHECK(results[0].value().body == "slow");
  CHECK(results[1].value().body == "fast");
  client.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE(
    "nghttp2-inspired client validation: missing :status yields protocol "
    "error") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{"server", "foo"}}, flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: server preface not starting with "
    "SETTINGS is rejected") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<uint8_t, 8> ping_data{1, 2, 3, 4, 5, 6, 7, 8};
    std::error_code ec;
    asio::write(
        sock, asio::buffer(make_frame(frame_type::ping, 0, 0, ping_data)), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto resp = async_simple::coro::syncAwait(client.async_get("/"));
  bool rejected =
      resp.net_err == std::make_error_code(std::errc::not_connected) ||
      resp.net_err == std::make_error_code(std::errc::protocol_error) ||
      resp.net_err == std::make_error_code(std::errc::connection_aborted);
  CHECK(rejected);

  client.close();
  srv.stop();
}

TEST_CASE(
    "nghttp2-inspired client validation: informational content-length is "
    "rejected") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{":status", "100"}, {"content-length", "0"}}, flags::END_HEADERS));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE("nghttp2-inspired client validation: duplicate :status is rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {":status", "200"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: unexpected pseudo header is "
    "rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {":scheme", "https"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: late pseudo header is rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{"server", "foo"}, {":status", "200"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: malformed status code is rejected") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{":status", "2000"}}, flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: invalid content-length is rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {"content-length", "-1"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: duplicate content-length is "
    "rejected") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{":status", "200"}, {"content-length", "0"}, {"content-length", "0"}},
      flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE("nghttp2-inspired client validation: connection header is rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {"connection", "close"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: informational END_STREAM is "
    "rejected") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{":status", "100"}}, flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: informational then final response "
    "succeeds") {
  std::string frames;
  frames += build_header_frame({{":status", "100"}}, flags::END_HEADERS);
  frames += build_header_frame({{":status", "200"}},
                               flags::END_HEADERS | flags::END_STREAM);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body.empty());
}

TEST_CASE(
    "nghttp2-inspired client validation: informational followed by empty DATA "
    "without END_STREAM is accepted") {
  std::string frames;
  frames += build_header_frame({{":status", "100"}}, flags::END_HEADERS);
  std::vector<uint8_t> empty_payload;
  frames += make_frame(frame_type::data, 0, 1, empty_payload);
  frames += build_header_frame({{":status", "200"}},
                               flags::END_HEADERS | flags::END_STREAM);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
}

TEST_CASE(
    "nghttp2-inspired client validation: informational followed by empty DATA "
    "with END_STREAM is rejected") {
  std::string frames;
  frames += build_header_frame({{":status", "100"}}, flags::END_HEADERS);
  std::vector<uint8_t> empty_payload;
  frames += make_frame(frame_type::data, flags::END_STREAM, 1, empty_payload);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: informational followed by nonempty "
    "DATA is rejected") {
  std::string frames;
  frames += build_header_frame({{":status", "100"}}, flags::END_HEADERS);
  std::vector<uint8_t> payload{'b', 'a', 'd'};
  frames += make_frame(frame_type::data, flags::END_STREAM, 1, payload);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: 204 with zero content-length is "
    "accepted") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "204"}, {"content-length", "0"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 204);
}

TEST_CASE(
    "nghttp2-inspired client validation: 204 with nonzero content-length is "
    "rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "204"}, {"content-length", "100"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE("nghttp2-inspired client validation: status 101 is rejected") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{":status", "101"}}, flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: host header on response is accepted") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {"host", "/localhost"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
}

TEST_CASE(
    "nghttp2-inspired client validation: response content-length mismatch on "
    "END_STREAM yields protocol error") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {"content-length", "20"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: response content-length mismatch "
    "after DATA yields protocol error") {
  std::string frames;
  frames += build_header_frame({{":status", "200"}, {"content-length", "20"}},
                               flags::END_HEADERS);
  std::vector<uint8_t> empty_payload;
  frames += make_frame(frame_type::data, flags::END_STREAM, 1, empty_payload);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: inbound frame exceeding advertised "
    "max_frame_size is rejected") {
  std::string frames;
  frames += build_header_frame({{":status", "200"}}, flags::END_HEADERS);
  std::vector<uint8_t> payload(coro_http2_client::MAX_FRAME_SIZE + 1,
                               uint8_t{'x'});
  frames += make_frame(frame_type::data, flags::END_STREAM, 1,
                       std::span<const uint8_t>(payload));

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::message_size));
}

TEST_CASE(
    "nghttp2-inspired client validation: HEADERS on stream 0 is rejected") {
  auto resp = run_client_with_raw_response(build_header_frame(
      {{":status", "200"}}, flags::END_HEADERS | flags::END_STREAM, 0));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE("nghttp2-inspired client validation: DATA on stream 0 is rejected") {
  std::vector<uint8_t> payload{'b', 'a', 'd'};
  auto resp = run_client_with_raw_response(
      make_frame(frame_type::data, flags::END_STREAM, 0,
                 std::span<const uint8_t>(payload)));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: server GOAWAY on nonzero stream is "
    "rejected") {
  std::array<uint8_t, 8> payload{};
  auto resp = run_client_with_raw_response(
      make_frame(frame_type::goaway, 0, 1, std::span<const uint8_t>(payload)));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: server GOAWAY short payload is "
    "rejected") {
  std::array<uint8_t, 4> payload{};
  auto resp = run_client_with_raw_response(
      make_frame(frame_type::goaway, 0, 0, std::span<const uint8_t>(payload)));

  CHECK(resp.net_err == std::make_error_code(std::errc::message_size));
}

TEST_CASE(
    "nghttp2-inspired client validation: server PING on nonzero stream is "
    "rejected") {
  std::array<uint8_t, 8> payload{};
  auto resp = run_client_with_raw_response(
      make_frame(frame_type::ping, 0, 1, std::span<const uint8_t>(payload)));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: server WINDOW_UPDATE short payload is "
    "rejected") {
  std::array<uint8_t, 3> payload{};
  auto resp = run_client_with_raw_response(make_frame(
      frame_type::window_update, 0, 0, std::span<const uint8_t>(payload)));

  CHECK(resp.net_err == std::make_error_code(std::errc::message_size));
}

TEST_CASE(
    "nghttp2-inspired client validation: server SETTINGS_ENABLE_PUSH is "
    "rejected") {
  // RFC 7540 section 6.5.2: server MUST NOT set ENABLE_PUSH to anything other
  // than 0. Value 0 is valid; value 2+ is a protocol error.
  std::string frames;
  std::array<settings_entry, 1> settings{
      settings_entry{settings_param::enable_push, 2},
  };
  frames += make_settings_frame(settings);
  frames += build_header_frame({{":status", "200"}},
                               flags::END_HEADERS | flags::END_STREAM);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: SETTINGS_HEADER_TABLE_SIZE updates "
    "outbound encoder") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<settings_entry, 1> settings{
        settings_entry{settings_param::header_table_size, 0},
    };
    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame(settings)), ec);
    if (ec)
      return;

    frame_header hdr{};
    std::vector<uint8_t> payload;
    for (int i = 0; i < 20; ++i) {
      if (!read_raw_frame(sock, hdr, payload))
        return;
      if (hdr.type == frame_type::settings ||
          hdr.type == frame_type::window_update)
        continue;
      if (hdr.type == frame_type::headers && hdr.stream_id == 1) {
        REQUIRE(!payload.empty());
        CHECK((payload[0] & 0xe0) == 0x20);
        break;
      }
    }

    std::string response_frames;
    response_frames +=
        build_header_frame({{":status", "200"}}, flags::END_HEADERS, 1);
    std::vector<uint8_t> ok_payload{'o', 'k'};
    response_frames += make_frame(frame_type::data, flags::END_STREAM, 1,
                                  std::span<const uint8_t>(ok_payload));
    asio::write(sock, asio::buffer(response_frames), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto resp = async_simple::coro::syncAwait(client.async_get("/"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "ok");

  client.close();
  srv.stop();
}

TEST_CASE(
    "coro_http2_client: first request waits for peer SETTINGS before sending "
    "HEADERS") {
  std::atomic<bool> saw_request_before_settings = false;
  std::atomic<bool> saw_request_after_settings = false;

  raw_h2_server_runner srv(
      [&saw_request_before_settings,
       &saw_request_after_settings](asio::ip::tcp::socket& sock) {
        if (!read_client_preface_and_settings(sock))
          return;

        saw_request_before_settings = saw_client_request_headers_within(
            sock, std::chrono::milliseconds(150));

        std::error_code ec;
        asio::write(sock, asio::buffer(make_settings_frame({})), ec);
        if (ec)
          return;

        saw_request_after_settings = wait_for_client_request_headers(sock);
        if (!saw_request_after_settings.load()) {
          sock.close(ec);
          return;
        }

        std::string response_frames;
        response_frames +=
            build_header_frame({{":status", "200"}}, flags::END_HEADERS, 1);
        std::vector<uint8_t> ok_payload{'o', 'k'};
        response_frames += make_frame(frame_type::data, flags::END_STREAM, 1,
                                      std::span<const uint8_t>(ok_payload));
        asio::write(sock, asio::buffer(response_frames), ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sock.close(ec);
      });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/blocked"));
  CHECK(!saw_request_before_settings.load());
  CHECK(saw_request_after_settings.load());
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "ok");

  client.close();
  srv.stop();
}

TEST_CASE(
    "coro_http2_client: request waiting on peer SETTINGS fails if peer "
    "closes") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::error_code ec;
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/"));
  CHECK(resp.net_err == std::make_error_code(std::errc::not_connected));

  client.close();
  srv.stop();
}

TEST_CASE(
    "coro_http2_client: SETTINGS_MAX_CONCURRENT_STREAMS applies backpressure "
    "to new requests") {
  bool saw_stream3_before_first_response = false;
  bool saw_stream3_after_first_response = false;

  raw_h2_server_runner srv([&saw_stream3_before_first_response,
                            &saw_stream3_after_first_response](
                               asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<settings_entry, 1> settings{
        settings_entry{settings_param::max_concurrent_streams, 1},
    };
    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame(settings)), ec);
    if (ec)
      return;

    if (!wait_for_client_request_headers(sock, 1))
      return;

    saw_stream3_before_first_response = saw_client_request_headers_within(
        sock, std::chrono::milliseconds(150), 3);

    std::string response_frames;
    response_frames +=
        build_header_frame({{":status", "200"}}, flags::END_HEADERS, 1);
    std::vector<uint8_t> first_payload{'o', 'n', 'e'};
    response_frames += make_frame(frame_type::data, flags::END_STREAM, 1,
                                  std::span<const uint8_t>(first_payload));
    asio::write(sock, asio::buffer(response_frames), ec);
    if (ec)
      return;

    saw_stream3_after_first_response = wait_for_client_request_headers(sock, 3);
    if (!saw_stream3_after_first_response)
      return;

    response_frames.clear();
    response_frames +=
        build_header_frame({{":status", "200"}}, flags::END_HEADERS, 3);
    std::vector<uint8_t> second_payload{'t', 'w', 'o'};
    response_frames += make_frame(frame_type::data, flags::END_STREAM, 3,
                                  std::span<const uint8_t>(second_payload));
    asio::write(sock, asio::buffer(response_frames), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto results = async_simple::coro::syncAwait(
      [&client]() -> async_simple::coro::Lazy<
                      std::vector<async_simple::Try<h2_client_response>>> {
        std::vector<async_simple::coro::Lazy<h2_client_response>> reqs;
        reqs.push_back(client.async_get("/one"));
        reqs.push_back(client.async_get("/two"));
        co_return co_await async_simple::coro::collectAll(std::move(reqs));
      }());

  REQUIRE(results.size() == 2);
  CHECK(!results[0].hasError());
  CHECK(!results[1].hasError());
  CHECK(!saw_stream3_before_first_response);
  CHECK(saw_stream3_after_first_response);
  CHECK(!results[0].value().net_err);
  CHECK(!results[1].value().net_err);
  CHECK(results[0].value().body == "one");
  CHECK(results[1].value().body == "two");

  client.close();
  srv.stop();
}

TEST_CASE(
    "nghttp2-inspired client validation: server PRIORITY on stream 0 is "
    "rejected") {
  std::string frames;
  auto payload = make_priority_payload(1);
  frames +=
      make_frame(frame_type::priority, 0, 0, std::span<const uint8_t>(payload));

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: server PUSH_PROMISE is rejected") {
  std::vector<uint8_t> payload{
      0x00,
      0x00,
      0x00,
      0x02,
  };
  std::string frames;
  frames += make_frame(frame_type::push_promise, flags::END_HEADERS, 1,
                       std::span<const uint8_t>(payload));

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE(
    "nghttp2-inspired client validation: response HEADERS priority "
    "self-dependency is rejected") {
  std::string frames;
  frames += build_priority_header_frame(
      {{":status", "200"}}, 1,
      flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 1);

  auto resp = run_client_with_raw_response(std::move(frames));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}

TEST_CASE("coro_http2_client: blocked upload is released by RST_STREAM") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<settings_entry, 1> settings{
        settings_entry{settings_param::initial_window_size, 0},
    };
    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame(settings)), ec);
    if (ec)
      return;

    if (!wait_for_client_request_headers(sock))
      return;

    asio::write(sock, asio::buffer(make_rst_stream(1, h2_error_code::cancel)),
                ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto start = std::chrono::steady_clock::now();
  auto resp = async_simple::coro::syncAwait(
      client.async_post("/upload", std::string(1 << 20, 'x')));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  CHECK(resp.net_err == std::make_error_code(std::errc::operation_canceled));
  CHECK(elapsed.count() < 1000);

  client.close();
  srv.stop();
}

TEST_CASE("coro_http2_client: blocked upload is released by GOAWAY") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<settings_entry, 1> settings{
        settings_entry{settings_param::initial_window_size, 0},
    };
    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame(settings)), ec);
    if (ec)
      return;

    if (!wait_for_client_request_headers(sock))
      return;

    asio::write(sock, asio::buffer(make_goaway(1, h2_error_code::no_error)),
                ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto start = std::chrono::steady_clock::now();
  auto resp = async_simple::coro::syncAwait(
      client.async_post("/upload", std::string(1 << 20, 'x')));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  CHECK(resp.net_err == std::make_error_code(std::errc::connection_aborted));
  CHECK(elapsed.count() < 1000);

  client.close();
  srv.stop();
}

TEST_CASE(
    "coro_http2_client: GOAWAY keeps streams up to last_stream_id alive") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame({})), ec);
    if (ec)
      return;

    bool got_stream1 = false;
    bool got_stream3 = false;
    frame_header hdr{};
    std::vector<uint8_t> payload;
    for (int i = 0; i < 20 && (!got_stream1 || !got_stream3); ++i) {
      if (!read_raw_frame(sock, hdr, payload))
        return;
      if (hdr.type == frame_type::settings ||
          hdr.type == frame_type::window_update) {
        continue;
      }
      if (hdr.type == frame_type::headers && hdr.stream_id == 1)
        got_stream1 = true;
      if (hdr.type == frame_type::headers && hdr.stream_id == 3)
        got_stream3 = true;
    }
    if (!got_stream1 || !got_stream3)
      return;

    std::string response_frames;
    response_frames += make_goaway(1, h2_error_code::no_error);
    response_frames +=
        build_header_frame({{":status", "200"}}, flags::END_HEADERS, 1);
    std::vector<uint8_t> ok_payload{'o', 'k'};
    response_frames += make_frame(frame_type::data, flags::END_STREAM, 1,
                                  std::span<const uint8_t>(ok_payload));
    asio::write(sock, asio::buffer(response_frames), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  auto results = async_simple::coro::syncAwait(
      [&client]() -> async_simple::coro::Lazy<
                      std::vector<async_simple::Try<h2_client_response>>> {
        std::vector<async_simple::coro::Lazy<h2_client_response>> reqs;
        reqs.push_back(client.async_get("/ok"));
        reqs.push_back(client.async_get("/ignored"));
        co_return co_await async_simple::coro::collectAll(std::move(reqs));
      }());

  REQUIRE(results.size() == 2);
  CHECK(!results[0].hasError());
  CHECK(!results[1].hasError());
  CHECK(!results[0].value().net_err);
  CHECK(results[0].value().status_code == 200);
  CHECK(results[0].value().body == "ok");
  CHECK(results[1].value().net_err ==
        std::make_error_code(std::errc::connection_aborted));

  client.close();
  srv.stop();
}

TEST_CASE(
    "nghttp2-inspired client validation: malformed response stream does not "
    "tear down sibling stream") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame({})), ec);
    if (ec)
      return;

    bool got_stream1 = false;
    bool got_stream3 = false;
    frame_header hdr{};
    std::vector<uint8_t> payload;
    for (int i = 0; i < 20 && (!got_stream1 || !got_stream3); ++i) {
      if (!read_raw_frame(sock, hdr, payload))
        return;
      if (hdr.type == frame_type::settings ||
          hdr.type == frame_type::window_update) {
        continue;
      }
      if (hdr.type == frame_type::headers && hdr.stream_id == 1)
        got_stream1 = true;
      if (hdr.type == frame_type::headers && hdr.stream_id == 3)
        got_stream3 = true;
    }
    if (!got_stream1 || !got_stream3)
      return;

    std::string response_frames;
    response_frames += build_header_frame(
        {{"server", "bad"}}, flags::END_HEADERS | flags::END_STREAM, 1);
    response_frames +=
        build_header_frame({{":status", "200"}}, flags::END_HEADERS, 3);
    std::vector<uint8_t> ok_payload{'o', 'k'};
    response_frames += make_frame(frame_type::data, flags::END_STREAM, 3,
                                  std::span<const uint8_t>(ok_payload));
    asio::write(sock, asio::buffer(response_frames), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  auto results = async_simple::coro::syncAwait(
      [&client]() -> async_simple::coro::Lazy<
                      std::vector<async_simple::Try<h2_client_response>>> {
        std::vector<async_simple::coro::Lazy<h2_client_response>> reqs;
        reqs.push_back(client.async_get("/bad"));
        reqs.push_back(client.async_get("/ok"));
        co_return co_await async_simple::coro::collectAll(std::move(reqs));
      }());

  REQUIRE(results.size() == 2);
  CHECK(!results[0].hasError());
  CHECK(!results[1].hasError());
  CHECK(results[0].value().net_err ==
        std::make_error_code(std::errc::protocol_error));
  CHECK(!results[1].value().net_err);
  CHECK(results[1].value().status_code == 200);
  CHECK(results[1].value().body == "ok");

  client.close();
  srv.stop();
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test_http2_required_server: multiple routes dispatch correctly" *
          doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>("/a", [](h2_request&, h2_response& resp) {
    resp.set_status_and_body(200, "route-a");
  });
  srv.set_http_handler<cinatra::GET>("/b", [](h2_request&, h2_response& resp) {
    resp.set_status_and_body(200, "route-b");
  });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (auto [path, expected] :
       std::initializer_list<std::pair<const char*, const char*>>{
           {"/a", "route-a"}, {"/b", "route-b"}}) {
    asio::io_context clioc;
    asio::ip::tcp::socket client(clioc);
    connect_with_retry(client, port);
    asio::write(client, asio::buffer(build_get_frames(path)));
    auto [status, body] = read_h2_response(client);
    CHECK(status == 200);
    CHECK(body == expected);
    client.close();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server: second connection is not blocked by first" *
    doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  std::atomic<bool> slow_started = false;
  std::atomic<bool> release_slow = false;
  srv.set_http_handler<cinatra::GET>(
      "/slow",
      [&slow_started, &release_slow](
          h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        slow_started = true;
        while (!release_slow.load()) {
          co_await coro_io::sleep_for(std::chrono::milliseconds(10));
        }
        resp.set_status_and_body(200, "slow");
      });
  srv.set_http_handler<cinatra::GET>("/fast",
                                     [](h2_request&, h2_response& resp) {
                                       resp.set_status_and_body(200, "fast");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context slow_ioc;
  asio::ip::tcp::socket slow_client(slow_ioc);
  connect_with_retry(slow_client, port);
  asio::write(slow_client, asio::buffer(build_get_frames("/slow")));

  for (int i = 0; i < 100 && !slow_started.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(slow_started.load());

  asio::io_context fast_ioc;
  asio::ip::tcp::socket fast_client(fast_ioc);
  connect_with_retry(fast_client, port);
  asio::write(fast_client, asio::buffer(build_get_frames("/fast")));
  auto [fast_status, fast_body] = read_h2_response(fast_client);

  CHECK(fast_status == 200);
  CHECK(fast_body == "fast");

  release_slow = true;
  auto [slow_status, slow_body] = read_h2_response(slow_client);
  CHECK(slow_status == 200);
  CHECK(slow_body == "slow");

  fast_client.close();
  slow_client.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  srv.stop();
}

TEST_CASE("test_http2_required_server: stop closes active connections" *
          doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>("/hello",
                                     [](h2_request&, h2_response& resp) {
                                       resp.set_status_and_body(200, "hello");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  asio::write(client, asio::buffer(init));

  // Consume the server SETTINGS and ACK it so the connection is established.
  {
    std::array<uint8_t, 9> hdr_buf;
    std::vector<uint8_t> payload;
    for (int i = 0; i < 4; ++i) {
      asio::read(client, asio::buffer(hdr_buf));
      auto hdr = parse_frame_header(hdr_buf);
      payload.resize(hdr.length);
      if (hdr.length > 0)
        asio::read(client, asio::buffer(payload));
      if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
        asio::write(client, asio::buffer(make_settings_frame({}, true)));
        break;
      }
    }
  }

  srv.stop();

  set_test_socket_timeouts(client, 200);
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  bool closed = false;
  for (int i = 0; i < 5 && !closed; ++i) {
    std::error_code read_ec;
    auto n = asio::read(client, asio::buffer(hdr_buf), read_ec);
    if (read_ec || n == 0) {
      closed = true;
      break;
    }
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0) {
      asio::read(client, asio::buffer(payload), read_ec);
      closed = static_cast<bool>(read_ec);
    }
  }
  CHECK(closed);
  client.close();
}

TEST_CASE(
    "test_http2_required_server: stop closes active streams without waiting") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  std::atomic<bool> handler_started = false;
  std::atomic<bool> handler_finished = false;
  srv.set_http_handler<cinatra::GET>(
      "/slow",
      [&handler_started, &handler_finished](
          h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        handler_started = true;
        co_await coro_io::sleep_for(std::chrono::milliseconds(500));
        resp.set_status_and_body(200, "slow");
        handler_finished = true;
        co_return;
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  std::optional<h2_client_response> resp;
  std::thread request_thread([&] {
    resp = async_simple::coro::syncAwait(client.async_get("/slow"));
  });

  for (int i = 0; i < 50 && !handler_started.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(handler_started.load());

  srv.stop();
  request_thread.join();

  REQUIRE(resp.has_value());
  CHECK(resp->net_err);

  for (int i = 0; i < 100 && !handler_finished.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(handler_finished.load());

  client.close();
}

TEST_CASE(
    "test_http2_required_server: client GOAWAY lets active stream finish" *
    doctest::skip()) {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.set_http_handler<cinatra::GET>(
      "/slow",
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        co_await coro_io::sleep_for(std::chrono::milliseconds(150));
        resp.set_status_and_body(200, "slow");
        co_return;
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_with_retry(client, port);

  std::string frames = build_get_frames("/slow");
  frames += make_goaway(0, h2_error_code::no_error);
  asio::write(client, asio::buffer(frames));

  auto [status, body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(body == "slow");

  client.close();
  srv.stop();
}
#endif

TEST_CASE("single connection: fast stream is not blocked by slow stream") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        if (req.path == "/slow") {
          co_await coro_io::sleep_for(std::chrono::milliseconds(300));
          resp.set_status_and_body(200, "slow");
          co_return;
        }
        resp.set_status_and_body(200, "fast");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> slow_headers{
      {":method", "GET"},
      {":path", "/slow"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto slow_block = enc.encode(slow_headers);
  frames +=
      make_frame(frame_type::headers, flags::END_HEADERS | flags::END_STREAM, 1,
                 std::span<const uint8_t>(slow_block));

  std::vector<header_field> fast_headers{
      {":method", "GET"},
      {":path", "/fast"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto fast_block = enc.encode(fast_headers);
  frames +=
      make_frame(frame_type::headers, flags::END_HEADERS | flags::END_STREAM, 3,
                 std::span<const uint8_t>(fast_block));
  asio::write(client, asio::buffer(frames));

  hpack_decoder dec;
  std::string stream1_body;
  std::string stream3_body;
  auto start = std::chrono::steady_clock::now();
  std::optional<std::chrono::milliseconds> fast_done_after;

  for (int i = 0; i < 20 && (stream1_body.empty() || stream3_body.empty());
       ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::settings && !(evt->flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }
    if (evt->type != frame_type::data)
      continue;

    if (evt->stream_id == 1)
      stream1_body += evt->body;
    if (evt->stream_id == 3) {
      stream3_body += evt->body;
      if ((evt->flags & flags::END_STREAM) && !fast_done_after.has_value()) {
        fast_done_after = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
      }
    }
  }

  REQUIRE(fast_done_after.has_value());
  CHECK(fast_done_after->count() < 200);
  CHECK(stream3_body == "fast");
  CHECK(stream1_body == "slow");
  client.close();
}

TEST_CASE("PRIORITY scheduling prefers higher-weight response stream") {
  server_runner srv;
  srv.launch([](h2_request& req,
                h2_response& resp) -> async_simple::coro::Lazy<void> {
    resp.set_status_and_body(200, req.path == "/high" ? "HIGH!!" : "lowlow");
    co_return;
  });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> peer_settings{
      settings_entry{settings_param::initial_window_size, 3},
  };
  frames += make_settings_frame(peer_settings);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/low"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      0, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 1, 0);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/high"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      0, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 3, 255);
  asio::write(client, asio::buffer(frames));

  hpack_decoder dec;
  std::optional<uint32_t> first_data_stream;
  for (int i = 0; i < 20 && !first_data_stream.has_value(); ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::settings && !(evt->flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }
    if (evt->type == frame_type::data)
      first_data_stream = evt->stream_id;
  }

  REQUIRE(first_data_stream.has_value());
  CHECK(*first_data_stream == 3);
  client.close();
}

TEST_CASE("PRIORITY scheduling honors stream dependency before child body") {
  server_runner srv;
  srv.launch([](h2_request& req,
                h2_response& resp) -> async_simple::coro::Lazy<void> {
    resp.set_status_and_body(200, req.path == "/parent" ? "PARENT" : "child!");
    co_return;
  });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> peer_settings{
      settings_entry{settings_param::initial_window_size, 3},
  };
  frames += make_settings_frame(peer_settings);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/parent"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      0, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 1, 0);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/child"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      1, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 3, 255);
  asio::write(client, asio::buffer(frames));

  hpack_decoder dec;
  bool parent_done = false;
  bool saw_child_before_parent_done = false;
  bool parent_window_updated = false;

  for (int i = 0; i < 30 && !parent_done; ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::settings && !(evt->flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }
    if (evt->type != frame_type::data)
      continue;

    if (evt->stream_id == 3 && !parent_done)
      saw_child_before_parent_done = true;

    if (evt->stream_id == 1) {
      if (!parent_window_updated) {
        asio::write(client, asio::buffer(make_window_update(1, 3)));
        parent_window_updated = true;
      }
      if (evt->flags & flags::END_STREAM)
        parent_done = true;
    }
  }

  CHECK(parent_done);
  CHECK(!saw_child_before_parent_done);
  client.close();
}

TEST_CASE("PRIORITY reprioritization honors exclusive reparenting") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        if (req.path == "/parent")
          resp.set_status_and_body(200, "PARENT");
        else if (req.path == "/sibling")
          resp.set_status_and_body(200, "SIBLING");
        else
          resp.set_status_and_body(200, "EXCLUSV");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> peer_settings{
      settings_entry{settings_param::initial_window_size, 3},
  };
  frames += make_settings_frame(peer_settings);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/parent"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      0, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 1, 0);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/sibling"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      1, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 3, 255);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/exclusive"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      1, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 5, 0);
  auto reprioritize = make_priority_payload(1, 0, true);
  frames += make_frame(frame_type::priority, 0, 5,
                       std::span<const uint8_t>(reprioritize));
  asio::write(client, asio::buffer(frames));

  hpack_decoder dec;
  bool parent_done = false;
  bool parent_window_updated = false;
  std::optional<uint32_t> first_after_parent;

  for (int i = 0; i < 40 && !first_after_parent.has_value(); ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::settings && !(evt->flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }
    if (evt->type != frame_type::data)
      continue;

    if (evt->stream_id == 1) {
      if (!parent_window_updated) {
        asio::write(client, asio::buffer(make_window_update(1, 3)));
        parent_window_updated = true;
      }
      if (evt->flags & flags::END_STREAM)
        parent_done = true;
      continue;
    }

    if (parent_done)
      first_after_parent = evt->stream_id;
  }

  REQUIRE(first_after_parent.has_value());
  CHECK(*first_after_parent == 5);
  client.close();
}

TEST_CASE(
    "PRIORITY reprioritization breaks ancestor cycles by moving the descendant "
    "subtree") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        if (req.path == "/one")
          resp.set_status_and_body(200, "ONEONE");
        else if (req.path == "/three")
          resp.set_status_and_body(200, "THREEX");
        else
          resp.set_status_and_body(200, "FIVE!!");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> peer_settings{
      settings_entry{settings_param::initial_window_size, 3},
  };
  frames += make_settings_frame(peer_settings);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/one"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      0, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 1, 0);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/three"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      1, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 3, 0);
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/five"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      3, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 5, 255);
  auto reprioritize = make_priority_payload(5, 0, false);
  frames += make_frame(frame_type::priority, 0, 1,
                       std::span<const uint8_t>(reprioritize));
  asio::write(client, asio::buffer(frames));

  hpack_decoder dec;
  std::optional<uint32_t> first_data_stream;
  for (int i = 0; i < 30 && !first_data_stream.has_value(); ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::settings && !(evt->flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }
    if (evt->type == frame_type::data)
      first_data_stream = evt->stream_id;
  }

  REQUIRE(first_data_stream.has_value());
  CHECK(*first_data_stream == 5);
  client.close();
}

TEST_CASE("response body respects peer initial window size") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "0123456789");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> peer_settings{
      settings_entry{settings_param::initial_window_size, 5},
  };
  frames += make_settings_frame(peer_settings);

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", "/win"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);
  frames +=
      make_frame(frame_type::headers, flags::END_HEADERS | flags::END_STREAM, 1,
                 std::span<const uint8_t>(block));
  asio::write(client, asio::buffer(frames));

  hpack_decoder dec;
  std::string body;
  int first_chunk = -1;
  bool seen_split = false;
  for (int i = 0; i < 20 && body.size() < 5; ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::settings && !(evt->flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }
    if (evt->type == frame_type::data && evt->stream_id == 1) {
      first_chunk = static_cast<int>(evt->body.size());
      body += evt->body;
      seen_split = !(evt->flags & flags::END_STREAM);
      break;
    }
  }

  REQUIRE(first_chunk == 5);
  CHECK(seen_split);
  CHECK(body == "01234");

  asio::write(client, asio::buffer(make_window_update(1, 5)));

  for (int i = 0; i < 20 && body.size() < 10; ++i) {
    auto evt = read_one_frame_event(client, dec);
    REQUIRE(evt.has_value());
    if (evt->type == frame_type::data && evt->stream_id == 1)
      body += evt->body;
  }

  CHECK(body == "0123456789");
  client.close();
}

TEST_CASE("PRIORITY frame with stream 0 triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  auto payload = make_priority_payload(1);
  init +=
      make_frame(frame_type::priority, 0, 0, std::span<const uint8_t>(payload));
  asio::write(client, asio::buffer(init));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("PUSH_PROMISE from client triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  std::vector<uint8_t> payload{
      0x00,
      0x00,
      0x00,
      0x02,
  };
  init += make_frame(frame_type::push_promise, flags::END_HEADERS, 1,
                     std::span<const uint8_t>(payload));
  asio::write(client, asio::buffer(init));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("GOAWAY with stream 1 triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::array<uint8_t, 8> payload{};
  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  init +=
      make_frame(frame_type::goaway, 0, 1, std::span<const uint8_t>(payload));
  asio::write(client, asio::buffer(init));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("GOAWAY with short payload triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::array<uint8_t, 4> payload{};
  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  init +=
      make_frame(frame_type::goaway, 0, 0, std::span<const uint8_t>(payload));
  asio::write(client, asio::buffer(init));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("request HEADERS priority self-dependency triggers RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});
  frames += build_priority_header_frame(
      {{":method", "GET"},
       {":path", "/"},
       {":scheme", "http"},
       {":authority", "localhost"}},
      1, flags::END_HEADERS | flags::END_STREAM | flags::PRIORITY, 1);
  asio::write(client, asio::buffer(frames));

  // RFC 7540 section 5.3.1: self-dependency is a stream error, not connection
  // error.
  CHECK(read_until_frame_type(client, frame_type::rst_stream));
  client.close();
}

// ---------------------------------------------------------------------------
// CONTINUATION hardening tests
// ---------------------------------------------------------------------------

// Helper: build HEADERS without END_HEADERS (for CONTINUATION tests)
static std::string build_get_no_end_headers(const std::string& path,
                                            uint32_t stream_id = 1) {
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);

  // HEADERS without END_HEADERS, with END_STREAM
  frames += make_frame(frame_type::headers, flags::END_STREAM, stream_id,
                       std::span<const uint8_t>(block));
  return frames;
}

static std::string build_post_headers(const std::string& path,
                                      uint32_t stream_id = 1,
                                      bool end_stream = false) {
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "POST"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);
  uint8_t flags_bits = flags::END_HEADERS;
  if (end_stream)
    flags_bits |= flags::END_STREAM;
  frames += make_frame(frame_type::headers, flags_bits, stream_id,
                       std::span<const uint8_t>(block));
  return frames;
}

TEST_CASE("SETTINGS ACK with payload triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});
  std::vector<uint8_t> bad_ack_payload{0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
  frames += make_frame(frame_type::settings, flags::ACK, 0, bad_ack_payload);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("SETTINGS invalid max_frame_size triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> bad_settings{
      settings_entry{settings_param::max_frame_size, 1024},
  };
  frames += make_settings_frame(bad_settings);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("DATA without END_STREAM yields batched WINDOW_UPDATE frames") {
  // WINDOW_UPDATE is now batched: the server accumulates consumed bytes and
  // only sends WINDOW_UPDATE when at least half of the initial window (65535/2)
  // has been consumed.  Send enough DATA to cross the threshold.
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames = build_post_headers("/upload", 1, false);
  asio::write(client, asio::buffer(frames));

  // Wait for server SETTINGS and ACK them.
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < 10; ++i) {
    if (!read_exact_with_timeout(
            client, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
            std::chrono::milliseconds(2000))) {
      break;
    }
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0 &&
        !read_exact_with_timeout(
            client, std::span<uint8_t>(payload.data(), payload.size()),
            std::chrono::milliseconds(2000))) {
      break;
    }
    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      break;
    }
  }

  // Send multiple DATA frames totalling > half the initial window (32768+)
  // to trigger batched WINDOW_UPDATE. MAX_FRAME_SIZE is 16384.
  constexpr size_t kChunkSize = 16384;
  constexpr int NUM_CHUNKS = 3;  // 3 * 16384 = 49152 > 32767 threshold
  std::string chunk_data(kChunkSize, 'X');
  auto chunk_span = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(chunk_data.data()), kChunkSize);
  for (int i = 0; i < NUM_CHUNKS; ++i) {
    auto data_frame = make_frame(frame_type::data, 0, 1, chunk_span);
    std::error_code ec;
    asio::write(client, asio::buffer(data_frame), ec);
    if (ec)
      break;
  }

  bool got_conn_window_update = false;
  bool got_stream_window_update = false;
  for (int i = 0;
       i < 30 && !(got_conn_window_update && got_stream_window_update); ++i) {
    if (!read_exact_with_timeout(
            client, std::span<uint8_t>(hdr_buf.data(), hdr_buf.size()),
            std::chrono::milliseconds(2000))) {
      break;
    }
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0 &&
        !read_exact_with_timeout(
            client, std::span<uint8_t>(payload.data(), payload.size()),
            std::chrono::milliseconds(2000))) {
      break;
    }

    if (hdr.type == frame_type::window_update && hdr.length == 4) {
      uint32_t increment = ((uint32_t(payload[0]) & 0x7f) << 24) |
                           (uint32_t(payload[1]) << 16) |
                           (uint32_t(payload[2]) << 8) | uint32_t(payload[3]);
      if (hdr.stream_id == 0 && increment > 0)
        got_conn_window_update = true;
      if (hdr.stream_id == 1 && increment > 0)
        got_stream_window_update = true;
    }
  }

  CHECK(got_conn_window_update);
  CHECK(got_stream_window_update);
  client.close();
}

TEST_CASE("HEADERS with invalid padding triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});
  std::vector<uint8_t> bad_headers{0x05, 0x82};
  frames += make_frame(frame_type::headers,
                       flags::END_HEADERS | flags::END_STREAM | flags::PADDED,
                       1, bad_headers);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("DATA with invalid padding triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, srv.port());

  std::string frames = build_post_headers("/upload", 1, false);
  std::vector<uint8_t> bad_data{0x05, 'x'};
  frames += make_frame(frame_type::data, flags::END_STREAM | flags::PADDED, 1,
                       bad_data);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: missing :path triggers RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: missing :method triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":path", "/"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: missing :scheme triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", "/"},
      {":authority", "localhost"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: connection header triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},          {":path", "/"},          {":scheme", "http"},
      {":authority", "localhost"}, {"connection", "close"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: duplicate :path triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},          {":path", "/"}, {":scheme", "http"},
      {":authority", "localhost"}, {":path", "/"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: duplicate :method triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},          {":path", "/"},     {":scheme", "http"},
      {":authority", "localhost"}, {":method", "GET"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: duplicate :scheme triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},          {":path", "/"},      {":scheme", "http"},
      {":authority", "localhost"}, {":scheme", "http"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: invalid content-length triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "POST"},      {":path", "/"},
      {":scheme", "http"},      {":authority", "localhost"},
      {"content-length", "-1"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: asterisk path with GET triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", "*"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: asterisk path with OPTIONS is "
    "accepted") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(
            req.method == "OPTIONS" && req.path == "*" ? 200 : 400, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "OPTIONS"},
      {":path", "*"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  auto [status, body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(body == "ok");
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: pseudo header after regular header "
    "triggers RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},
      {"x-foo", "bar"},
      {":path", "/"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: duplicate content-length triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "POST"},     {":path", "/"},
      {":scheme", "http"},     {":authority", "localhost"},
      {"content-length", "0"}, {"content-length", "0"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: content-length mismatch triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "POST"},         {":path", "/upload"},    {":scheme", "http"},
      {":authority", "localhost"}, {"content-length", "4"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: malformed stream does not tear down "
    "sibling stream") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(req.path == "/ok" ? 200 : 404, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});
  frames += build_header_frame(
      {{":method", "GET"}, {":scheme", "http"}, {":authority", "localhost"}},
      flags::END_HEADERS | flags::END_STREAM, 1);
  frames += build_header_frame({{":method", "GET"},
                                {":path", "/ok"},
                                {":scheme", "http"},
                                {":authority", "localhost"}},
                               flags::END_HEADERS | flags::END_STREAM, 3);
  asio::write(client, asio::buffer(frames));

  bool got_rst_stream1 = false;
  int status3 = 0;
  std::string body3;
  hpack_decoder dec;
  for (int i = 0; i < 20 && (!got_rst_stream1 || body3 != "ok"); ++i) {
    frame_header hdr{};
    std::vector<uint8_t> payload;
    REQUIRE(read_raw_frame(client, hdr, payload));

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }

    if (hdr.type == frame_type::rst_stream && hdr.stream_id == 1) {
      got_rst_stream1 = true;
      continue;
    }

    if (hdr.type == frame_type::headers && hdr.stream_id == 3) {
      auto decoded = dec.decode(std::span<const uint8_t>(payload));
      for (auto& h : decoded)
        if (h.name == ":status")
          status3 = std::stoi(h.value);
      continue;
    }

    if (hdr.type == frame_type::data && hdr.stream_id == 3) {
      body3.append(reinterpret_cast<const char*>(payload.data()),
                   payload.size());
    }
  }

  CHECK(got_rst_stream1);
  CHECK(status3 == 200);
  CHECK(body3 == "ok");
  client.close();
}

TEST_CASE("CONTINUATION: interleaved DATA frame rejected") {
  // Send HEADERS without END_HEADERS, then a DATA frame -> GOAWAY
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  auto init = build_get_no_end_headers("/hello", 1);
  asio::write(client, asio::buffer(init));

  // Send DATA frame while CONTINUATION is expected
  std::vector<uint8_t> data_payload{0x01, 0x02};
  asio::write(client,
              asio::buffer(make_frame(frame_type::data, 0, 1, data_payload)));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("CONTINUATION: different stream CONTINUATION rejected") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  auto init = build_get_no_end_headers("/hello", 1);
  asio::write(client, asio::buffer(init));

  // Send CONTINUATION for stream 3 (wrong stream)
  std::vector<uint8_t> empty_block;
  asio::write(client,
              asio::buffer(make_frame(frame_type::continuation,
                                      flags::END_HEADERS, 3, empty_block)));

  CHECK(read_until_frame_type(client, frame_type::goaway));
  client.close();
}

TEST_CASE("CONTINUATION: correct sequence succeeds") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(req.path == "/cont" ? 200 : 404, "cont ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  // Build frames: preface + SETTINGS + HEADERS(no END_HEADERS) +
  // CONTINUATION(END_HEADERS)
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", "/cont"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);

  // Split the block: first part in HEADERS, rest in CONTINUATION
  size_t split = block.size() / 2;
  auto part1 = std::span<const uint8_t>(block.data(), split);
  auto part2 =
      std::span<const uint8_t>(block.data() + split, block.size() - split);

  frames += make_frame(frame_type::headers, flags::END_STREAM, 1, part1);
  frames += make_frame(frame_type::continuation, flags::END_HEADERS, 1, part2);
  asio::write(client, asio::buffer(frames));

  auto [status, body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(body == "cont ok");
  client.close();
}

// ---------------------------------------------------------------------------
// Stream state machine tests
// -------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("RST_STREAM: invalid payload length triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  // Send preface + SETTINGS + HEADERS (to create stream 1)
  asio::write(client, asio::buffer(build_get_frames("/hello")));

  // Now send RST_STREAM with wrong payload length (2 bytes instead of 4)
  std::vector<uint8_t> bad_payload{0x00, 0x00};
  asio::write(client, asio::buffer(make_frame(frame_type::rst_stream, 0, 1,
                                              bad_payload)));

  bool got_goaway = false;
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < 10 && !got_goaway; ++i) {
    std::error_code ec;
    asio::read(client, asio::buffer(hdr_buf), ec);
    if (ec)
      break;
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0)
      asio::read(client, asio::buffer(payload), ec);
    if (ec)
      break;

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK))
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
    else if (hdr.type == frame_type::goaway)
      got_goaway = true;
  }
  CHECK(got_goaway);
  client.close();
}

TEST_CASE("RST_STREAM: stream_id 0 triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string init(CLIENT_PREFACE);
  init += make_settings_frame({});
  asio::write(client, asio::buffer(init));

  // Finish the SETTINGS handshake first so the subsequent GOAWAY assertion
  // only exercises RST_STREAM validation, not frame ordering races.
  {
    std::array<uint8_t, 9> hdr_buf;
    std::vector<uint8_t> payload;
    for (int i = 0; i < 4; ++i) {
      asio::read(client, asio::buffer(hdr_buf));
      auto hdr = parse_frame_header(hdr_buf);
      payload.resize(hdr.length);
      if (hdr.length > 0)
        asio::read(client, asio::buffer(payload));
      if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
        asio::write(client, asio::buffer(make_settings_frame({}, true)));
        break;
      }
    }
  }

  // RST_STREAM on stream 0 is illegal.
  asio::write(client, asio::buffer(make_rst_stream(0, h2_error_code::cancel)));

  bool got_goaway = false;
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < 10 && !got_goaway; ++i) {
    std::error_code ec;
    asio::read(client, asio::buffer(hdr_buf), ec);
    if (ec)
      break;
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0)
      asio::read(client, asio::buffer(payload), ec);
    if (ec)
      break;

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK))
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
    else if (hdr.type == frame_type::goaway)
      got_goaway = true;
  }
  CHECK(got_goaway);
  client.close();
}

TEST_CASE("stream ID: non-increasing stream ID triggers GOAWAY") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  // Build: preface + SETTINGS + HEADERS(stream=3) + HEADERS(stream=1)
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "GET"},
      {":path", "/"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);
  // Stream 3 first (valid)
  frames +=
      make_frame(frame_type::headers, flags::END_HEADERS | flags::END_STREAM, 3,
                 std::span<const uint8_t>(block));
  // Stream 1 second (invalid: 1 < 3)
  block = enc.encode(hdrs);
  frames +=
      make_frame(frame_type::headers, flags::END_HEADERS | flags::END_STREAM, 1,
                 std::span<const uint8_t>(block));
  asio::write(client, asio::buffer(frames));

  bool got_goaway = false;
  std::array<uint8_t, 9> hdr_buf;
  std::vector<uint8_t> payload;
  for (int i = 0; i < 15 && !got_goaway; ++i) {
    std::error_code ec;
    asio::read(client, asio::buffer(hdr_buf), ec);
    if (ec)
      break;
    auto hdr = parse_frame_header(hdr_buf);
    payload.resize(hdr.length);
    if (hdr.length > 0)
      asio::read(client, asio::buffer(payload), ec);
    if (ec)
      break;

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK))
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
    else if (hdr.type == frame_type::goaway)
      got_goaway = true;
  }
  CHECK(got_goaway);
  client.close();
}

TEST_CASE("HEADERS + DATA with END_STREAM dispatches correctly") {
  server_runner srv;
  std::string received_body;
  srv.launch(
      [&received_body](h2_request& req,
                       h2_response& resp) -> async_simple::coro::Lazy<void> {
        received_body = req.body;
        resp.set_status_and_body(200, "got it");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  // Build: preface + SETTINGS + HEADERS (no END_STREAM) + DATA (END_STREAM)
  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});

  hpack_encoder enc;
  std::vector<header_field> hdrs{
      {":method", "POST"},
      {":path", "/upload"},
      {":scheme", "http"},
      {":authority", "localhost"},
  };
  auto block = enc.encode(hdrs);
  frames += make_frame(frame_type::headers, flags::END_HEADERS, 1,
                       std::span<const uint8_t>(block));
  std::string body_data = "hello world";
  auto body_span = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(body_data.data()), body_data.size());
  frames += make_frame(frame_type::data, flags::END_STREAM, 1, body_span);
  asio::write(client, asio::buffer(frames));

  auto [status, resp_body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(resp_body == "got it");
  CHECK(received_body == "hello world");
  client.close();
}

TEST_CASE(
    "response header block is fragmented into CONTINUATION when oversized") {
  server_runner srv;
  auto big_value = make_large_header_value(40000);
  srv.launch([big_value](h2_request&,
                         h2_response& resp) -> async_simple::coro::Lazy<void> {
    resp.add_header("x-big", big_value);
    resp.set_status_and_body(200, "ok");
    co_return;
  });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);
  asio::write(client,
              asio::buffer(build_get_frames("/large-response-headers")));

  hpack_decoder dec;
  std::vector<uint8_t> header_block;
  bool saw_headers = false;
  bool saw_continuation = false;
  bool header_block_complete = false;
  bool header_end_stream = false;
  int status = 0;
  std::string body;

  for (int i = 0; i < 64 && (!header_block_complete || body != "ok"); ++i) {
    frame_header hdr{};
    std::vector<uint8_t> payload;
    REQUIRE(read_raw_frame(client, hdr, payload));

    if (hdr.type == frame_type::settings && !(hdr.flags & flags::ACK)) {
      asio::write(client, asio::buffer(make_settings_frame({}, true)));
      continue;
    }

    if (hdr.type == frame_type::headers && hdr.stream_id == 1) {
      saw_headers = true;
      header_block.assign(payload.begin(), payload.end());
      header_end_stream = (hdr.flags & flags::END_STREAM) != 0;
      CHECK(hdr.length == coro_http2_connection::MAX_FRAME_SIZE);
      CHECK((hdr.flags & flags::END_HEADERS) == 0);
      continue;
    }

    if (hdr.type == frame_type::continuation && hdr.stream_id == 1) {
      saw_continuation = true;
      header_block.insert(header_block.end(), payload.begin(), payload.end());
      if (hdr.flags & flags::END_HEADERS) {
        auto decoded = dec.decode(std::span<const uint8_t>(header_block));
        for (auto& h : decoded)
          if (h.name == ":status")
            status = std::stoi(h.value);
        header_block.clear();
        header_block_complete = true;
        if (header_end_stream)
          break;
      }
      continue;
    }

    if (hdr.type == frame_type::data && hdr.stream_id == 1) {
      body.append(reinterpret_cast<const char*>(payload.data()),
                  payload.size());
      if (hdr.flags & flags::END_STREAM)
        break;
    }
  }

  CHECK(saw_headers);
  CHECK(saw_continuation);
  CHECK(header_block_complete);
  CHECK(status == 200);
  CHECK(body == "ok");
  client.close();
}

TEST_CASE(
    "client request header block is fragmented into CONTINUATION when "
    "oversized") {
  auto big_value = make_large_header_value(40000);
  bool saw_continuation = false;
  std::string received_big_header;

  raw_h2_server_runner srv([&saw_continuation,
                            &received_big_header](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame({})), ec);
    if (ec)
      return;

    frame_header hdr{};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> header_block;
    for (int i = 0; i < 64; ++i) {
      if (!read_raw_frame(sock, hdr, payload))
        return;
      if (hdr.type == frame_type::settings ||
          hdr.type == frame_type::window_update) {
        continue;
      }
      if (hdr.type == frame_type::headers && hdr.stream_id == 1) {
        header_block.insert(header_block.end(), payload.begin(), payload.end());
        if (hdr.flags & flags::END_HEADERS)
          break;
        continue;
      }
      if (hdr.type == frame_type::continuation && hdr.stream_id == 1) {
        saw_continuation = true;
        header_block.insert(header_block.end(), payload.begin(), payload.end());
        if (hdr.flags & flags::END_HEADERS)
          break;
      }
    }

    hpack_decoder dec;
    auto decoded = dec.decode(std::span<const uint8_t>(header_block));
    for (auto& h : decoded) {
      if (h.name == "x-big")
        received_big_header = h.value;
    }

    auto response = build_header_frame({{":status", "200"}},
                                       flags::END_HEADERS | flags::END_STREAM);
    asio::write(sock, asio::buffer(response), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  h2_client_request req;
  req.path = "/large-request-headers";
  req.headers.push_back({"x-big", big_value});
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(saw_continuation);
  CHECK(received_big_header == big_value);
  client.close();
  srv.stop();
}

TEST_CASE("request trailers are preserved and dispatch succeeds") {
  server_runner srv;
  std::string received_body;
  std::string received_trailer;
  srv.launch([&received_body, &received_trailer](
                 h2_request& req,
                 h2_response& resp) -> async_simple::coro::Lazy<void> {
    received_body = req.body;
    for (auto& hf : req.trailers) {
      if (hf.name == "x-check")
        received_trailer = hf.value;
    }
    resp.set_status_and_body(received_trailer == "ok" ? 200 : 400, "done");
    co_return;
  });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  frames += make_settings_frame({});
  frames += build_header_frame({{":method", "POST"},
                                {":path", "/trailers"},
                                {":scheme", "http"},
                                {":authority", "localhost"},
                                {"content-length", "3"}},
                               flags::END_HEADERS, 1);

  std::string body_data = "hey";
  auto body_span = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(body_data.data()), body_data.size());
  frames += make_frame(frame_type::data, 0, 1, body_span);
  frames += build_header_frame({{"x-check", "ok"}},
                               flags::END_HEADERS | flags::END_STREAM, 1);
  asio::write(client, asio::buffer(frames));

  auto [status, body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(body == "done");
  CHECK(received_body == "hey");
  CHECK(received_trailer == "ok");
  client.close();
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE(
    "test_http2_required_server/client: request trailers are emitted after "
    "body") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  std::string received_body;
  std::string received_trailer;
  srv.set_http_handler<cinatra::POST>(
      "/trailers",
      [&received_body, &received_trailer](h2_request& req, h2_response& resp) {
        received_body = req.body;
        for (auto& hf : req.trailers) {
          if (hf.name == "x-check")
            received_trailer = hf.value;
        }
        resp.set_status_and_body(
            received_body == "hey" && received_trailer == "ok" ? 200 : 400,
            "done");
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  h2_client_request req;
  req.method = "POST";
  req.path = "/trailers";
  req.body = "hey";
  req.add_trailer("x-check", "ok");
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "done");
  CHECK(received_body == "hey");
  CHECK(received_trailer == "ok");

  client.close();
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server/client: request trailers can terminate an "
    "empty body") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  std::string received_body;
  std::string received_trailer;
  srv.set_http_handler<cinatra::POST>(
      "/empty-trailers",
      [&received_body, &received_trailer](h2_request& req, h2_response& resp) {
        received_body = req.body;
        for (auto& hf : req.trailers) {
          if (hf.name == "x-check")
            received_trailer = hf.value;
        }
        resp.set_status_and_body(
            received_body.empty() && received_trailer == "empty" ? 200 : 400,
            "done");
      });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  h2_client_request req;
  req.method = "POST";
  req.path = "/empty-trailers";
  req.add_trailer("x-check", "empty");
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "done");
  CHECK(received_body.empty());
  CHECK(received_trailer == "empty");

  client.close();
  srv.stop();
}
#endif

TEST_CASE("response trailers are preserved by client") {
  raw_h2_server_runner srv([](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame({})), ec);
    if (ec)
      return;
    if (!wait_for_client_request_headers(sock))
      return;

    std::string frames;
    frames += build_header_frame({{":status", "200"}}, flags::END_HEADERS, 1);
    std::string body = "abc";
    frames += make_frame(
        frame_type::data, 0, 1,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()),
                                 body.size()));
    frames += build_header_frame({{"x-finished", "yes"}},
                                 flags::END_HEADERS | flags::END_STREAM, 1);
    asio::write(sock, asio::buffer(frames), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/trailers"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "abc");
  REQUIRE(resp.trailers.size() == 1);
  CHECK(resp.trailers[0].name == "x-finished");
  CHECK(resp.trailers[0].value == "yes");
  client.close();
  srv.stop();
}

#ifdef CINATRA_ENABLE_SSL
TEST_CASE(
    "test_http2_required_server/client: response trailers are emitted after "
    "body") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>("/trailers",
                                     [](h2_request&, h2_response& resp) {
                                       resp.set_status_and_body(200, "abc");
                                       resp.add_trailer("x-finished", "yes");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/trailers"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "abc");
  REQUIRE(resp.trailers.size() == 1);
  CHECK(resp.trailers[0].name == "x-finished");
  CHECK(resp.trailers[0].value == "yes");

  client.close();
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server/client: response trailers can terminate an "
    "empty body") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>("/empty-trailers",
                                     [](h2_request&, h2_response& resp) {
                                       resp.status_code = 204;
                                       resp.add_trailer("x-finished", "empty");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  auto resp =
      async_simple::coro::syncAwait(client.async_get("/empty-trailers"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 204);
  CHECK(resp.body.empty());
  REQUIRE(resp.trailers.size() == 1);
  CHECK(resp.trailers[0].name == "x-finished");
  CHECK(resp.trailers[0].value == "empty");

  client.close();
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server/client: explicit response content-length is "
    "not duplicated") {
  ioc_runner runner;
  test_http2_required_server srv(runner.ioc, 0);
  srv.init_ssl(test_tls_cert_path(), test_tls_key_path(), "test");
  srv.set_http_handler<cinatra::GET>("/length",
                                     [](h2_request&, h2_response& resp) {
                                       resp.add_header("content-length", "5");
                                       resp.set_status_and_body(200, "hello");
                                     });
  uint16_t port = srv.start(*runner.exec);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(port), "https"));
  REQUIRE(!ec);

  auto resp = async_simple::coro::syncAwait(client.async_get("/length"));
  CHECK(!resp.net_err);
  CHECK(resp.status_code == 200);
  CHECK(resp.body == "hello");
  CHECK(std::count_if(resp.headers.begin(), resp.headers.end(),
                      [](const header_field& hf) {
                        return hf.name == "content-length";
                      }) == 1);

  client.close();
  srv.stop();
}
#endif

TEST_CASE(
    "coro_http2_client: peer max header list size rejects oversized request "
    "headers locally") {
  std::atomic<bool> saw_request_headers = false;
  raw_h2_server_runner srv([&saw_request_headers](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<settings_entry, 1> settings{
        settings_entry{settings_param::max_header_list_size, 256},
    };
    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame(settings)), ec);
    if (ec)
      return;

    saw_request_headers =
        saw_client_request_headers_within(sock, std::chrono::milliseconds(200));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  h2_client_request req;
  req.path = "/too-large";
  req.add_header("x-large", std::string(256, 'a'));
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(resp.net_err == std::make_error_code(std::errc::message_size));
  CHECK(!saw_request_headers.load());
  client.close();
  srv.stop();
}

TEST_CASE(
    "coro_http2_client: peer max header list size rejects oversized request "
    "trailers locally") {
  std::atomic<bool> saw_request_headers = false;
  raw_h2_server_runner srv([&saw_request_headers](asio::ip::tcp::socket& sock) {
    if (!read_client_preface_and_settings(sock))
      return;

    std::array<settings_entry, 1> settings{
        settings_entry{settings_param::max_header_list_size, 256},
    };
    std::error_code ec;
    asio::write(sock, asio::buffer(make_settings_frame(settings)), ec);
    if (ec)
      return;

    saw_request_headers =
        saw_client_request_headers_within(sock, std::chrono::milliseconds(200));
    sock.close(ec);
  });

  ioc_runner runner;
  coro_http2_client client(runner.exec.get());
  auto ec = async_simple::coro::syncAwait(
      client.connect("127.0.0.1", std::to_string(srv.port())));
  REQUIRE(!ec);

  h2_client_request req;
  req.method = "POST";
  req.path = "/too-large-trailer";
  req.add_trailer("x-large", std::string(256, 'a'));
  auto resp =
      async_simple::coro::syncAwait(client.async_request(std::move(req)));

  CHECK(resp.net_err == std::make_error_code(std::errc::message_size));
  CHECK(!saw_request_headers.load());
  client.close();
  srv.stop();
}

TEST_CASE(
    "test_http2_required_server: peer max header list size rejects oversized "
    "response headers locally" *
    doctest::skip()) {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.add_header("x-large", std::string(256, 'a'));
        resp.set_status_and_body(200, "unused");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> settings{
      settings_entry{settings_param::max_header_list_size, 128},
  };
  frames += make_settings_frame(settings);
  frames += build_header_frame({{":method", "GET"},
                                {":path", "/too-large"},
                                {":scheme", "http"},
                                {":authority", "localhost"}},
                               flags::END_HEADERS | flags::END_STREAM, 1);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "test_http2_required_server: peer max header list size rejects oversized "
    "response trailers locally" *
    doctest::skip()) {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "unused");
        resp.add_trailer("x-large", std::string(256, 'a'));
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::string frames(CLIENT_PREFACE);
  std::array<settings_entry, 1> settings{
      settings_entry{settings_param::max_header_list_size, 128},
  };
  frames += make_settings_frame(settings);
  frames += build_header_frame({{":method", "GET"},
                                {":path", "/too-large-trailer"},
                                {":scheme", "http"},
                                {":authority", "localhost"}},
                               flags::END_HEADERS | flags::END_STREAM, 1);
  asio::write(client, asio::buffer(frames));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE("nghttp2-inspired request validation: te trailers is accepted") {
  server_runner srv;
  srv.launch(
      [](h2_request& req, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(req.get_header("te") == "trailers" ? 200 : 400,
                                 "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},          {":path", "/"},     {":scheme", "http"},
      {":authority", "localhost"}, {"te", "trailers"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  auto [status, body] = read_h2_response(client);
  CHECK(status == 200);
  CHECK(body == "ok");
  client.close();
}

TEST_CASE(
    "nghttp2-inspired request validation: te other than trailers triggers "
    "RST_STREAM") {
  server_runner srv;
  srv.launch(
      [](h2_request&, h2_response& resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_body(200, "ok");
        co_return;
      });
  uint16_t port = srv.port();

  asio::io_context client_ioc;
  asio::ip::tcp::socket client(client_ioc);
  connect_direct(client, port);

  std::vector<header_field> hdrs{
      {":method", "GET"},          {":path", "/"}, {":scheme", "http"},
      {":authority", "localhost"}, {"te", "gzip"},
  };
  asio::write(client, asio::buffer(build_request_frames(hdrs)));

  CHECK(read_until_frame_type_on_stream(client, frame_type::rst_stream, 1));
  client.close();
}

TEST_CASE(
    "nghttp2-inspired client validation: transfer-encoding header is "
    "rejected") {
  auto resp = run_client_with_raw_response(
      build_header_frame({{":status", "200"}, {"transfer-encoding", "chunked"}},
                         flags::END_HEADERS | flags::END_STREAM));

  CHECK(resp.net_err == std::make_error_code(std::errc::protocol_error));
}
