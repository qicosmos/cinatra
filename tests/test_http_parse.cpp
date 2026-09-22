#define DOCTEST_CONFIG_IMPLEMENT

#include <array>
#include <cstdint>

#include "cinatra/coro_http_server.hpp"
#include "cinatra/secure_string_hash.hpp"
#include "doctest/doctest.h"

using namespace cinatra;

std::string_view REQ =
    "R(GET /wp-content/uploads/2010/03/hello-kitty-darth-vader-pink.jpg "
    "HTTP/1.1\r\n"
    "Host: www.kittyhell.com\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; U; Intel Mac OS X 10.6; ja-JP-mac; "
    "rv:1.9.2.3) Gecko/20100401 Firefox/3.6.3 "
    "Pathtraq/0.9\r\n"
    "Accept: "
    "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
    "Accept-Language: ja,en-us;q=0.7,en;q=0.3\r\n"
    "Accept-Encoding: gzip,deflate\r\n"
    "Accept-Charset: Shift_JIS,utf-8;q=0.7,*;q=0.7\r\n"
    "Keep-Alive: 115\r\n"
    "Connection: keep-alive\r\n"
    "Cookie: wp_ozh_wsa_visits=2; wp_ozh_wsa_visit_lasttime=xxxxxxxxxx; "
    "__utma=xxxxxxxxx.xxxxxxxxxx.xxxxxxxxxx.xxxxxxxxxx.xxxxxxxxxx.x; "
    "__utmz=xxxxxxxxx.xxxxxxxxxx.x.x.utmccn=(referral)|utmcsr=reader.livedoor."
    "com|utmcct=/reader/|utmcmd=referral\r\n"
    "\r\n)";

std::string_view multipart_str =
    "R(POST / HTTP/1.1\r\n"
    "User-Agent: PostmanRuntime/7.39.0\r\n"
    "Accept: */*\r\n"
    "Cache-Control: no-cache\r\n"
    "Postman-Token: 33c25732-1648-42ed-a467-cc9f1eb1e961\r\n"
    "Host: purecpp.cn\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "Connection: keep-alive\r\n"
    "Content-Type: multipart/form-data; "
    "boundary=--------------------------559980232503017651158362\r\n"
    "Cookie: CSESSIONID=87343c8a24f34e28be05efea55315aab\r\n"
    "\r\n"
    "----------------------------559980232503017651158362\r\n"
    "Content-Disposition: form-data; name=\"test\"\r\n"
    "tom\r\n"
    "----------------------------559980232503017651158362--\r\n";

std::string_view bad_multipart_str =
    "R(POST / HTTP/1.1\r\n"
    "User-Agent: PostmanRuntime/7.39.0\r\n"
    "Accept: */*\r\n"
    "Cache-Control: no-cache\r\n"
    "Postman-Token: 33c25732-1648-42ed-a467-cc9f1eb1e961\r\n"
    "Host: purecpp.cn\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "Connection: keep-alive\r\n"
    "Content-Type: multipart/form-data; boundary=559980232503017651158362\r\n"
    "Cookie: CSESSIONID=87343c8a24f34e28be05efea55315aab\r\n"
    "\r\n"
    "559980232503017651158362\r\n"
    "Content-Disposition: form-data; name=\"test\"\r\n"
    "tom\r\n"
    "559980232503017651158362--\r\n";

std::string_view resp_str =
    "R(HTTP/1.1 400 Bad Request\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 20\r\n"
    "Host: cinatra\r\n"
    "\r\n\r\n"
    "the url is not right)";

TEST_CASE("http_parser test") {
  http_parser parser{};
  parser.parse_request(REQ.data(), REQ.size(), 0);
  CHECK(parser.body_len() == 0);
  CHECK(parser.body_len() + parser.header_len() == parser.total_len());
  CHECK(parser.has_connection());

  parser = {};
  std::string_view str(REQ.data(), 20);
  int ret = parser.parse_request(str.data(), str.size(), 0);
  CHECK(ret < 0);

  parser = {};
  ret = parser.parse_request(multipart_str.data(), multipart_str.size(), 0);
  CHECK(ret > 0);
  auto boundary = parser.get_boundary();
  CHECK(boundary == "--------------------------559980232503017651158362");

  parser = {};
  ret = parser.parse_request(bad_multipart_str.data(), bad_multipart_str.size(),
                             0);
  CHECK(ret > 0);
  auto bad_boundary = parser.get_boundary();
  CHECK(bad_boundary.empty());

  parser = {};
  std::string_view part_resp(resp_str.data(), 20);
  ret = parser.parse_response(part_resp.data(), part_resp.size(), 0);
  CHECK(ret < 0);
}

