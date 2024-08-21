/*
 * Copyright (c) 2023, Alibaba Group Holding Limited;
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
#include <async_simple/Promise.h>
#include <async_simple/Try.h>
#include <async_simple/Unit.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/Sleep.h>
#include <async_simple/coro/SpinLock.h>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../../cinatra_log_wrapper.hpp"
#include "../util/expected.hpp"
#include "async_simple/coro/Collect.h"
#include "coro_io.hpp"
#include "detail/client_queue.hpp"
#include "io_context_pool.hpp"

namespace coro_io {

template <typename client_t, typename io_context_pool_t>
class client_pools;

template <typename, typename>
class load_blancer;

template <typename client_t,
          typename io_context_pool_t = coro_io::io_context_pool>
class client_pool : public std::enable_shared_from_this<
                        client_pool<client_t, io_context_pool_t>> {
  using client_pools_t = client_pools<client_t, io_context_pool_t>;
  static async_simple::coro::Lazy<void> collect_idle_timeout_client(
      std::weak_ptr<client_pool> self_weak,
      coro_io::detail::client_queue<std::unique_ptr<client_t>>& clients,
      std::chrono::milliseconds sleep_time, std::size_t clear_cnt) {
    std::shared_ptr<client_pool> self = self_weak.lock();
    if (self == nullptr) {
      co_return;
    }
    while (true) {
      clients.reselect();
      self = nullptr;
      co_await coro_io::sleep_for(sleep_time);
      if ((self = self_weak.lock()) == nullptr) {
        break;
      }
      while (true) {
        CINATRA_LOG_TRACE << "start collect timeout client of pool{"
                          << self->host_name_
                          << "}, now client count: " << clients.size();
        std::size_t is_all_cleared = clients.clear_old(clear_cnt);
        CINATRA_LOG_TRACE << "finish collect timeout client of pool{"
                          << self->host_name_
                          << "}, now client cnt: " << clients.size();
        if (is_all_cleared != 0) [[unlikely]] {
          try {
            co_await async_simple::coro::Yield{};
          } catch (std::exception& e) {
            CINATRA_LOG_ERROR << "unexcepted yield exception: " << e.what();
          }
        }
        else {
          break;
        }
      }
      --clients.collecter_cnt_;
      if (clients.size() == 0) {
        break;
      }
      std::size_t expected = 0;
      if (!clients.collecter_cnt_.compare_exchange_strong(expected, 1))
        break;
    }
    co_return;
  }

  static auto rand_time(std::chrono::milliseconds ms) {
    static thread_local std::default_random_engine r;
    std::uniform_real_distribution e(1.0f, 1.2f);
    return std::chrono::milliseconds{static_cast<long>(e(r) * ms.count())};
  }

  static async_simple::coro::Lazy<std::pair<bool, std::chrono::milliseconds>>
  reconnect_impl(std::unique_ptr<client_t>& client,
                 std::shared_ptr<client_pool>& self) {
    auto pre_time_point = std::chrono::steady_clock::now();
    auto result = co_await client->connect(self->host_name_);
    bool ok = client_t::is_ok(result);
    auto post_time_point = std::chrono::steady_clock::now();
    auto cost_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        post_time_point - pre_time_point);
    CINATRA_LOG_TRACE << "reconnect client{" << client.get() << "} cost time: "
                      << cost_time / std::chrono::milliseconds{1} << "ms";
    co_return std::pair{ok, cost_time};
  }

  static async_simple::coro::Lazy<void> reconnect(
      std::unique_ptr<client_t>& client, std::weak_ptr<client_pool> watcher) {
    using namespace std::chrono_literals;
    std::shared_ptr<client_pool> self = watcher.lock();
    uint32_t i = UINT32_MAX;  // (at least connect once)
    do {
      CINATRA_LOG_TRACE << "try to reconnect client{" << client.get()
                        << "},host:{" << client->get_host() << ":"
                        << client->get_port() << "}, try count:" << i + 1
                        << "max retry limit:"
                        << self->pool_config_.connect_retry_count;
      auto [ok, cost_time] = co_await reconnect_impl(client, self);
      if (ok) {
        CINATRA_LOG_TRACE << "reconnect client{" << client.get() << "} success";
        co_return;
      }
      CINATRA_LOG_TRACE << "reconnect client{" << client.get()
                        << "} failed. If client close:{" << client->has_closed()
                        << "}";
      auto wait_time = rand_time(
          (self->pool_config_.reconnect_wait_time - cost_time) / 1ms * 1ms);
      self = nullptr;
      if (wait_time.count() > 0)
        co_await coro_io::sleep_for(wait_time, &client->get_executor());
      self = watcher.lock();
      ++i;
    } while (i < self->pool_config_.connect_retry_count);
    CINATRA_LOG_WARNING << "reconnect client{" << client.get() << "},host:{"
                        << client->get_host() << ":" << client->get_port()
                        << "} out of max limit, stop retry. connect failed";
    alive_detect(client->get_config(), std::move(self)).start([](auto&&) {
    });
    client = nullptr;
  }

  static async_simple::coro::Lazy<void> alive_detect(
      const typename client_t::config& client_config,
      std::weak_ptr<client_pool> watcher) {
    std::shared_ptr<client_pool> self = watcher.lock();
    using namespace std::chrono_literals;
    if (self && self->pool_config_.host_alive_detect_duration.count() != 0 &&
        self->free_client_count() == 0) {
      bool expected = true;
      if (!self->is_alive_.compare_exchange_strong(
              expected, false)) {  // other alive detect coroutine is running.
        co_return;
      }
      if (self->free_client_count() > 0) {  // recheck for multi-thread
        self->is_alive_ = true;
        co_return;
      }
      auto executor = self->io_context_pool_.get_executor();
      auto client = std::make_unique<client_t>(*executor);
      if (!client->init_config(client_config))
        AS_UNLIKELY {
          CINATRA_LOG_ERROR
              << "Init client config failed in host alive detect. That "
                 "is not expected.";
          co_return;
        }
      while (true) {
        auto [ok, cost_time] = co_await reconnect_impl(client, self);
        if (ok) {
          CINATRA_LOG_TRACE << "reconnect client{" << client.get()
                            << "} success. stop alive detect.";
          self->collect_free_client(std::move(client));
          self->is_alive_ =
              true; /*if client close(), we still mark it as alive*/
          co_return;
        }
        if (self->is_alive_) {
          CINATRA_LOG_TRACE << "client pool is aliving, stop connect client {"
                            << client.get() << "} for alive detect";
          co_return;
        }
        CINATRA_LOG_TRACE << "reconnect client{" << client.get()
                          << "} failed. continue alive detect.";
        auto wait_time = rand_time(
            (self->pool_config_.host_alive_detect_duration - cost_time) / 1ms *
            1ms);
        self = nullptr;
        if (wait_time.count() > 0) {
          co_await coro_io::sleep_for(wait_time, &client->get_executor());
        }
        self = watcher.lock();
        if (self->is_alive_) {
          CINATRA_LOG_TRACE << "client pool is aliving, stop connect client {"
                            << client.get() << "} for alive detect";
          co_return;
        }
      }
    }
  }

  async_simple::coro::Lazy<std::unique_ptr<client_t>> get_client(
      const typename client_t::config& client_config) {
    std::unique_ptr<client_t> client;
    free_clients_.try_dequeue(client);
    if (!client) {
      short_connect_clients_.try_dequeue(client);
    }
    if (client == nullptr) {
      std::unique_ptr<client_t> cli;
      auto executor = io_context_pool_.get_executor();
      client = std::make_unique<client_t>(*executor);
      if (!client->init_config(client_config))
        AS_UNLIKELY {
          CINATRA_LOG_ERROR << "init client config failed.";
          co_return nullptr;
        }
      co_await reconnect(client, this->weak_from_this());
    }
    else {
      CINATRA_LOG_TRACE << "get free client{" << client.get()
                        << "}. from queue";
    }
    co_return std::move(client);
  }

  void enqueue(
      coro_io::detail::client_queue<std::unique_ptr<client_t>>& clients,
      std::unique_ptr<client_t> client,
      std::chrono::milliseconds collect_time) {
    if (clients.enqueue(std::move(client)) == 1) {
      std::size_t expected = 0;
      if (clients.collecter_cnt_.compare_exchange_strong(expected, 1)) {
        CINATRA_LOG_TRACE << "start timeout client collecter of client_pool{"
                          << host_name_ << "}";
        collect_idle_timeout_client(
            this->weak_from_this(), clients,
            (std::max)(collect_time, std::chrono::milliseconds{50}),
            pool_config_.idle_queue_per_max_clear_count)
            .via(coro_io::get_global_executor())
            .start([](auto&&) {
            });
      }
    }
  }

  void collect_free_client(std::unique_ptr<client_t> client) {
    if (!client->has_closed()) {
      if (free_clients_.size() < pool_config_.max_connection) {
        CINATRA_LOG_TRACE << "collect free client{" << client.get()
                          << "} enqueue";
        enqueue(free_clients_, std::move(client), pool_config_.idle_timeout);
      }
      else {
        CINATRA_LOG_TRACE << "out of max connection limit <<"
                          << pool_config_.max_connection
                          << ", collect free client{" << client.get()
                          << "} enqueue short connect queue";
        enqueue(short_connect_clients_, std::move(client),
                pool_config_.short_connect_idle_timeout);
      }
      is_alive_ = true;
    }
    else {
      CINATRA_LOG_TRACE << "client{" << client.get()
                        << "} is closed. we won't collect it";
    }

    return;
  };

  template <typename T>
  struct lazy_hacker {
    using type = void;
  };
  template <typename T>
  struct lazy_hacker<async_simple::coro::Lazy<T>> {
    using type = T;
  };
  template <typename T>
  using return_type =
      tl::expected<typename lazy_hacker<decltype(std::declval<T>()(
                       std::declval<client_t&>()))>::type,
                   std::errc>;

  template <typename T>
  using return_type_with_host =
      tl::expected<typename lazy_hacker<decltype(std::declval<T>()(
                       std::declval<client_t&>(), std::string_view{}))>::type,
                   std::errc>;

 public:
  struct pool_config {
    uint32_t max_connection = 100;
    uint32_t connect_retry_count = 3;
    uint32_t idle_queue_per_max_clear_count = 1000;
    std::chrono::milliseconds reconnect_wait_time{1000};
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds short_connect_idle_timeout{1000};
    std::chrono::milliseconds host_alive_detect_duration{
        30000}; /* zero means wont detect */
    typename client_t::config client_config;
  };

 private:
  struct private_construct_token {};

 public:
  static std::shared_ptr<client_pool> create(
      std::string_view host_name, const pool_config& pool_config = {},
      io_context_pool_t& io_context_pool = coro_io::g_io_context_pool()) {
    return std::make_shared<client_pool>(private_construct_token{}, host_name,
                                         pool_config, io_context_pool);
  }

  client_pool(private_construct_token t, std::string_view host_name,
              const pool_config& pool_config,
              io_context_pool_t& io_context_pool)
      : host_name_(host_name),
        pool_config_(pool_config),
        io_context_pool_(io_context_pool),
        free_clients_(pool_config.max_connection){};

  client_pool(private_construct_token t, client_pools_t* pools_manager_,
              std::string_view host_name, const pool_config& pool_config,
              io_context_pool_t& io_context_pool)
      : pools_manager_(pools_manager_),
        host_name_(host_name),
        pool_config_(pool_config),
        io_context_pool_(io_context_pool),
        free_clients_(pool_config.max_connection){};

  template <typename T>
  async_simple::coro::Lazy<return_type<T>> send_request(
      T op, typename client_t::config& client_config) {
    // return type: Lazy<expected<T::returnType,std::errc>>
    CINATRA_LOG_TRACE << "try send request to " << host_name_;
    auto client = co_await get_client(client_config);
    if (!client) {
      CINATRA_LOG_WARNING << "send request to " << host_name_
                          << " failed. connection refused.";
      co_return return_type<T>{tl::unexpect, std::errc::connection_refused};
    }
    if constexpr (std::is_same_v<typename return_type<T>::value_type, void>) {
      co_await op(*client);
      collect_free_client(std::move(client));
      co_return return_type<T>{};
    }
    else {
      auto ret = co_await op(*client);
      collect_free_client(std::move(client));
      co_return std::move(ret);
    }
  }

  template <typename T>
  decltype(auto) send_request(T op) {
    return send_request(std::move(op), pool_config_.client_config);
  }

  /**
   * @brief approx connection of client pools
   *
   * @return std::size_t
   */
  std::size_t free_client_count() const noexcept {
    return free_clients_.size() + short_connect_clients_.size();
  }
  /**
   * @brief if host may not useable now.
   *
   * @return bool
   */
  bool is_alive() const noexcept { return is_alive_; }

  /**
   * @brief approx connection of client pools
   *
   * @return std::size_t
   */
  std::size_t size() const noexcept { return free_client_count(); }

  std::string_view get_host_name() const noexcept { return host_name_; }

 private:
  template <typename, typename>
  friend class client_pools;

  template <typename, typename>
  friend class load_blancer;

  template <typename T>
  async_simple::coro::Lazy<return_type_with_host<T>> send_request(
      T op, std::string_view endpoint,
      typename client_t::config& client_config) {
    // return type: Lazy<expected<T::returnType,std::errc>>
    CINATRA_LOG_TRACE << "try send request to " << endpoint;
    auto client = co_await get_client(client_config);
    if (!client) {
      CINATRA_LOG_WARNING << "send request to " << endpoint
                          << " failed. connection refused.";
      co_return return_type_with_host<T>{tl::unexpect,
                                         std::errc::connection_refused};
    }
    if constexpr (std::is_same_v<typename return_type_with_host<T>::value_type,
                                 void>) {
      co_await op(*client, endpoint);
      collect_free_client(std::move(client));
      co_return return_type_with_host<T>{};
    }
    else {
      auto ret = co_await op(*client, endpoint);
      collect_free_client(std::move(client));
      co_return std::move(ret);
    }
  }

  template <typename T>
  decltype(auto) send_request(T op, std::string_view sv) {
    return send_request(std::move(op), sv, pool_config_.client_config);
  }

  coro_io::detail::client_queue<std::unique_ptr<client_t>> free_clients_;
  coro_io::detail::client_queue<std::unique_ptr<client_t>>
      short_connect_clients_;
  client_pools_t* pools_manager_ = nullptr;
  async_simple::Promise<async_simple::Unit> idle_timeout_waiter;
  std::string host_name_;
  pool_config pool_config_;
  io_context_pool_t& io_context_pool_;
  std::atomic<bool> is_alive_ = true;
};

