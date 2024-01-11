#pragma once
#include <string_view>

#include "define.h"

namespace cinatra {
enum class status_type {
  init,
  http_continue = 100,
  switching_protocols = 101,
  processing = 102,
  ok = 200,
  created = 201,
  accepted = 202,
  nonauthoritative = 203,
  no_content = 204,
  reset_content = 205,
  partial_content = 206,
  multi_status = 207,
  already_reported = 208,
  im_used = 226,
  multiple_choices = 300,
  moved_permanently = 301,
  moved_temporarily = 302,
  not_modified = 304,
  use_proxy = 305,
  temporary_redirect = 307,
  permanent_redirect = 308,
  bad_request = 400,
  unauthorized = 401,
  payment_required = 402,
  forbidden = 403,
  not_found = 404,
  method_not_allowed = 405,
  not_acceptable = 406,
  proxy_authentication_required = 407,
  request_timeout = 408,
  conflict = 409,
  gone = 410,
  length_required = 411,
  precondition_failed = 412,
  request_entity_too_large = 413,
  request_uri_too_long = 414,
  unsupported_media_type = 415,
  range_not_satisfiable = 416,
  expectation_failed = 417,
  im_a_teapot = 418,
  enchance_your_calm = 420,
  misdirected_request = 421,
  unprocessable_entity = 422,
  locked = 423,
  failed_dependency = 424,
  too_early = 425,
  upgrade_required = 426,
  precondition_required = 428,
  too_many_requests = 429,
  request_header_fields_too_large = 431,
  unavailable_for_legal_reasons = 451,
  internal_server_error = 500,
  not_implemented = 501,
  bad_gateway = 502,
  service_unavailable = 503,
  gateway_timeout = 504,
  version_not_supported = 505,
  variant_also_negotiates = 506,
  insufficient_storage = 507,
  loop_detected = 508,
  not_extended = 510,
  network_authentication_required = 511
};

enum class content_encoding { gzip, none };

// http response status string
namespace http_status_string {
inline constexpr std::string_view switching_protocols =
    "HTTP/1.1 101 Switching Protocals\r\n";
inline constexpr std::string_view rep_ok = "HTTP/1.1 200 OK\r\n";
inline constexpr std::string_view rep_created = "HTTP/1.1 201 Created\r\n";
inline constexpr std::string_view rep_accepted = "HTTP/1.1 202 Accepted\r\n";
inline constexpr std::string_view rep_no_content =
    "HTTP/1.1 204 No Content\r\n";
inline constexpr std::string_view rep_partial_content =
    "HTTP/1.1 206 Partial Content\r\n";
inline constexpr std::string_view rep_multiple_choices =
    "HTTP/1.1 300 Multiple Choices\r\n";
inline constexpr std::string_view rep_moved_permanently =
    "HTTP/1.1 301 Moved Permanently\r\n";
inline constexpr std::string_view rep_temporary_redirect =
    "HTTP/1.1 307 Temporary Redirect\r\n";
inline constexpr std::string_view rep_moved_temporarily =
    "HTTP/1.1 302 Moved Temporarily\r\n";
inline constexpr std::string_view rep_not_modified =
    "HTTP/1.1 304 Not Modified\r\n";
inline constexpr std::string_view rep_bad_request =
    "HTTP/1.1 400 Bad Request\r\n";
inline constexpr std::string_view rep_unauthorized =
    "HTTP/1.1 401 Unauthorized\r\n";
inline constexpr std::string_view rep_forbidden = "HTTP/1.1 403 Forbidden\r\n";
inline constexpr std::string_view rep_not_found = "HTTP/1.1 404 Not Found\r\n";
inline constexpr std::string_view rep_method_not_allowed =
    "HTTP/1.1 405 Method Not Allowed\r\n";
inline constexpr std::string_view rep_conflict = "HTTP/1.1 409 Conflict\r\n";
inline constexpr std::string_view rep_range_not_satisfiable =
    "HTTP/1.1 416 Requested Range Not Satisfiable\r\n";
inline constexpr std::string_view rep_internal_server_error =
    "HTTP/1.1 500 Internal Server Error\r\n";
inline constexpr std::string_view rep_not_implemented =
    "HTTP/1.1 501 Not Implemented\r\n";
inline constexpr std::string_view rep_bad_gateway =
    "HTTP/1.1 502 Bad Gateway\r\n";
inline constexpr std::string_view rep_service_unavailable =
    "HTTP/1.1 503 Service Unavailable\r\n";
}  // namespace http_status_string

inline constexpr std::string_view rep_html =
    "Content-Type: text/html; charset=UTF-8\r\n";
inline constexpr std::string_view rep_json =
    "Content-Type: application/json; charset=UTF-8\r\n";
inline constexpr std::string_view rep_string =
    "Content-Type: text/plain; charset=UTF-8\r\n";
inline constexpr std::string_view rep_multipart =
    "Content-Type: multipart/form-data; boundary=";

inline constexpr std::string_view rep_keep = "Connection: keep-alive\r\n";
inline constexpr std::string_view rep_close = "Connection: close     \r\n";
inline constexpr std::string_view rep_len = "Content-Length: ";
inline constexpr std::string_view rep_crcf = "\r\n";
inline constexpr std::string_view rep_server = "Server: cinatra\r\n";

inline const char name_value_separator[] = {':', ' '};
// inline std::string_view crlf = "\r\n";

constexpr std::string_view crlf = "\r\n";
constexpr std::string_view last_chunk = "0\r\n";
inline const std::string http_chunk_header =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n";
/*"Content-Type: video/mp4\r\n"
"\r\n";*/

inline const std::string http_range_chunk_header =
    "HTTP/1.1 206 Partial Content\r\n"
    "Transfer-Encoding: chunked\r\n";
/*"Content-Type: video/mp4\r\n"
"\r\n";*/

inline constexpr std::string_view to_content_type_str(req_content_type type) {
  switch (type) {
    case req_content_type::html:
      return rep_html;
    case req_content_type::json:
      return rep_json;
    case req_content_type::string:
      return rep_string;
    case req_content_type::multipart:
      return rep_multipart;
    default:
      return "";
  }
}

namespace detail {
template <unsigned... digits>
struct to_chars {
  static constexpr std::array<char, sizeof...(digits) + 18> value = {
      'C',
      'o',
      'n',
      't',
      'e',
      'n',
      't',
      '-',
      'L',
      'e',
      'n',
      'g',
      't',
      'h',
      ':',
      ' ',
      ('0' + digits)...,
      '\r',
      '\n'};
};

template <unsigned rem, unsigned... digits>
struct explode : explode<rem / 10, rem % 10, digits...> {};

template <unsigned... digits>
struct explode<0, digits...> : to_chars<digits...> {};
}  // namespace detail

template <unsigned num>
struct num_to_string : detail::explode<num / 10, num % 10> {};

inline constexpr std::string_view to_http_status_string(status_type status) {
  using namespace http_status_string;
  switch (status) {
    case cinatra::status_type::switching_protocols:
      return switching_protocols;
    case cinatra::status_type::ok:
      return rep_ok;
    case cinatra::status_type::created:
      return rep_created;
    case cinatra::status_type::accepted:
      return rep_accepted;
    case cinatra::status_type::no_content:
      return rep_no_content;
    case cinatra::status_type::partial_content:
      return rep_partial_content;
    case cinatra::status_type::multiple_choices:
      return rep_multiple_choices;
    case cinatra::status_type::moved_permanently:
      return rep_moved_permanently;
    case cinatra::status_type::moved_temporarily:
      return rep_moved_temporarily;
    case cinatra::status_type::not_modified:
      return rep_not_modified;
    case cinatra::status_type::temporary_redirect:
      return rep_temporary_redirect;
    case cinatra::status_type::bad_request:
      return rep_bad_request;
    case cinatra::status_type::unauthorized:
      return rep_unauthorized;
    case cinatra::status_type::forbidden:
      return rep_forbidden;
    case cinatra::status_type::not_found:
      return rep_not_found;
    case cinatra::status_type::method_not_allowed:
      return rep_method_not_allowed;
    case cinatra::status_type::conflict:
      return rep_conflict;
    case cinatra::status_type::range_not_satisfiable:
      return rep_range_not_satisfiable;
    case cinatra::status_type::internal_server_error:
      return rep_internal_server_error;
    case cinatra::status_type::not_implemented:
      return rep_not_implemented;
    case cinatra::status_type::bad_gateway:
      return rep_bad_gateway;
    case cinatra::status_type::service_unavailable:
      return rep_service_unavailable;
    default:
      return rep_not_implemented;
  }
}

inline constexpr std::string_view default_status_content(status_type status) {
  std::string_view str = to_http_status_string(status);
  return str.substr(9, str.size() - 11);
}

}  // namespace cinatra
