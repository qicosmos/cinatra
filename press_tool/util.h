#pragma once
#include <string>

namespace cinatra::press_tool {
constexpr uint64_t ONE_BYTE = 1;
constexpr uint64_t KB_BYTE = ONE_BYTE * 1024;
constexpr uint64_t MB_BYTE = ONE_BYTE * 1024 * 1024;
constexpr uint64_t GB_BYTE = ONE_BYTE * 1024 * 1024 * 1024;

inline std::string bytes_to_string(uint64_t bytes) {
  double rt;
  std::string suffix;
  if (bytes > GB_BYTE) {
    suffix = "GB";
    rt = (double(bytes) / GB_BYTE);
  }
  else if (bytes > MB_BYTE) {
    suffix = "MB";
    rt = (double(bytes) / MB_BYTE);
  }
  else if (bytes > KB_BYTE) {
    suffix = "KB";
    rt = (double(bytes) / KB_BYTE);
  }
  else {
    suffix = "bytes";
    rt = (double)bytes;
  }
  return (std::to_string(rt) + suffix);
}

inline std::vector<std::string> &split(std::string &str,
                                       const std::string &delimiter,
                                       std::vector<std::string> &elems) {
  size_t pos = 0;
  std::string token;
  while ((pos = str.find(delimiter)) != std::string::npos) {
    token = str.substr(0, pos);
    elems.emplace_back(token);
    str.erase(0, pos + delimiter.length());
  }
  if (str.find(delimiter) == std::string::npos && !str.empty()) {
    elems.emplace_back(str);
  }
  return elems;
}
}  // namespace cinatra::press_tool