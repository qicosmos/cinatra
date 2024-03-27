#pragma once

#include <atomic>
#include <chrono>
#include <new>
#include <optional>
#include <thread>

#include "utils.hpp"

namespace cinatra {
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

struct token_bucket_policy_default {
  using align_type =
      std::integral_constant<size_t, hardware_destructive_interference_size>;

  template <typename T>
  using atom_type = std::atomic<T>;

  using clock_type = std::chrono::steady_clock;

  using concurrent_type = std::true_type;
};

template <typename policy = token_bucket_policy_default>
class token_bucket_storage {
  template <typename T>
  using atom = typename policy::template atom_type<T>;
  using align = typename policy::align_type;
  using clock = typename policy::clock_type;
  using concurrent = typename policy::concurrent_type;

 public:
  explicit token_bucket_storage(double zero_time = 0) noexcept
      : zero_time_(zero_time) {}
  token_bucket_storage(const token_bucket_storage& other) noexcept
      : zero_time_(other.zero_time_.load(std::memory_order_relaxed)) {}
  token_bucket_storage& operator=(const token_bucket_storage& other) noexcept {
    zero_time_.store(other.zero_time(), std::memory_order_relaxed);
    return *this;
  }
  void reset(double zero_time = 0) noexcept {
    zero_time_.store(zero_time, std::memory_order_relaxed);
  }
  double balance(double rate, double burst_size,
                 double now_in_seconds) const noexcept {
    double zt = this->zero_time_.load(std::memory_order_relaxed);
    return std::min((now_in_seconds - zt) * rate, burst_size);
  }
  template <typename Callback>
  double consume(double rate, double burst_size, double now_in_seconds,
                 const Callback& callback) {
    double zero_time_old;
    double zero_time_new;
    double consumed;
    do {
      zero_time_old = zero_time();
      double tokens =
          std::min((now_in_seconds - zero_time_old) * rate, burst_size);
      consumed = callback(tokens);
      double tokens_new = tokens - consumed;
      if (consumed == 0.0) {
        return consumed;
      }

      zero_time_new = now_in_seconds - tokens_new / rate;
    } while (!compare_exchange_weak_relaxed(zero_time_, zero_time_old,
                                            zero_time_new));

    return consumed;
  }

  double time_when_bucket(double rate, double target) {
    return zero_time() + target / rate;
  }

  void return_tokens(double tokens_to_return, double rate) {
    return_tokens_impl(tokens_to_return, rate);
  }

 private:
  double return_tokens_impl(double token_count, double rate) {
    auto zero_time_old = zero_time_.load(std::memory_order_relaxed);

    double zero_time_new;
    do {
      zero_time_new = zero_time_old - token_count / rate;

    } while (!compare_exchange_weak_relaxed(zero_time_, zero_time_old,
                                            zero_time_new));
    return zero_time_new;
  }

  static bool compare_exchange_weak_relaxed(atom<double>& atom,
                                            double& expected,
                                            double zero_time) {
    if (concurrent::value) {
      return atom.compare_exchange_weak(expected, zero_time,
                                        std::memory_order_relaxed);
    }
    else {
      return atom.store(zero_time, std::memory_order_relaxed), true;
    }
  }

  double zero_time() const {
    return this->zero_time_.load(std::memory_order_relaxed);
  }

  static constexpr size_t align_zero_time =
      constexpr_max(align::value, alignof(atom<double>));
  alignas(align_zero_time) atom<double> zero_time_;
};

template <typename policy = token_bucket_policy_default>
class basic_token_bucket {
 private:
  using impl = basic_dynamic_token_bucket<policy>;

 public:
  basic_token_bucket(double gen_rate, double burst_size,
                     double zero_time = 0) noexcept
      : token_bucket_(zero_time), rate_(gen_rate), burst_size_(burst_size) {}

  basic_token_bucket(const basic_token_bucket& other) noexcept = default;

  basic_token_bucket& operator=(const basic_token_bucket& other) noexcept =
      default;

  static double default_clock_now() noexcept(
      noexcept(impl::default_clock_now())) {
    return impl::default_clock_now();
  }

  void reset(double gen_rate, double burst_size,
             double now_in_seconds = default_clock_now()) noexcept {
    const double avail_tokens = available(now_in_seconds);
    rate_ = gen_rate;
    burst_size_ = burst_size;
    set_capacity(avail_tokens, now_in_seconds);
  }

  void set_capacity(double tokens, double now_in_seconds) noexcept {
    token_bucket_.reset(now_in_seconds - tokens / rate_);
  }

  bool consume(double to_consume, double now_in_seconds = default_clock_now()) {
    return token_bucket_.consume(to_consume, rate_, burst_size_,
                                 now_in_seconds);
  }

  double consume_or_drain(double to_consume,
                          double now_in_seconds = default_clock_now()) {
    return token_bucket_.consume_or_drain(to_consume, rate_, burst_size_,
                                          now_in_seconds);
  }

  void return_tokens(double tokens_to_return) {
    return token_bucket_.return_tokens(tokens_to_return, rate_);
  }

  std::optional<double> consume_with_borrow_nonblocking(
      double to_consume, double now_in_seconds = default_clock_now()) {
    return token_bucket_.consume_with_borrow_nonblocking(
        to_consume, rate_, burst_size_, now_in_seconds);
  }

  bool consume_with_borrow_and_wait(
      double to_consume, double now_in_seconds = default_clock_now()) {
    return token_bucket_.consume_with_borrow_and_wait(
        to_consume, rate_, burst_size_, now_in_seconds);
  }

  double available(double now_in_seconds = default_clock_now()) const noexcept {
    return std::max(0.0, balance(now_in_seconds));
  }

  double balance(double now_in_seconds = default_clock_now()) const noexcept {
    return token_bucket_.balance(rate_, burst_size_, now_in_seconds);
  }

  double rate() const noexcept { return rate_; }

  double burst() const noexcept { return burst_size_; }

 private:
  impl token_bucket_;
  double rate_;
  double burst_size_;
};

using token_bucket = basic_token_bucket<>;
}  // namespace cinatra