template <typename client_t,
          typename io_context_pool_t = coro_io::io_context_pool>
class client_pools {
  using client_pool_t = client_pool<client_t, io_context_pool_t>;

 public:
  client_pools(
      const typename client_pool_t::pool_config& pool_config = {},
      io_context_pool_t& io_context_pool = coro_io::g_io_context_pool())
      : io_context_pool_(io_context_pool), default_pool_config_(pool_config) {}
  auto send_request(std::string_view host_name, auto op)
      -> decltype(std::declval<client_pool_t>().send_request(std::move(op))) {
    auto pool = get_client_pool(host_name, default_pool_config_);
    auto ret = co_await pool->send_request(std::move(op));
    co_return ret;
  }
  auto send_request(std::string_view host_name,
                    typename client_pool_t::pool_config& pool_config, auto op)
      -> decltype(std::declval<client_pool_t>().send_request(std::move(op))) {
    auto pool = get_client_pool(host_name, pool_config);
    auto ret = co_await pool->send_request(std::move(op));
    co_return ret;
  }
  auto at(std::string_view host_name) {
    return get_client_pool(host_name, default_pool_config_);
  }
  auto at(std::string_view host_name,
          const typename client_pool_t::pool_config& pool_config) {
    return get_client_pool(host_name, pool_config);
  }
  auto operator[](std::string_view host_name) { return at(host_name); }
  auto get_io_context_pool() { return io_context_pool_; }

