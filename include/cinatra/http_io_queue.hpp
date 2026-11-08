#pragma once

#include <array>
#include <cassert>
#include <deque>
#include <memory>
#include <optional>

#include "ylt/coro_io/coro_io.hpp"

namespace cinatra::detail {

// Each queue belongs to one io_context run by one thread. Shared ownership
// keeps it alive while connections or posted drain operations still reference
// it.
class http_io_queue : public std::enable_shared_from_this<http_io_queue> {
 public:
  static constexpr size_t max_capacity = 256;
  static constexpr size_t batch_size = 16;
  using handler_type = coro_io::callback_awaitor<
      std::pair<std::error_code, size_t>>::awaitor_handler;

  http_io_queue(asio::io_context::executor_type executor, size_t capacity)
      : executor_(executor), capacity_(capacity) {
    assert(capacity > 0 && capacity <= max_capacity);
  }

  void enqueue(handler_type handler) {
    if (size_ < capacity_) {
      push_local(handler);
    }
    else {
      overflow_.push_back(handler);
    }
    if (!scheduled_) {
      scheduled_ = true;
      post_drain();
    }
  }

  bool empty() const { return size_ == 0 && overflow_.empty(); }

 private:
  void push_local(handler_type handler) {
    slots_[(head_ + size_) % max_capacity].emplace(handler);
    ++size_;
  }

  std::optional<handler_type> pop(bool admit_old) {
    if (admit_old && !overflow_.empty()) {
      auto handler = overflow_.front();
      overflow_.pop_front();
      return handler;
    }
    if (size_ == 0) {
      while (size_ < capacity_ && !overflow_.empty()) {
        push_local(overflow_.front());
        overflow_.pop_front();
      }
    }
    if (size_ == 0) {
      return {};
    }
    auto handler = slots_[head_];
    slots_[head_].reset();
    head_ = (head_ + 1) % max_capacity;
    --size_;
    return handler;
  }

  void post_drain() {
    asio::post(executor_, [self = shared_from_this()] {
      self->drain();
    });
  }

  void finish_turn() {
    if (empty()) {
      scheduled_ = false;
    }
    else {
      post_drain();
    }
  }

  void drain() {
    try {
      for (size_t i = 0; i < batch_size; ++i) {
        // Reserve an execution slot for old work even if hot tasks immediately
        // refill every local slot when they resume.
        auto handler = pop(i == 0);
        if (!handler) {
          break;
        }
        handler->resume();
      }
    } catch (...) {
      finish_turn();
      throw;
    }
    finish_turn();
  }

  asio::io_context::executor_type executor_;
  std::array<std::optional<handler_type>, max_capacity> slots_;
  std::deque<handler_type> overflow_;
  size_t capacity_;
  size_t head_ = 0;
  size_t size_ = 0;
  bool scheduled_ = false;
};

}  // namespace cinatra::detail