std::string_view req_str =
    "R(GET /wp-content/uploads/2010/03/hello-kitty-darth-vader-pink.jpg "
    "HTTP/1.1\r\n"
    "Content-Type: application/octet-stream"
    "Host: cinatra\r\n"
    "\r\n)";

std::string_view req_str1 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: cinatra\r\n"
    "\r\n)";

std::string_view req_str2 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: websocket\r\n"
    "\r\n)";

std::string_view req_str3 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Extensions: permessage-deflate\r\n"
    "\r\n)";

std::string_view req_str4 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Content-Encoding: gzip\r\n"
    "\r\n)";

std::string_view req_str5 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Content-Encoding: deflate\r\n"
    "\r\n)";

std::string_view req_str6 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Content-Encoding: br\r\n"
    "\r\n)";

std::string_view req_str7 =
    "R(GET /ws "
    "HTTP/1.1\r\n"
    "Connection: upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Content-Encoding: cinatra\r\n"
    "\r\n)";

TEST_CASE("http_request test") {
  http_parser parser{};
  int ret = parser.parse_request(req_str.data(), req_str.size(), 0);
  CHECK(ret);
  coro_http_request req(parser, nullptr);
  CHECK(parser.msg().empty());

  CHECK(req.get_accept_encoding().empty());
  CHECK(req.get_content_type() == content_type::octet_stream);
  CHECK(req.get_boundary().empty());

  req.set_aspect_data(std::string("test"));
  CHECK(req.get_aspect_data().size() == 1);
  req.set_aspect_data(std::vector<std::string>{"test", "aspect"});
  CHECK(req.get_aspect_data().size() == 2);
  CHECK(!req.is_support_compressed());
  CHECK(!req.is_upgrade());

  parser = {};
  parser.parse_request(req_str2.data(), req_str2.size(), 0);
  CHECK(!req.is_upgrade());

  parser = {};
  parser.parse_request(req_str3.data(), req_str3.size(), 0);
  CHECK(req.is_upgrade());
  CHECK(req.is_support_compressed());
  CHECK(req.get_encoding_type() == content_encoding::none);

  parser = {};
  parser.parse_request(req_str4.data(), req_str4.size(), 0);
  CHECK(req.is_upgrade());
  CHECK(req.get_encoding_type() == content_encoding::gzip);

  parser = {};
  parser.parse_request(req_str5.data(), req_str5.size(), 0);
  CHECK(req.is_upgrade());
  CHECK(req.get_encoding_type() == content_encoding::deflate);

  parser = {};
  parser.parse_request(req_str6.data(), req_str6.size(), 0);
  CHECK(req.is_upgrade());
  CHECK(req.get_encoding_type() == content_encoding::br);

  parser = {};
  parser.parse_request(req_str7.data(), req_str7.size(), 0);
  CHECK(req.is_upgrade());
  CHECK(req.get_encoding_type() == content_encoding::none);
}

TEST_CASE("query and form fields are bounded") {
  auto make_fields = [](size_t count, bool unique) {
    std::string fields;
    for (size_t i = 0; i < count; ++i) {
      if (!fields.empty()) {
        fields.push_back('&');
      }
      if (unique) {
        fields.append("key").append(std::to_string(i));
      }
      else {
        fields.push_back('a');
      }
    }
    return fields;
  };

  http_parser parser{};
  parser.parse_query(make_fields(CINATRA_MAX_QUERY_FIELD_COUNT + 1, true));
  CHECK(parser.parameter_limit_exceeded());
  CHECK(parser.queries().size() == CINATRA_MAX_QUERY_FIELD_COUNT);

  auto over_limit = make_fields(CINATRA_MAX_QUERY_FIELD_COUNT + 1, false);
  std::string request =
      "GET /?" + over_limit + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
  http_parser request_parser{};
  CHECK(request_parser.parse_request(request.data(), request.size(), 0) < 0);
  CHECK(request_parser.parameter_limit_exceeded());

  std::string form_request =
      "POST /?url=value HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 0\r\n\r\n";
  http_parser form_parser{};
  REQUIRE(form_parser.parse_request(form_request.data(), form_request.size(),
                                    0) > 0);
  coro_http_request form(form_parser, nullptr);
  auto fields = make_fields(CINATRA_MAX_QUERY_FIELD_COUNT, false);
  form.set_body(fields);
  CHECK(form_parser.parameter_limit_exceeded());
}

