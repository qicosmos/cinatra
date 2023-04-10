#pragma once
#include <string>

namespace cinatra::press_tool {
constexpr uint64_t ONE_BYTE = 1;
const uint64_t KB_BYTE = ONE_BYTE * 1024;
const uint64_t MB_BYTE = ONE_BYTE * 1024 * 1024;
const uint64_t GB_BYTE = ONE_BYTE * 1024 * 1024 * 1024;

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
}  // namespace cinatra::press_tool