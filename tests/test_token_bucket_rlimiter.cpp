#define DOCTEST_CONFIG_IMPLEMENT
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <chrono>

#include "cinatra/rate_limiter.hpp"
#include "doctest/doctest.h"

using namespace cinatra;
using namespace std::chrono_literals;

TEST_CASE("token bucket limiter basic") {
  // Test: allow 1 request per second, bucket capacity = 1
  cinatra::rate_limiter limiter(1.0, 1);

  CHECK(limiter.allow());        // First request should be allowed
  CHECK_FALSE(limiter.allow());  // Immediate second request should be rejected

  std::this_thread::sleep_for(1100ms);  // Wait 1.1 seconds
  CHECK(limiter.allow());               // Should be allowed again

  auto start = std::chrono::steady_clock::now();
  limiter.wait();  // First passes immediately
  limiter.wait();  // Second needs to wait about 1 second
  auto duration = std::chrono::steady_clock::now() - start;

  CHECK(duration >= 900ms);   // Should wait at least 0.9 seconds
  CHECK(duration <= 2100ms);  // Allow some tolerance for system scheduling
}

async_simple::coro::Lazy<void> test_async_limiter() {
  cinatra::rate_limiter limiter(1.0, 1);

  auto start = std::chrono::steady_clock::now();
  co_await limiter.wait_async();  // First passes immediately
  co_await limiter.wait_async();  // Second needs to wait about 1 second
  auto duration = std::chrono::steady_clock::now() - start;

  CHECK(duration >= 900ms);
  CHECK(duration <= 1100ms);  // Allow some tolerance for system scheduling
}

TEST_CASE("token bucket limiter async") {
  async_simple::coro::syncAwait(test_async_limiter());
}

TEST_CASE("allow_n_tokens") {
  // Test allow_n_tokens
  // rate=2, burst=3: generates 2 tokens per second, max bucket capacity = 3
  cinatra::rate_limiter limiter(2.0, 3);

  CHECK(limiter.allow_n_tokens(1));        // Bucket full, can take 1
  CHECK(limiter.allow_n_tokens(2));        // 2 remaining, can take 2
  CHECK_FALSE(limiter.allow_n_tokens(1));  // Bucket empty, rejected

  std::this_thread::sleep_for(
      600ms);  // Wait 0.6 seconds, generates about 1.2 tokens
  CHECK(limiter.allow_n_tokens(1));  // Can take 1
}

TEST_CASE("wait_n_tokens") {
  // Test wait_n_tokens
  // rate=2, burst=3
  cinatra::rate_limiter limiter(2.0, 3);

  auto start = std::chrono::steady_clock::now();

  // Take 6 tokens continuously, need to wait (6-3)/2 = 1.5 seconds
  limiter.wait_n_tokens(3);  // Returns immediately, bucket full
  limiter.wait_n_tokens(3);  // Needs to wait 1.5 seconds

  auto duration = std::chrono::steady_clock::now() - start;
  CHECK(duration >= 1400ms);
  CHECK(duration <= 2500ms);
}

async_simple::coro::Lazy<void> test_wait_async_n() {
  cinatra::rate_limiter limiter(2.0, 3);

  auto start = std::chrono::steady_clock::now();

  co_await limiter.wait_async_n(3);  // Passes immediately, bucket full
  co_await limiter.wait_async_n(3);  // Needs to wait 1.5 seconds

  auto duration = std::chrono::steady_clock::now() - start;
  CHECK(duration >= 1400ms);
  CHECK(duration <= 2500ms);
}

TEST_CASE("wait_async_n") {
  async_simple::coro::syncAwait(test_wait_async_n());
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP