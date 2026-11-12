#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "cinatra/coro_http_response.hpp"
#include "doctest/doctest.h"

namespace {
// This executable isolates allocation injection from the networking tests.
// Arm only around the operation under test; doctest and snapshots run unarmed.
thread_local bool track_allocations = false;
thread_local std::ptrdiff_t fail_after = -1;
thread_local size_t allocation_count = 0;
thread_local bool allocation_failed = false;

struct allocation_scope {
  void arm(std::ptrdiff_t fail) {
    allocation_count = 0;
    allocation_failed = false;
    fail_after = fail;
    track_allocations = true;
  }
  ~allocation_scope() { track_allocations = false; }
};
}  // namespace

void *operator new(std::size_t size) {
  if (track_allocations) {
    ++allocation_count;
    if (fail_after == 0) {
      fail_after = -1;
      allocation_failed = true;
      throw std::bad_alloc();
    }
    if (fail_after > 0) {
      --fail_after;
    }
  }
  if (void *pointer = std::malloc(size == 0 ? 1 : size)) {
    return pointer;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void *pointer, std::size_t) noexcept {
  std::free(pointer);
}

using namespace cinatra;

namespace {
std::string response_wire(coro_http_response &response) {
  std::vector<asio::const_buffer> buffers;
  std::string chunk_size;
  response.to_buffers(buffers, chunk_size);
  std::string wire;
  for (auto buffer : buffers) {
    wire.append(static_cast<const char *>(buffer.data()), buffer.size());
  }
  return wire;
}

void set_previous_response(coro_http_response &response, bool owned) {
  response.need_date_head(false);
  response.add_header("X-Previous", "retained");
  if (owned) {
    response.set_status_and_content(status_type::accepted, "previous body");
  }
  else {
    response.set_status_and_content_view(status_type::accepted,
                                         std::string_view("previous body"));
  }
}
}  // namespace

TEST_CASE("owning a view preserves the response when allocation fails") {
  const std::string input(65536, 'x');
  for (bool owned : {false, true}) {
    for (bool view_api : {false, true}) {
      coro_http_response response(nullptr);
      set_previous_response(response, owned);
      const auto previous = response_wire(response);
      bool threw = false;
      {
        allocation_scope scope;
        scope.arm(0);
        try {
          if (view_api) {
            response.set_status_and_content_view(status_type::ok,
                                                 std::string_view(input),
                                                 content_encoding::none, false);
          }
          else {
            response.set_status_and_content(status_type::ok,
                                            std::string_view(input));
          }
        } catch (const std::bad_alloc &) {
          threw = true;
        }
      }
      CHECK(allocation_failed);
      CHECK(threw);
      CHECK(response.status() == status_type::accepted);
      CHECK(response_wire(response) == previous);
    }
  }
}

#ifdef CINATRA_ENABLE_GZIP
TEST_CASE(
    "encoded responses preserve their state on commit allocation errors") {
  for (auto encoding : {content_encoding::gzip, content_encoding::deflate}) {
    for (bool owned : {false, true}) {
      size_t allocations;
      {
        coro_http_response response(nullptr);
        set_previous_response(response, owned);
        allocation_scope scope;
        response.set_status_and_content(
            status_type::ok,
            [&](auto &stream) {
              stream.append("{}");
              scope.arm(-1);
            },
            encoding);
        allocations = allocation_count;
      }
      REQUIRE(allocations > 0);
      for (size_t failure = 0; failure < allocations; ++failure) {
        CAPTURE(static_cast<int>(encoding));
        CAPTURE(owned);
        CAPTURE(failure);
        coro_http_response response(nullptr);
        set_previous_response(response, owned);
        const auto previous = response_wire(response);
        bool threw = false;
        {
          allocation_scope scope;
          try {
            response.set_status_and_content(
                status_type::ok,
                [&](auto &stream) {
                  stream.append("{}");
                  scope.arm(static_cast<std::ptrdiff_t>(failure));
                },
                encoding);
          } catch (const std::bad_alloc &) {
            threw = true;
          }
        }
        REQUIRE(allocation_failed);
        CHECK(threw);
        CHECK(response.status() == status_type::accepted);
        CHECK(response_wire(response) == previous);
      }
    }
  }
}
#endif
