#pragma once
#include <filesystem>
namespace fs = std::filesystem;

namespace cinatra {
enum class content_type {
  string,
  multipart,
  urlencoded,
  chunked,
  octet_stream,
  websocket,
  unknown,
};

enum class req_content_type { html, json, string, multipart, none };

constexpr inline auto HTML = req_content_type::html;
constexpr inline auto JSON = req_content_type::json;
constexpr inline auto TEXT = req_content_type::string;
constexpr inline auto NONE = req_content_type::none;

inline const std::string_view STATIC_RESOURCE = "cinatra_static_resource";
inline const std::string CSESSIONID = "CSESSIONID";

const static inline std::string CRCF = "\r\n";
const static inline std::string TWO_CRCF = "\r\n\r\n";
const static inline std::string BOUNDARY = "--CinatraBoundary2B8FAF4A80EDB307";
const static inline std::string MULTIPART_END =
    CRCF + "--" + BOUNDARY + "--" + TWO_CRCF;

struct NonSSL {};
struct SSL {};
} // namespace cinatra