TEST_CASE("cookie fields are bounded") {
  std::string cookies;
  for (size_t i = 0; i < CINATRA_MAX_COOKIE_COUNT; ++i) {
    if (!cookies.empty()) {
      cookies.append("; ");
    }
    cookies.append("cookie").append(std::to_string(i)).append("=value");
  }

  bool limit_exceeded = true;
  auto at_limit = get_cookies_map(cookies, &limit_exceeded);
  CHECK_FALSE(limit_exceeded);
  CHECK(at_limit.size() == CINATRA_MAX_COOKIE_COUNT);

  cookies.append("; overflow=value");
  CHECK(get_cookies_map(cookies, &limit_exceeded).empty());
  CHECK(limit_exceeded);

  std::string raw_request = "GET / HTTP/1.1\r\nCookie: " + cookies + "\r\n\r\n";
  http_parser parser;
  REQUIRE(parser.parse_request(raw_request.data(), raw_request.size(), 0) > 0);
  coro_http_request request(parser, nullptr);
  CHECK(request.get_session() == nullptr);

  auto ordinary = get_cookies_map("first=one; second=two");
  CHECK(ordinary.size() == 2);
  CHECK(ordinary.at("first") == "one");
  CHECK(ordinary.at("second") == "two");
}

TEST_CASE("client supplied session ids cannot create sessions") {
  auto &manager = session_manager::get();
  const std::string client_session_id = "client-chosen-session-id";
  REQUIRE_FALSE(manager.check_session_existence(client_session_id));

  std::string raw_request = "GET / HTTP/1.1\r\nCookie: " + CSESSIONID + "=" +
                            client_session_id + "\r\n\r\n";
  http_parser parser;
  REQUIRE(parser.parse_request(raw_request.data(), raw_request.size(), 0) > 0);
  coro_http_request request(parser, nullptr);

  CHECK(manager.get_session(client_session_id) == nullptr);
  CHECK(request.get_session(false) == nullptr);
  CHECK_FALSE(manager.check_session_existence(client_session_id));

  auto created_session = request.get_session();
  REQUIRE(created_session != nullptr);
  std::string generated_session_id = created_session->get_session_id();
  CHECK(generated_session_id != client_session_id);
  CHECK(generated_session_id.size() == 32);
  CHECK(generated_session_id.find_first_not_of("0123456789abcdef") ==
        std::string::npos);
  CHECK_FALSE(manager.check_session_existence(client_session_id));
  CHECK(manager.find_session(generated_session_id) == created_session);
  CHECK(manager.get_session(generated_session_id) == created_session);
  CHECK(request.get_session() == created_session);

  std::string existing_request = "GET / HTTP/1.1\r\nCookie: " + CSESSIONID +
                                 "=" + generated_session_id + "\r\n\r\n";
  http_parser existing_parser;
  REQUIRE(existing_parser.parse_request(existing_request.data(),
                                        existing_request.size(), 0) > 0);
  coro_http_request existing(existing_parser, nullptr);
  CHECK(existing.get_session(false) == created_session);

  std::string oversized_session_id(CINATRA_MAX_SESSION_ID_SIZE + 1, 'a');
  std::string oversized_request = "GET / HTTP/1.1\r\nCookie: " + CSESSIONID +
                                  "=" + oversized_session_id + "\r\n\r\n";
  http_parser oversized_parser;
  REQUIRE(oversized_parser.parse_request(oversized_request.data(),
                                         oversized_request.size(), 0) > 0);
  coro_http_request oversized(oversized_parser, nullptr);
  CHECK(oversized.get_session(false) == nullptr);
  CHECK_FALSE(manager.check_session_existence(oversized_session_id));

  auto replacement_session = oversized.get_session();
  REQUIRE(replacement_session != nullptr);
  CHECK(replacement_session->get_session_id() != oversized_session_id);

  created_session->invalidate();
  replacement_session->invalidate();
  manager.remove_expire_session();
  CHECK_FALSE(manager.check_session_existence(generated_session_id));
}

