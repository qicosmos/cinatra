#pragma once

#include <algorithm>
#include <asio/buffers_iterator.hpp>
#include <asio/streambuf.hpp>
#include <cstring>
#include <type_traits>
#include <utility>

namespace cinatra::detail {

struct http_header_matcher {
  // Asio recognises function objects with a result_type as match conditions.
  using result_type = void;

  template <typename Buffer>
    requires std::is_convertible_v<Buffer, asio::const_buffer>
  std::pair<asio::buffers_iterator<Buffer>, bool> operator()(
      asio::buffers_iterator<Buffer> begin,
      asio::buffers_iterator<Buffer> end) const noexcept {
    if (begin == end) {
      return {end, false};
    }

    // streambuf and string dynamic buffers expose one contiguous buffer.
    // Do not accept iterators over scatter/gather buffer sequences here.
    const char *first = &*begin;
    const char *last = first + (end - begin);
    const char *next = first;
    while (next != last) {
      const char *candidate = static_cast<const char *>(
          std::memchr(next, '\r', static_cast<size_t>(last - next)));
      if (candidate == nullptr) {
        break;
      }

      auto remaining = static_cast<size_t>(last - candidate);
      if (remaining >= 4) {
        if (std::memcmp(candidate, "\r\n\r\n", 4) == 0) {
          return {begin + (candidate - first) + 4, true};
        }
      }
      else if (std::memcmp(candidate, "\r\n\r\n", remaining) == 0) {
        // Resume at the partial delimiter when the next read appends data.
        return {begin + (candidate - first), false};
      }
      next = candidate + 1;
    }
    return {end, false};
  }
};

struct bounded_http_header_matcher {
  using result_type = void;
  const asio::streambuf *source;
  size_t maximum;
  bool *exceeded;
  template <typename Buffer>
    requires std::is_convertible_v<Buffer, asio::const_buffer>
  std::pair<asio::buffers_iterator<Buffer>, bool> operator()(
      asio::buffers_iterator<Buffer> begin,
      asio::buffers_iterator<Buffer> end) const noexcept {
    const auto buffered = source->size();
    if (buffered < maximum) [[likely]] {
      return http_header_matcher{}(begin, end);
    }

    // Asio may resume partway through the buffer. Truncate the search
    // interval by the bytes already buffered past the header limit.
    const auto available = static_cast<size_t>(end - begin);
    const auto excess = buffered - maximum;
    const auto checked = available > excess ? available - excess : 0;
    const auto limit = begin + checked;
    auto match = http_header_matcher{}(begin, limit);
    if (match.second || buffered == 0) {
      return match;
    }
    *exceeded = true;
    return {limit, true};
  }
};

}  // namespace cinatra::detail
