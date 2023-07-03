/*
 * Copyright (c) 2022, Alibaba Group Holding Limited;
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <async_simple/Executor.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "async_simple/coro/Lazy.h"

namespace coro_io {

template <typename ExecutorImpl = asio::io_context::executor_type>
class ExecutorWrapper : public async_simple::Executor {
 private:
  ExecutorImpl executor_;

 public:
  ExecutorWrapper(ExecutorImpl executor) : executor_(executor) {}

  using context_t = std::remove_cvref_t<decltype(executor_.context())>;

  virtual bool schedule(Func func) override {
    if constexpr (requires(ExecutorImpl e) { e.post(std::move(func)); }) {
      executor_.post(std::move(func));
    }
    else {
      asio::post(executor_, std::move(func));
    }

    return true;
  }

  virtual bool checkin(Func func, void *ctx) override {
    using context_t = std::remove_cvref_t<decltype(executor_.context())>;
    auto &executor = *(context_t *)ctx;
    if constexpr (requires(ExecutorImpl e) { e.post(std::move(func)); }) {
      executor.post(std::move(func));
    }
    else {
      asio::post(executor, std::move(func));
    }
    return true;
  }
  virtual void *checkout() override { return &executor_.context(); }

  context_t &context() { return executor_.context(); }

  auto get_asio_executor() { return executor_; }

  operator ExecutorImpl() { return executor_; }

 private:
  void schedule(Func func, Duration dur) override {
    auto timer = std::make_shared<asio::steady_timer>(executor_, dur);
    timer->async_wait([fn = std::move(func), timer](auto ec) {
      fn();
    });
  }
};

template <typename ExecutorImpl = asio::io_context>
inline async_simple::coro::Lazy<typename ExecutorImpl::executor_type>
get_executor() {
  auto executor = co_await async_simple::CurrentExecutor{};
  assert(executor != nullptr);
  co_return static_cast<ExecutorImpl *>(executor->checkout())->get_executor();
}

class io_context_pool {
 public:
  using executor_type = asio::io_context::executor_type;
  explicit io_context_pool(std::size_t pool_size) : next_io_context_(0) {
    if (pool_size == 0) {
      pool_size = 1;  // set default value as 1
    }

    for (std::size_t i = 0; i < pool_size; ++i) {
      io_context_ptr io_context(new asio::io_context);
      work_ptr work(new asio::io_context::work(*io_context));
      io_contexts_.push_back(io_context);
      auto executor = std::make_unique<coro_io::ExecutorWrapper<>>(
          io_context->get_executor());
      executors.push_back(std::move(executor));
      work_.push_back(work);
    }
  }

  void run() {
    std::vector<std::shared_ptr<std::thread>> threads;
    for (std::size_t i = 0; i < io_contexts_.size(); ++i) {
      threads.emplace_back(std::make_shared<std::thread>(
          [](io_context_ptr svr) {
            svr->run();
          },
          io_contexts_[i]));
    }

    for (std::size_t i = 0; i < threads.size(); ++i) {
      threads[i]->join();
    }
    promise_.set_value();
  }

  void stop() {
    work_.clear();
    promise_.get_future().wait();
    return;
  }

  // ~io_context_pool() {
  //   if (!has_stop())
  //     stop();
  // }

  std::size_t pool_size() const noexcept { return io_contexts_.size(); }

  bool has_stop() const { return work_.empty(); }

  size_t current_io_context() { return next_io_context_ - 1; }

  coro_io::ExecutorWrapper<> *get_executor() {
    auto i = next_io_context_.fetch_add(1, std::memory_order::relaxed);
    auto *ret = executors[i % io_contexts_.size()].get();
    return ret;
  }

  template <typename T>
  friend io_context_pool &g_io_context_pool();

 private:
  using io_context_ptr = std::shared_ptr<asio::io_context>;
  using work_ptr = std::shared_ptr<asio::io_context::work>;

  std::vector<io_context_ptr> io_contexts_;
  std::vector<std::unique_ptr<coro_io::ExecutorWrapper<>>> executors;
  std::vector<work_ptr> work_;
  std::atomic<std::size_t> next_io_context_;
  std::promise<void> promise_;
};

class multithread_context_pool {
 public:
  multithread_context_pool(size_t thd_num = std::thread::hardware_concurrency())
      : work_(std::make_unique<asio::io_context::work>(ioc_)),
        executor_(ioc_.get_executor()),
        thd_num_(thd_num) {}

  ~multithread_context_pool() { stop(); }

  void run() {
    for (int i = 0; i < thd_num_; i++) {
      thds_.emplace_back([this] {
        ioc_.run();
      });
    }

    promise_.set_value();
  }

  void stop() {
    if (thds_.empty()) {
      return;
    }

    work_.reset();
    for (auto &thd : thds_) {
      thd.join();
    }
    promise_.get_future().wait();
    thds_.clear();
  }

  coro_io::ExecutorWrapper<> *get_executor() { return &executor_; }

 private:
  asio::io_context ioc_;
  std::unique_ptr<asio::io_context::work> work_;
  coro_io::ExecutorWrapper<> executor_;
  size_t thd_num_;
  std::vector<std::thread> thds_;
  std::promise<void> promise_;
};

template <typename T = io_context_pool>
inline T &g_io_context_pool(
    unsigned pool_size = std::thread::hardware_concurrency()) {
  static auto _g_io_context_pool = std::make_shared<T>(pool_size);
  static bool run_helper = [](auto pool) {
    std::thread thrd{[pool] {
      pool->run();
    }};
    thrd.detach();
    return true;
  }(_g_io_context_pool);
  return *_g_io_context_pool;
}

template <typename T = io_context_pool>
inline T &g_block_io_context_pool(
    unsigned pool_size = std::thread::hardware_concurrency()) {
  static auto _g_io_context_pool = std::make_shared<T>(pool_size);
  static bool run_helper = [](auto pool) {
    std::thread thrd{[pool] {
      pool->run();
    }};
    thrd.detach();
    return true;
  }(_g_io_context_pool);
  return *_g_io_context_pool;
}

template <typename T = io_context_pool>
inline auto get_global_executor() {
  return g_io_context_pool<T>().get_executor();
}

template <typename T = io_context_pool>
inline auto get_global_block_executor() {
  return g_block_io_context_pool<T>().get_executor();
}

}  // namespace coro_io