 private:
  std::shared_ptr<client_pool_t> get_client_pool(
      std::string_view host_name,
      const typename client_pool_t::pool_config& pool_config) {
    decltype(client_pool_manager_.end()) iter;
    bool has_inserted;
    {
#ifdef __cpp_lib_generic_unordered_lookup
      std::shared_lock shared_lock{mutex_};
      iter = client_pool_manager_.find(host_name);
#else
      std::string host_name_copy = std::string{host_name};
      std::shared_lock shared_lock{mutex_};
      iter = client_pool_manager_.find(host_name_copy);
#endif
      if (iter == client_pool_manager_.end()) {
        shared_lock.unlock();
        auto pool = std::make_shared<client_pool_t>(
            typename client_pool_t::private_construct_token{}, this, host_name,
            pool_config, io_context_pool_);
        {
          std::lock_guard lock{mutex_};
          std::tie(iter, has_inserted) =
              client_pool_manager_.emplace(host_name, nullptr);
          if (has_inserted) {
            iter->second = pool;
          }
        }
      }
      return iter->second;
    }
  }
  struct string_hash {
    using hash_type = std::hash<std::string_view>;
    using is_transparent = void;

    std::size_t operator()(std::string_view str) const {
      return hash_type{}(str);
    }
    std::size_t operator()(std::string const& str) const {
      return hash_type{}(str);
    }
  };
  typename client_pool_t::pool_config default_pool_config_{};
  std::unordered_map<std::string, std::shared_ptr<client_pool_t>, string_hash,
                     std::equal_to<>>
      client_pool_manager_;
  io_context_pool_t& io_context_pool_;
  std::shared_mutex mutex_;
};

template <typename client_t,
          typename io_context_pool_t = coro_io::io_context_pool>
inline client_pools<client_t, io_context_pool_t>& g_clients_pool(
    const typename client_pool<client_t, io_context_pool_t>::pool_config&
        pool_config = {},
    io_context_pool_t& io_context_pool = coro_io::g_io_context_pool()) {
  static client_pools<client_t, io_context_pool_t> _g_clients_pool(
      pool_config, io_context_pool);
  return _g_clients_pool;
}

}  // namespace coro_io
