#pragma once
#include <filesystem>
#include <string_view>
#include <unordered_map>
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

enum class req_content_type { html, json, string, multipart, ranges, none };

constexpr inline auto HTML = req_content_type::html;
constexpr inline auto JSON = req_content_type::json;
constexpr inline auto TEXT = req_content_type::string;
constexpr inline auto RANGES = req_content_type::ranges;
constexpr inline auto NONE = req_content_type::none;

inline const std::string_view STATIC_RESOURCE = "cinatra_static_resource";
inline const std::string CSESSIONID = "CSESSIONID";

const static inline std::string CRCF = "\r\n";
const static inline std::string TWO_CRCF = "\r\n\r\n";
const static inline std::string BOUNDARY = "--CinatraBoundary2B8FAF4A80EDB307";
const static inline std::string MULTIPART_END =
    CRCF + "--" + BOUNDARY + "--" + TWO_CRCF;

inline std::unordered_map<std::string, std::string> g_content_type_map = {
    {".css", "text/css"},
    {".csv", "text/csv"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".js", "text/javascript"},
    {".mjs", "text/javascript"},
    {".txt", "text/plain"},
    {".vtt", "text/vtt"},
    {".apng", "image/apng"},
    {".avif", "image/avif"},
    {".bmp", "image/bmp"},
    {".gif", "image/gif"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".webp", "image/webp"},
    {".ico", "image/x-icon"},
    {".tif", "image/tiff"},
    {".tiff", "image/tiff"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".mp4", "video/mp4"},
    {".mpeg", "video/mpeg"},
    {".webm", "video/webm"},
    {".mp3", "audio/mp3"},
    {".mpga", "audio/mpeg"},
    {".weba", "audio/webm"},
    {".wav", "audio/wave"},
    {".otf", "font/otf"},
    {".ttf", "font/ttf"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".7z", "application/x-7z-compressed"},
    {".atom", "application/atom+xml"},
    {".pdf", "application/pdf"},
    {".json", "application/json"},
    {".rss", "application/rss+xml"},
    {".tar", "application/x-tar"},
    {"xht", "application/xhtml+xml"},
    {"xhtml", "application/xhtml+xml"},
    {"xslt", "application/xslt+xml"},
    {"xml", "application/xml"},
    {"gz", "application/gzip"},
    {"zip", "application/zip"},
    {"wasm", "application/wasm"}};
struct NonSSL {};
struct SSL {};

/*
  IMF-fixdate = day-name "," SP date1 SP time-of-day SP GMT
  day-name     = %s"Mon" / %s"Tue" / %s"Wed"
               / %s"Thu" / %s"Fri" / %s"Sat" / %s"Sun"

  date1        = day SP month SP year
               ; e.g., 02 Jun 1982

  day          = 2DIGIT
  month        = %s"Jan" / %s"Feb" / %s"Mar" / %s"Apr"
               / %s"May" / %s"Jun" / %s"Jul" / %s"Aug"
               / %s"Sep" / %s"Oct" / %s"Nov" / %s"Dec"
  year         = 4DIGIT

  GMT          = %s"GMT"

  time-of-day  = hour ":" minute ":" second
               ; 00:00:00 - 23:59:60 (leap second)

  hour         = 2DIGIT
  minute       = 2DIGIT
  second       = 2DIGIT
*/
enum component_of_time_format {
  day_name,
  day,
  month,
  year,
  GMT,
  hour,
  minute,
  second,
  SP,
  comma,
  colon
};
inline std::vector<component_of_time_format> http_time_format{
    component_of_time_format::day_name, component_of_time_format::comma,
    component_of_time_format::SP,       component_of_time_format::day,
    component_of_time_format::SP,       component_of_time_format::month,
    component_of_time_format::SP,       component_of_time_format::year,
    component_of_time_format::SP,       component_of_time_format::hour,
    component_of_time_format::colon,    component_of_time_format::minute,
    component_of_time_format::colon,    component_of_time_format::second,
    component_of_time_format::SP,       component_of_time_format::GMT};
const inline int len_of_http_format =
    3 + 1 + 1 + 2 + 1 + 3 + 1 + 4 + 1 + 2 + 1 + 2 + 1 + 2 + 1 + 3;
inline std::unordered_map<std::string_view, int> name_of_day = {
    {"Sun", 0}, {"Mon", 1}, {"Tue", 2}, {"Wed", 3},
    {"Thu", 4}, {"Fri", 5}, {"Sat", 6}};
inline std::unordered_map<std::string_view, int> name_of_month = {
    {"Jan", 0}, {"Feb", 1}, {"Mar", 2}, {"Apr", 3}, {"May", 4},  {"Jun", 5},
    {"Jul", 6}, {"Aug", 7}, {"Sep", 8}, {"Oct", 9}, {"Nov", 10}, {"Dec", 11}};
const inline std::int64_t absolute_zero_year = -292277022399;
const inline std::int64_t days_per_100_years = 365 * 100 + 24;
const inline std::int64_t days_per_400_years = 365 * 400 + 97;
const inline std::int64_t days_per_4_years = 365 * 4 + 1;
const inline std::int64_t seconds_per_minute = 60;
const inline std::int64_t seconds_per_hour = 60 * seconds_per_minute;
const inline std::int64_t seconds_per_day = 24 * seconds_per_hour;
const inline std::int64_t seconds_per_week = 7 * seconds_per_day;
const inline std::int64_t internal_year = 1;
const inline std::int64_t absolute_to_internal =
    (absolute_zero_year - internal_year) *
    std::int64_t(365.2425 * seconds_per_day);
const inline std::int64_t unix_to_internal =
    (1969 * 365 + 1969 / 4 - 1969 / 100 + 1969 / 400) * seconds_per_day;
const inline std::int64_t internal_to_unix = -unix_to_internal;
const inline std::vector<std::int32_t> days_before = {
    0,
    31,
    31 + 28,
    31 + 28 + 31,
    31 + 28 + 31 + 30,
    31 + 28 + 31 + 30 + 31,
    31 + 28 + 31 + 30 + 31 + 30,
    31 + 28 + 31 + 30 + 31 + 30 + 31,
    31 + 28 + 31 + 30 + 31 + 30 + 31 + 31,
    31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30,
    31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31,
    31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30,
    31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30 + 31,
};
}  // namespace cinatra