TEST_CASE("siphash matches the reference vectors") {
  constexpr uint64_t key0 = UINT64_C(0x0706050403020100);
  constexpr uint64_t key1 = UINT64_C(0x0f0e0d0c0b0a0908);
  constexpr std::array<uint64_t, 64> expected = {
      UINT64_C(0x726fdb47dd0e0e31), UINT64_C(0x74f839c593dc67fd),
      UINT64_C(0x0d6c8009d9a94f5a), UINT64_C(0x85676696d7fb7e2d),
      UINT64_C(0xcf2794e0277187b7), UINT64_C(0x18765564cd99a68d),
      UINT64_C(0xcbc9466e58fee3ce), UINT64_C(0xab0200f58b01d137),
      UINT64_C(0x93f5f5799a932462), UINT64_C(0x9e0082df0ba9e4b0),
      UINT64_C(0x7a5dbbc594ddb9f3), UINT64_C(0xf4b32f46226bada7),
      UINT64_C(0x751e8fbc860ee5fb), UINT64_C(0x14ea5627c0843d90),
      UINT64_C(0xf723ca908e7af2ee), UINT64_C(0xa129ca6149be45e5),
      UINT64_C(0x3f2acc7f57c29bdb), UINT64_C(0x699ae9f52cbe4794),
      UINT64_C(0x4bc1b3f0968dd39c), UINT64_C(0xbb6dc91da77961bd),
      UINT64_C(0xbed65cf21aa2ee98), UINT64_C(0xd0f2cbb02e3b67c7),
      UINT64_C(0x93536795e3a33e88), UINT64_C(0xa80c038ccd5ccec8),
      UINT64_C(0xb8ad50c6f649af94), UINT64_C(0xbce192de8a85b8ea),
      UINT64_C(0x17d835b85bbb15f3), UINT64_C(0x2f2e6163076bcfad),
      UINT64_C(0xde4daaaca71dc9a5), UINT64_C(0xa6a2506687956571),
      UINT64_C(0xad87a3535c49ef28), UINT64_C(0x32d892fad841c342),
      UINT64_C(0x7127512f72f27cce), UINT64_C(0xa7f32346f95978e3),
      UINT64_C(0x12e0b01abb051238), UINT64_C(0x15e034d40fa197ae),
      UINT64_C(0x314dffbe0815a3b4), UINT64_C(0x027990f029623981),
      UINT64_C(0xcadcd4e59ef40c4d), UINT64_C(0x9abfd8766a33735c),
      UINT64_C(0x0e3ea96b5304a7d0), UINT64_C(0xad0c42d6fc585992),
      UINT64_C(0x187306c89bc215a9), UINT64_C(0xd4a60abcf3792b95),
      UINT64_C(0xf935451de4f21df2), UINT64_C(0xa9538f0419755787),
      UINT64_C(0xdb9acddff56ca510), UINT64_C(0xd06c98cd5c0975eb),
      UINT64_C(0xe612a3cb9ecba951), UINT64_C(0xc766e62cfcadaf96),
      UINT64_C(0xee64435a9752fe72), UINT64_C(0xa192d576b245165a),
      UINT64_C(0x0a8787bf8ecb74b2), UINT64_C(0x81b3e73d20b49b6f),
      UINT64_C(0x7fa8220ba3b2ecea), UINT64_C(0x245731c13ca42499),
      UINT64_C(0xb78dbfaf3a8d83bd), UINT64_C(0xea1ad565322a1a0b),
      UINT64_C(0x60e61c23a3795013), UINT64_C(0x6606d7e446282b93),
      UINT64_C(0x6ca4ecb15c5f91e1), UINT64_C(0x9f626da15c9625f3),
      UINT64_C(0xe51b38608ef25f57), UINT64_C(0x958a324ceb064572)};

  std::string input;
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(cinatra::detail::sip_hash_2_4(input, key0, key1) == expected[i]);
    input.push_back(static_cast<char>(i));
  }
}

TEST_CASE("uri test") {
  std::string uri = "https://example.com?name=tom";
  uri_t u;
  bool r = u.parse_from(uri.data());
  CHECK(r);
  CHECK(u.get_port() == "443");
  context c{u, http_method::GET};
  context c1{u, http_method::GET, "test"};
  CHECK(u.get_query() == "name=tom");

  uri = "https://example.com:521?name=tom";
  r = u.parse_from(uri.data());
  CHECK(r);
  CHECK(u.get_port() == "521");

  uri = "#https://example.com?name=tom";
  r = u.parse_from(uri.data());
  CHECK(!r);

  uri = "https##://example.com?name=tom";
  r = u.parse_from(uri.data());
  CHECK(!r);

  uri = "https://^example.com?name=tom";
  r = u.parse_from(uri.data());
  CHECK(!r);

  uri = "https://example.com?^name=tom";
  r = u.parse_from(uri.data());
  CHECK(!r);

  uri = "http://username:password@example.com";
  r = u.parse_from(uri.data());
  CHECK(r);
  CHECK(u.uinfo == "username:password");

  uri = "http://example.com/data.csv#row=4";
  r = u.parse_from(uri.data());
  CHECK(r);
  CHECK(u.fragment == "row=4");

  uri = "https://example.com?name=tom$";
  r = u.parse_from(uri.data());
  CHECK(r);

  uri = "https://example.com?name=tom!";
  r = u.parse_from(uri.data());
  CHECK(r);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP
