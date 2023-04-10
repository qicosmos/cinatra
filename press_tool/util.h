#pragma once

namespace press_util {
const uint64_t byte = 1;
const uint64_t kb_byte = byte * 1024;
const uint64_t mb_byte = byte * 1024 * 1024;
const uint64_t gb_byte = byte * 1024 * 1024 * 1024;

std::string bytes_to_string(uint64_t bytes) {
  double rt;
  std::string suffix;
  if (bytes > gb_byte) {
    suffix = "GB";
    rt = (double)(bytes / gb_byte);
  }
  else if (bytes > mb_byte) {
    suffix = "MB";
    rt = (double)(bytes / mb_byte);
  }
  else if (bytes > kb_byte) {
    suffix = "KB";
    rt = (double)(bytes / kb_byte);
  }
  else {
    suffix = "bytes";
    rt = (double)bytes;
  }
  return (std::to_string(rt) + suffix);
}
}