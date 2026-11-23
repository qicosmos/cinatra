#pragma once

#include <libdeflate.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace cinatra::gzip_codec::detail {

// Whole-buffer compression is synchronous. No input/output or compressor
// pointer escapes this function, and every thread owns its compressor.
inline bool compress_small(std::string_view input, std::string &output,
                           int level) {
  if (level < -1 || level > 9) {
    return false;
  }
  struct compressor_slot {
    std::unique_ptr<libdeflate_compressor,
                    decltype(&libdeflate_free_compressor)>
        value{nullptr, libdeflate_free_compressor};
    int level = -2;
  };
  thread_local compressor_slot cached;
  const int requested = level == -1 ? 6 : level;
  if (!cached.value || cached.level != requested) {
    decltype(cached.value) replacement(libdeflate_alloc_compressor(requested),
                                       libdeflate_free_compressor);
    if (!replacement) {
      return false;
    }
    cached.value = std::move(replacement);
    cached.level = requested;
  }
  std::less<const char *> less;
  if (!input.empty() && less(input.data(), output.data() + output.size() + 1) &&
      less(output.data(), input.data() + input.size())) {
    std::string owned(input);
    return compress_small(owned, output, level);
  }
  const auto prefix = output.size();
  const auto bound =
      libdeflate_gzip_compress_bound(cached.value.get(), input.size());
  if (bound > output.max_size() - prefix) {
    return false;
  }
  output.resize(prefix + bound);
  const auto bytes = libdeflate_gzip_compress(
      cached.value.get(), input.empty() ? "" : input.data(), input.size(),
      output.data() + prefix, bound);
  output.resize(prefix + bytes);
  return bytes != 0;
}

}  // namespace cinatra::gzip_codec::detail
