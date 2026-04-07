#pragma once

#include <async_simple/Executor.h>
#include <async_simple/coro/Collect.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/Sleep.h>
#include <async_simple/coro/SyncAwait.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

#include "ylt/coro_io/coro_io.hpp"

namespace cinatra {
class rate_limiter {
 public:
  // rate: tokens per second, burst: maximum capacity of the bucket
  rate_limiter(double rate, int burst)
      : rate_(rate),
        burst_(burst),
        tokens_per_ns_(rate / 1e9),
        ns_per_token_(1e9 / rate) {
    // Initialization: assume the bucket is full at creation time
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_time_ns_.store(now - static_cast<long long>(burst * ns_per_token_));
  }

  bool allow() {
    // Check if one token can be obtained immediately
    return allow_n_tokens(std::chrono::steady_clock::now(), 1);
  }

  bool allow_n_tokens(int n) {
    return allow_n_tokens(std::chrono::steady_clock::now(), n);
  }

  // Wait blocks the current thread until a token is obtained
  void wait() {
    auto delay = reserve(std::chrono::steady_clock::now(), 1);
    if (delay > std::chrono::nanoseconds(0)) {
      std::this_thread::sleep_for(delay);
    }
  }

  void wait_n_tokens(int n) {
    auto delay = reserve(std::chrono::steady_clock::now(), n);
    if (delay > std::chrono::nanoseconds(0)) {
      std::this_thread::sleep_for(delay);
    }
  }

  async_simple::coro::Lazy<void> wait_async() {
    auto delay = reserve(std::chrono::steady_clock::now(), 1);
    if (delay > std::chrono::nanoseconds(0)) {
      co_await coro_io::sleep_for(delay);
    }
  }

  async_simple::coro::Lazy<void> wait_async_n(int n) {
    auto delay = reserve(std::chrono::steady_clock::now(), n);
    if (delay > std::chrono::nanoseconds(0)) {
      co_await coro_io::sleep_for(delay);
    }
  }

 private:
  // calculate and update state (lock-free CAS loop)
  std::chrono::nanoseconds reserve(std::chrono::steady_clock::time_point now,
                                   int n) {
    long long now_ns = now.time_since_epoch().count();
    long long old_last_ns, new_last_ns;

    do {
      old_last_ns = last_time_ns_.load();
      // 1. Calculate how many tokens are actually in the bucket at current time
      // (considering burst limit)
      long long min_last_ns =
          now_ns - static_cast<long long>(burst_ * ns_per_token_);
      long long start_ns = std::max(old_last_ns, min_last_ns);

      // 2. Add the time required to acquire n tokens
      new_last_ns = start_ns + static_cast<long long>(n * ns_per_token_);

      // 3. CAS update
    } while (!last_time_ns_.compare_exchange_weak(old_last_ns, new_last_ns));

    return std::chrono::nanoseconds(std::max(0LL, new_last_ns - now_ns));
  }

  bool allow_n_tokens(std::chrono::steady_clock::time_point now, int n) {
    long long now_ns = now.time_since_epoch().count();
    long long old_last_ns = last_time_ns_.load();

    long long min_last_ns =
        now_ns - static_cast<long long>(burst_ * ns_per_token_);
    long long start_ns = std::max(old_last_ns, min_last_ns);
    long long new_last_ns =
        start_ns + static_cast<long long>(n * ns_per_token_);

    // If the expected available time after getting tokens is in the future,
    // insufficient tokens
    if (new_last_ns > now_ns) {
      return false;
    }

    return last_time_ns_.compare_exchange_strong(old_last_ns, new_last_ns);
  }

  double rate_;
  int burst_;
  double tokens_per_ns_;
  double ns_per_token_;
  std::atomic<long long>
      last_time_ns_;  // Predicted time point when tokens were last exhausted
};
}  // namespace cinatra