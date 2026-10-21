#pragma once

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <asio/strand.hpp>

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_response.hpp"
#include "cinatra/coro_http_router.hpp"
#include "cinatra/define.h"
#include "cinatra/mime_types.hpp"
#include "cinatra_log_wrapper.hpp"
#include "coro_http_connection.hpp"
#include "ylt/coro_io/coro_file.hpp"
#include "ylt/coro_io/coro_io.hpp"
#include "ylt/coro_io/io_context_pool.hpp"
#include "ylt/coro_io/load_blancer.hpp"

namespace cinatra {
enum class file_resp_format_type {
  chunked,
  range,
};
class coro_http_server {
  using file_cache_type = std::unordered_map<std::string, std::string>;
  using file_cache_ptr = std::shared_ptr<file_cache_type>;
  using out_strand_type = asio::strand<asio::io_context::executor_type>;

  struct completion_control {
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
    std::error_code result;
  };

  struct accept_state {
    std::mutex mtx;
    std::error_code first_error;
    bool finalized = false;
    size_t remaining = 0;
    std::shared_ptr<completion_control> control;
  };

  enum class life_state { idle, starting, running, stopping, stopped };

  // Posted runtime handlers outlive the server when the io_context is owned
  // by the caller; the token lets them observe destruction without touching
  // any member. Handlers hold the mutex while running, so marking the server
  // dead waits for an in-flight handler to finish.
  struct life_token {
    std::mutex mtx;
    bool alive = true;
  };

 public:
  coro_http_server(asio::io_context &ctx, unsigned short port,
                   std::string address = "0.0.0.0")
      : out_ctx_(&ctx),
        out_strand_(asio::make_strand(ctx)),
        port_(port),
        check_timer_(*out_strand_),
        cache_refresh_timer_(*out_strand_) {
    init_address(std::move(address));
  }

  coro_http_server(asio::io_context &ctx,
                   std::string address /* = "0.0.0.0:9001" */)
      : out_ctx_(&ctx),
        out_strand_(asio::make_strand(ctx)),
        check_timer_(*out_strand_),
        cache_refresh_timer_(*out_strand_) {
    init_address(std::move(address));
  }

  coro_http_server(size_t thread_num, unsigned short port,
                   std::string address = "0.0.0.0", bool cpu_affinity = false)
      : pool_(std::make_unique<coro_io::io_context_pool>(thread_num,
                                                         cpu_affinity)),
        port_(port),
        thread_num_(thread_num),
        cpu_affinity_(cpu_affinity),
        check_timer_(pool_->get_executor()->get_asio_executor()),
        cache_refresh_timer_(pool_->get_executor()->get_asio_executor()) {
    init_address(std::move(address));
  }

  coro_http_server(size_t thread_num,
                   std::string address /* = "0.0.0.0:9001" */,
                   bool cpu_affinity = false)
      : pool_(std::make_unique<coro_io::io_context_pool>(thread_num,
                                                         cpu_affinity)),
        thread_num_(thread_num),
        cpu_affinity_(cpu_affinity),
        check_timer_(pool_->get_executor()->get_asio_executor()),
        cache_refresh_timer_(pool_->get_executor()->get_asio_executor()) {
    init_address(std::move(address));
  }

  ~coro_http_server() {
    CINATRA_LOG_INFO << "coro_http_server will quit";
    stop();
    std::scoped_lock lock(life_token_->mtx);
    life_token_->alive = false;
  }

  void set_no_delay(bool r) { no_delay_ = r; }

  // On POSIX platforms that provide SO_REUSEPORT, servers in the same
  // io_context pool share the listening port by default (one acceptor per
  // pool thread). This is a behavioral change: a second server bound to the
  // same port joins the group instead of failing with address_in_use. Call
  // set_reuse_port(false) before start to keep exclusive binding.
  void set_reuse_port(bool enable) { reuse_port_ = enable; }

  void set_max_http_body_size(int64_t max_size) {
    max_http_body_len_ = max_size;
  }

  void set_max_http_header_size(size_t max_size) {
    max_http_header_size_ = max_size;
  }

#ifdef CINATRA_ENABLE_SSL
  void init_ssl(const std::string &cert_file, const std::string &key_file,
                const std::string &passwd = "") {
    cert_file_ = cert_file;
    key_file_ = key_file;
    passwd_ = passwd;
    use_ssl_ = true;
  }
#endif

  std::error_code sync_start() noexcept {
    auto ret = async_start();
    ret.wait();
    return ret.value();
  }

  // Completes with operation_not_permitted if the server is not idle. A
  // start that fails before any acceptor loop runs rolls back to idle, so
  // the caller may retry with the same server.
  async_simple::Future<std::error_code> async_start() {
    auto promise = std::make_shared<async_simple::Promise<std::error_code>>();
    auto future = promise->getFuture();

    {
      std::scoped_lock lock(life_mtx_);
      if (life_ != life_state::idle) {
        auto ec = std::make_error_code(std::errc::operation_not_permitted);
        promise->setValue(ec);
        return future;
      }
      life_ = life_state::starting;
    }

    // A previous start may have failed during pool startup and rolled back to
    // idle; rebuild the per-run synchronization state so this attempt does not
    // inherit its result, shutdown flag, or consumed waiter.
    shutting_down_.store(false, std::memory_order_release);
    stop_timer_.store(false, std::memory_order_release);
    set_errc(std::error_code{});
    acceptor_close_waiter_ = std::promise<void>{};
    acceptor_close_future_ = acceptor_close_waiter_.get_future();

    std::shared_ptr<accept_state> state;
    std::shared_ptr<completion_control> control;
    size_t started = 0;
    bool state_armed = false;

    try {
      auto listen_error = listen();
      if (listen_error) {
        set_errc(listen_error);
        // Leave `starting` before the future completes: a continuation may
        // legitimately start the server again as soon as it observes the
        // failure.
        leave_starting(life_state::idle);
        promise->setValue(listen_error);
        return future;
      }

      state = std::make_shared<accept_state>();
      control = std::make_shared<completion_control>();
      state->control = control;

      try {
#ifdef INJECT_FOR_HTTP_SEVER_TEST
        if (inject_completion_failure_) {
          throw std::runtime_error("injected completion thread failure");
        }
#endif
        std::thread completion_thread([control, promise] {
          std::error_code result;
          {
            std::unique_lock lock(control->mtx);
            control->cv.wait(lock, [&control] { return control->ready; });
            result = control->result;
          }
          try {
            promise->setValue(result);
          } catch (...) {
            CINATRA_LOG_ERROR << "failed to complete start future";
          }
        });
        completion_thread.detach();
      } catch (...) {
        auto start_error = std::make_error_code(std::errc::io_error);
        set_errc(start_error);
        close_listeners();
        leave_starting(life_state::idle);
        promise->setValue(start_error);
        return future;
      }

      if (out_ctx_ == nullptr) {
        try {
          thd_ = std::thread([this] {
            try {
              pool_->run();
            } catch (...) {
              CINATRA_LOG_ERROR << "server thread pool run failed";
            }
          });
        } catch (...) {
          auto start_error = std::make_error_code(std::errc::io_error);
          set_errc(start_error);
          close_listeners();
          leave_starting(life_state::idle);
          complete_control(control, start_error);
          return future;
        }

        if (!pool_->wait_run_started()) {
          auto start_error = std::make_error_code(std::errc::io_error);
          set_errc(start_error);
          close_listeners();
          if (thd_.joinable()) {
            thd_.join();
          }
          // The failed pool cannot run again, and its executors own the
          // acceptors that were just closed. Replace it so a later start on
          // this server gets a working pool instead of a dead one.
          reset_pool();
          leave_starting(life_state::idle);
          complete_control(control, start_error);
          return future;
        }
      }

      // Background timers/coroutines are created only after the listener and
      // the pool are usable, and only for a start that will proceed.
      start_background_tasks();

      // The extra unit keeps the future pending until every launched loop
      // has exited, even if a loop completes synchronously.
      state->remaining = acceptors_.size() + 1;
      state_armed = true;

      std::error_code launch_error;
      for (size_t i = 0; i < acceptors_.size(); ++i) {
        try {
#ifdef INJECT_FOR_HTTP_SEVER_TEST
          if (inject_launch_failure_index_ == i) {
            throw std::runtime_error("injected accept launch failure");
          }
#endif
          coro_io::ExecutorWrapper<> *fixed_executor =
              acceptors_.size() > 1 ? pool_->get_executor_at(i) : nullptr;
          accept(*acceptors_[i], fixed_executor)
              .start([state, this](auto &&res) mutable {
                on_accept_loop_exit(state, std::move(res));
              });
          ++started;
#ifdef INJECT_FOR_HTTP_SEVER_TEST
          if (inject_launch_delay_.count() > 0) {
            std::this_thread::sleep_for(inject_launch_delay_);
          }
#endif
        } catch (...) {
          launch_error = std::make_error_code(std::errc::io_error);
          break;
        }
      }

      if (launch_error) {
        fail_launch(state, acceptors_.size() - started);
      }
      else {
        finish_accept_state(state);
      }

      leave_starting(life_state::running);
      return future;
    } catch (...) {
      auto start_error = std::make_error_code(std::errc::io_error);
      if (state_armed) {
        fail_launch(state, acceptors_.size() - started);
        leave_starting(life_state::running);
      }
      else {
        set_errc(start_error);
        // If the failure happened while arming background tasks, tear them
        // down before the pool goes away.
        stop_background_tasks();
        close_listeners();
        if (out_ctx_ == nullptr && thd_.joinable()) {
          pool_->stop();
          thd_.join();
        }
        leave_starting(life_state::idle);
        if (control) {
          complete_control(control, start_error);
        }
        else {
          promise->setValue(start_error);
        }
      }
      return future;
    }
  }

  // Idempotent and safe to call concurrently. Must not be called on one of
  // the server's own io_context worker threads (a continuation that calls
  // stop() runs on the completion thread, which is not a worker).
  void stop() {
    std::unique_lock lock(life_mtx_);
    for (;;) {
      switch (life_) {
        case life_state::idle:
        case life_state::stopped:
          return;
        case life_state::starting:
          // The starting thread never waits for stop(), so this converges.
          life_cv_.wait(lock);
          break;
        case life_state::stopping:
          // Wait for the shutdown already in progress. That path never joins
          // the completion thread, so waiting from a continuation run by the
          // completion thread cannot deadlock.
          life_cv_.wait(lock,
                        [this] { return life_ == life_state::stopped; });
          return;
        case life_state::running:
          life_ = life_state::stopping;
          lock.unlock();
          stop_impl();
          lock.lock();
          life_ = life_state::stopped;
          lock.unlock();
          life_cv_.notify_all();
          return;
      }
    }
  }

  // call it after server async_start or sync_start.
  uint16_t port() const { return port_; }

  template <http_method... method, typename Func, typename... Aspects>
  void set_http_handler(std::string key, Func handler, Aspects &&...asps) {
    static_assert(sizeof...(method) >= 1, "must set http_method");
    if constexpr (sizeof...(method) == 1) {
      (router_.set_http_handler<method>(std::move(key), std::move(handler),
                                        std::forward<Aspects>(asps)...),
       ...);
    }
    else {
      (router_.set_http_handler<method>(key, handler,
                                        std::forward<Aspects>(asps)...),
       ...);
    }
  }

  template <http_method... method, typename Func, typename... Aspects>
  void set_http_handler(std::string key, Func handler,
                        util::class_type_t<Func> &owner, Aspects &&...asps) {
    static_assert(std::is_member_function_pointer_v<Func>,
                  "must be member function");
    using return_type = typename util::function_traits<Func>::return_type;
    if constexpr (coro_io::is_lazy_v<return_type>) {
      std::function<async_simple::coro::Lazy<void>(coro_http_request & req,
                                                   coro_http_response & resp)>
          f = std::bind(handler, &owner, std::placeholders::_1,
                        std::placeholders::_2);
      set_http_handler<method...>(std::move(key), std::move(f),
                                  std::forward<Aspects>(asps)...);
    }
    else {
      std::function<void(coro_http_request & req, coro_http_response & resp)>
          f = std::bind(handler, &owner, std::placeholders::_1,
                        std::placeholders::_2);
      set_http_handler<method...>(std::move(key), std::move(f),
                                  std::forward<Aspects>(asps)...);
    }
  }

  template <http_method... method, typename... Aspects>
  void set_http_proxy_handler(std::string url_path,
                              std::vector<std::string_view> hosts,
                              coro_io::load_blance_algorithm type =
                                  coro_io::load_blance_algorithm::random,
                              std::vector<int> weights = {},
                              Aspects &&...aspects) {
    if (hosts.empty()) {
      throw std::invalid_argument("not config hosts yet!");
    }

    auto load_blancer =
        std::make_shared<coro_io::load_blancer<coro_http_client>>(
            coro_io::load_blancer<coro_http_client>::create(
                hosts, {.lba = type}, weights));
    auto handler =
        [this, load_blancer, type](
            coro_http_request &req,
            coro_http_response &response) -> async_simple::coro::Lazy<void> {
      co_await load_blancer->send_request(
          [this, &req, &response](
              coro_http_client &client,
              std::string_view host) -> async_simple::coro::Lazy<void> {
            co_await reply(client, host, req, response);
          });
    };

    if constexpr (sizeof...(method) == 0) {
      set_http_handler<http_method::GET, http_method::POST, http_method::DEL,
                       http_method::HEAD, http_method::PUT, http_method::PATCH,
                       http_method::CONNECT, http_method::TRACE,
                       http_method::OPTIONS>(url_path, std::move(handler),
                                             std::forward<Aspects>(aspects)...);
    }
    else {
      set_http_handler<method...>(url_path, std::move(handler),
                                  std::forward<Aspects>(aspects)...);
    }
  }

  template <http_method... method, typename... Aspects>
  void set_websocket_proxy_handler(std::string url_path,
                                   std::vector<std::string_view> hosts,
                                   coro_io::load_blance_algorithm type =
                                       coro_io::load_blance_algorithm::random,
                                   std::vector<int> weights = {},
                                   Aspects &&...aspects) {
    if (hosts.empty()) {
      throw std::invalid_argument("not config hosts yet!");
    }

    auto load_blancer =
        std::make_shared<coro_io::load_blancer<coro_http_client>>(
            coro_io::load_blancer<coro_http_client>::create(
                hosts, {.lba = type}, weights));

    set_http_handler<cinatra::GET>(
        url_path,
        [load_blancer](coro_http_request &req, coro_http_response &resp)
            -> async_simple::coro::Lazy<void> {
          websocket_result result{};
          while (true) {
            result = co_await req.get_conn()->read_websocket();
            if (result.ec) {
              break;
            }

            if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
              CINATRA_LOG_INFO << "close frame";
              break;
            }

            auto ret = co_await load_blancer->send_request(
                [&req, result](coro_http_client &client, std::string_view host)
                    -> async_simple::coro::Lazy<std::error_code> {
                  auto r =
                      co_await client.write_websocket(std::string(result.data));
                  if (r.net_err) {
                    co_return r.net_err;
                  }
                  auto data = co_await client.read_websocket();
                  if (data.net_err) {
                    co_return data.net_err;
                  }
                  auto ec = co_await req.get_conn()->write_websocket(
                      std::string(result.data));
                  if (ec) {
                    co_return ec;
                  }
                  co_return std::error_code{};
                });
            if (!ret.has_value()) {
              req.get_conn()->close();
              break;
            }
          }
        },
        std::forward<Aspects>(aspects)...);
  }

  void set_max_size_of_cache_files(size_t max_size = 3 * 1024 * 1024) {
    std::error_code ec;
    auto new_cache =
        std::make_shared<std::unordered_map<std::string, std::string>>();
    for (const auto &file :
         std::filesystem::recursive_directory_iterator(static_dir_, ec)) {
      if (ec) {
        continue;
      }

      if (!file.is_directory()) {
        size_t filesize = fs::file_size(file, ec);
        if (ec || filesize > max_size) {
          continue;
        }

        std::ifstream ifs(file.path(), std::ios::binary);
        if (ifs.is_open()) {
          std::string content;
          detail::resize(content, filesize);
          ifs.read(content.data(), content.size());
          new_cache->emplace(file.path().string(), std::move(content));
        }
      }
    }
    store_file_cache(std::move(new_cache));
  }

  const coro_http_router &get_router() const { return router_; }

  void set_file_resp_format_type(file_resp_format_type type) {
    format_type_ = type;
  }

  void set_transfer_chunked_size(size_t size) { chunked_size_ = size; }

  // Enable background cache refresh for the static resource directory.
  // The timer checks the directory mtime every `interval`; only when the
  // directory has changed (file added/removed) is the cache rebuilt.
  // The background task itself is created by async_start(), so a server that
  // never starts (or fails to listen) holds no live timer referencing it.
  // Safe to call from any thread while the server runs: the field updates are
  // mutex-protected and the timer is only touched on its own executor.
  void set_cache_refresh_interval(
      std::chrono::steady_clock::duration interval = std::chrono::seconds(3),
      size_t max_file_size = 3 * 1024 * 1024) {
    asio::any_io_executor executor;
    {
      std::scoped_lock lock(config_mtx_);
      cache_refresh_interval_ = interval;
      max_cache_file_size_ = max_file_size;
      cache_refresh_enabled_ = true;
      executor = cache_refresh_timer_.get_executor();
    }
    // An idle server only records the config so a failed listen cannot leave
    // a dangling task. The loop re-reads interval and size every cycle, so an
    // already active refresh needs no restart; one first enabled at runtime
    // is started on the timer executor.
    if (!is_running()) {
      return;
    }
    post_guarded(executor, [this] {
      {
        std::scoped_lock lock(config_mtx_);
        if (cache_refresh_active_ ||
            stop_timer_.load(std::memory_order_acquire)) {
          return;
        }
        cache_refresh_stopped_ = std::promise<void>{};
        cache_refresh_done_ = cache_refresh_stopped_.get_future().share();
        cache_refresh_active_ = true;
      }
      cache_refresh_loop().start([](auto &&) {
      });
    });
  }

#ifdef INJECT_FOR_HTTP_SEVER_TEST
  void set_write_failed_forever(bool r) { write_failed_forever_ = r; }

  void set_read_failed_forever(bool r) { read_failed_forever_ = r; }

  enum class inject_failure {
    none,
    resolver_error,
    resolver_empty,
    open,
    reuse_address,
    reuse_port,
    bind,
    listen,
    local_endpoint,
    extra_acceptor,
  };

  void set_inject_failure(inject_failure failure) { inject_failure_ = failure; }

  void set_accept_exception(bool fail,
                            size_t listener_index = static_cast<size_t>(-1)) {
    accept_exception_ = fail;
    accept_exception_listener_ = listener_index;
  }

  void set_accept_transient_errors(
      int count, asio::error_code error = asio::error::would_block) {
    accept_transient_errors_ = count;
    accept_transient_error_ = error;
  }

  void set_inject_launch_failure(size_t listener_index) {
    inject_launch_failure_index_ = listener_index;
  }

  void set_inject_completion_failure(bool fail) {
    inject_completion_failure_ = fail;
  }

  // Pause between listener launches so a failure of an earlier listener can
  // race with the remaining launches deterministically.
  void set_inject_launch_delay(std::chrono::milliseconds delay) {
    inject_launch_delay_ = delay;
  }
#endif

  template <typename... Aspects>
  void set_static_res_dir(std::string_view uri_suffix = "",
                          std::string file_path = "www", Aspects &&...aspects) {
    bool has_double_dot = (file_path.find("..") != std::string::npos) ||
                          (uri_suffix.find("..") != std::string::npos);
    if (std::filesystem::path(file_path).has_root_path() ||
        std::filesystem::path(uri_suffix).has_root_path() || has_double_dot) {
      CINATRA_LOG_ERROR << "invalid file path: " << file_path;
      std::exit(1);
    }

    if (!uri_suffix.empty()) {
      static_dir_router_path_ =
          std::filesystem::path(uri_suffix).make_preferred().string();
    }

    // Track whether a named subdirectory was given, which determines the URI
    // prefix when uri_suffix is also empty.
    bool has_named_dir = false;
    if (!file_path.empty()) {
      file_path = std::filesystem::path(file_path).filename().string();
      if (file_path.empty()) {
        static_dir_ = fs::absolute(fs::current_path().string()).string();
      }
      else {
        static_dir_ =
            std::filesystem::path(file_path).make_preferred().string();
        has_named_dir = true;
      }
    }
    else {
      static_dir_ = fs::absolute(fs::current_path().string()).string();
    }

    // URI prefix: use uri_suffix if given, otherwise serve at root "/(.+)"
    std::string uri_prefix;
    if (!static_dir_router_path_.empty()) {
      uri_prefix = "/" + static_dir_router_path_;
    }
    std::string pattern = uri_prefix + "/(.+)";
    set_http_handler<cinatra::GET>(
        pattern,
        [this, base_dir = static_dir_](
            coro_http_request &req,
            coro_http_response &resp) -> async_simple::coro::Lazy<void> {
          std::string rel = req.matches_.str(1);
          replace_all(rel, "\\", "/");
          std::string file_name =
              (fs::path(base_dir) / rel).make_preferred().string();

          std::error_code path_ec;
          auto abs = fs::weakly_canonical(file_name, path_ec);
          auto base = fs::weakly_canonical(base_dir, path_ec);
          if (path_ec || abs.string().find(base.string()) != 0) {
            resp.set_status(status_type::bad_request);
            co_return;
          }

          co_await serve_static_file_(file_name, req, resp);
        },
        std::forward<Aspects>(aspects)...);

    set_cache_refresh_interval(std::chrono::seconds(3));
  }

 public:
  async_simple::coro::Lazy<void> serve_static_file_(
      const std::string &file_name, coro_http_request &req,
      coro_http_response &resp) {
    std::string_view extension = get_extension(file_name);
    std::string_view mime = get_mime_type(extension);
    auto range_str = req.get_header_value("Range");

    auto cache = load_file_cache();
    if (cache) {
      if (auto it = cache->find(file_name); it != cache->end()) {
        auto range_header = build_range_header(
            mime, file_name, std::to_string(fs::file_size(file_name)));
        resp.set_delay(true);
        const std::string &body = it->second;
        std::array<asio::const_buffer, 2> arr{asio::buffer(range_header),
                                              asio::buffer(body)};
        co_await req.get_conn()->async_write(arr);
        co_return;
      }
    }

    std::string content;
    detail::resize(content, chunked_size_);

    if (fs::is_directory(file_name)) {
      resp.set_status(status_type::not_found);
      co_return;
    }

    std::error_code size_ec;
    size_t file_size = fs::file_size(file_name, size_ec);
    if (size_ec) {
      resp.set_status(status_type::not_found);
      co_return;
    }

    coro_io::coro_file in_file{};
    in_file.open(file_name, std::ios::in);
    if (!in_file.is_open()) {
#ifndef NDEBUG
      resp.set_status_and_content(status_type::not_found,
                                  file_name + " not found");
#else
      resp.set_status(status_type::not_found);
#endif
      co_return;
    }

    if (format_type_ == file_resp_format_type::chunked && range_str.empty()) {
      resp.add_header("Content-Type", std::string{mime});
      resp.set_format_type(format_type::chunked);
      bool ok;
      if (ok = co_await resp.get_conn()->begin_chunked(); !ok) {
        co_return;
      }
      while (true) {
        auto [ec, size] =
            co_await in_file.async_read(content.data(), content.size());
        if (ec) {
          resp.set_status(status_type::no_content);
          co_await resp.get_conn()->reply();
          co_return;
        }
        bool r = co_await resp.get_conn()->write_chunked(
            std::string_view(content.data(), size));
        if (!r) {
          co_return;
        }
        if (in_file.eof()) {
          co_await resp.get_conn()->end_chunked();
          break;
        }
      }
      co_return;
    }

    auto pos = range_str.find('=');
    if (pos != std::string_view::npos) {
      range_str = range_str.substr(pos + 1);
      bool is_valid = true;
      auto ranges = parse_ranges(range_str, file_size, is_valid);
      if (!is_valid) {
        resp.set_status(status_type::range_not_satisfiable);
        co_return;
      }
      assert(!ranges.empty());

      if (ranges.size() == 1) {
        auto [start, end] = ranges[0];
        in_file.seek(start, std::ios::beg);
        size_t part_size = end + 1 - start;
        int status = (part_size == file_size) ? 200 : 206;
        std::string content_range = "Content-Range: bytes ";
        content_range.append(std::to_string(start))
            .append("-")
            .append(std::to_string(end))
            .append("/")
            .append(std::to_string(file_size))
            .append(CRCF);
        auto range_header = build_range_header(
            mime, file_name, std::to_string(part_size), status, content_range);
        resp.set_delay(true);
        bool r = co_await req.get_conn()->write_data(range_header);
        if (!r) {
          co_return;
        }
        co_await send_single_part(in_file, content, req, resp, part_size);
      }
      else {
        resp.set_delay(true);
        std::string file_size_str = std::to_string(file_size);
        size_t content_len = 0;
        std::vector<std::string> multi_heads =
            build_part_heads(ranges, mime, file_size_str, content_len);
        auto range_header = build_multiple_range_header(content_len);
        bool r = co_await req.get_conn()->write_data(range_header);
        if (!r) {
          co_return;
        }
        for (int i = 0; i < ranges.size(); i++) {
          r = co_await req.get_conn()->write_data(multi_heads[i]);
          if (!r) {
            co_return;
          }
          auto [start, end] = ranges[i];
          bool ok = in_file.seek(start, std::ios::beg);
          if (!ok) {
            resp.set_status_and_content(status_type::bad_request,
                                        "invalid range");
            co_await resp.get_conn()->reply();
            co_return;
          }
          size_t part_size = end + 1 - start;
          std::string_view more =
              (i == (int)ranges.size() - 1) ? MULTIPART_END : CRCF;
          r = co_await send_single_part(in_file, content, req, resp, part_size,
                                        more);
          if (!r) {
            co_return;
          }
        }
      }
      co_return;
    }

    auto range_header =
        build_range_header(mime, file_name, std::to_string(file_size));
    resp.set_delay(true);
    bool r = co_await req.get_conn()->write_data(range_header);
    if (!r) {
      co_return;
    }
    while (true) {
      auto [ec, size] =
          co_await in_file.async_read(content.data(), content.size());
      if (ec) {
        resp.set_status(status_type::no_content);
        co_await resp.get_conn()->reply();
        co_return;
      }
      r = co_await req.get_conn()->write_data(
          std::string_view(content.data(), size));
      if (!r) {
        co_return;
      }
      if (in_file.eof()) {
        break;
      }
    }
  }

  void set_check_duration(auto duration) {
    std::scoped_lock lock(config_mtx_);
    check_duration_ = duration;
  }

  // Before startup this only records the timeout: arming the timer in an idle
  // server would leave a handler pointing at this server after a failed
  // listen and teardown. A running server arms it on the timer executor so a
  // call from a business thread cannot race the handler running on an IO
  // thread.
  void set_timeout_duration(
      std::chrono::steady_clock::duration timeout_duration) {
    if (!(timeout_duration > std::chrono::steady_clock::duration::zero())) {
      return;
    }
    asio::any_io_executor executor;
    {
      std::scoped_lock lock(config_mtx_);
      need_check_ = true;
      timeout_duration_ = timeout_duration;
      executor = check_timer_.get_executor();
    }
    if (!is_running()) {
      return;
    }
    post_guarded(executor, [this] {
      if (stop_timer_.load(std::memory_order_acquire) || !is_running()) {
        return;
      }
      start_check_timer();
    });
  }

  void set_shrink_to_fit(bool r) { need_shrink_every_time_ = r; }

  void set_default_handler(std::function<async_simple::coro::Lazy<void>(
                               coro_http_request &, coro_http_response &)>
                               handler) {
    default_handler_ = std::move(handler);
  }

  void set_error_handler(
      std::function<void(coro_http_request &, coro_http_response &,
                         std::string_view)>
          handler) {
    router_.set_error_handler(std::move(handler));
  }

  size_t connection_count() {
    std::scoped_lock lock(conn_mtx_);
    return connections_.size();
  }

  std::string_view address() { return address_; }
  std::error_code get_errc() {
    std::scoped_lock lock(errc_mtx_);
    return errc_;
  }

 private:
  void set_errc(std::error_code ec) {
    std::scoped_lock lock(errc_mtx_);
    errc_ = ec;
  }

  void leave_starting(life_state next) {
    std::unique_lock lock(life_mtx_);
    life_ = next;
    lock.unlock();
    life_cv_.notify_all();
  }

  bool is_running() {
    std::scoped_lock lock(life_mtx_);
    return life_ == life_state::running;
  }

  static void complete_control(
      const std::shared_ptr<completion_control> &control,
      std::error_code ec) {
    {
      std::scoped_lock lock(control->mtx);
      control->result = ec;
      control->ready = true;
    }
    control->cv.notify_one();
  }

  void on_accept_loop_exit(const std::shared_ptr<accept_state> &state,
                           async_simple::Try<std::error_code> res) noexcept {
    std::error_code error;
    bool fatal = false;
    if (res.hasError()) {
      error = std::make_error_code(std::errc::io_error);
      fatal = true;
    }
    else {
      error = res.value();
      bool stopping = shutting_down_.load(std::memory_order_acquire);
      if (error == asio::error::operation_aborted ||
          error == asio::error::bad_descriptor) {
        fatal = !stopping;
      }
      else if (!error) {
        fatal = !stopping;
        if (fatal) {
          error = std::make_error_code(std::errc::io_error);
        }
      }
      else {
        fatal = true;
      }
    }

    if (fatal) {
      try {
        CINATRA_LOG_ERROR << "accept loop failed: " << error.message();
      } catch (...) {
      }
      {
        std::scoped_lock lock(state->mtx);
        if (!state->first_error) {
          state->first_error = error;
        }
      }
      if (!shutting_down_.exchange(true)) {
        close_acceptors_async();
      }
    }

    finish_accept_state(state);
  }

  // Called from on_accept_loop_exit (a noexcept completion callback) and from
  // the startup failure path; it must never let an exception escape.
  void finish_accept_state(const std::shared_ptr<accept_state> &state) noexcept {
    try {
      std::error_code first_error;
      bool last = false;
      {
        std::scoped_lock lock(state->mtx);
        if (--state->remaining == 0 && !state->finalized) {
          state->finalized = true;
          first_error = state->first_error;
          last = true;
        }
      }
      if (!last) {
        return;
      }

      auto final_result =
          first_error ? first_error : asio::error::operation_aborted;
      if (first_error) {
        set_errc(first_error);
      }

      try {
        acceptor_close_waiter_.set_value();
      } catch (...) {
      }

      auto control = state->control;
      complete_control(control, final_result);
    } catch (...) {
    }
  }

  void fail_launch(const std::shared_ptr<accept_state> &state,
                   size_t unstarted) noexcept {
    try {
      auto launch_error = std::make_error_code(std::errc::io_error);
      {
        std::scoped_lock lock(state->mtx);
        if (!state->first_error) {
          state->first_error = launch_error;
        }
      }
      if (!shutting_down_.exchange(true)) {
        close_acceptors_async();
      }
      // Loops that were never started will never decrement `remaining`, and
      // the launch permit is still held.
      for (size_t i = 0; i < unstarted + 1; ++i) {
        finish_accept_state(state);
      }
    } catch (...) {
    }
  }

  static bool is_backoff_accept_error(const asio::error_code &ec) {
    if (ec == asio::error::no_descriptors ||
        ec == asio::error::no_memory ||
        ec == asio::error::no_buffer_space) {
      return true;
    }
#if defined(ENFILE)
    return ec.value() == ENFILE;
#else
    return false;
#endif
  }

  static bool is_retryable_accept_error(const asio::error_code &ec) {
    return ec == asio::error::connection_aborted ||
           ec == asio::error::interrupted || ec == asio::error::would_block ||
           ec == asio::error::try_again || ec == asio::error::network_down ||
           ec == asio::error::network_reset ||
           ec == asio::error::network_unreachable ||
           ec == asio::error::host_unreachable ||
           ec == asio::error::timed_out;
  }

  std::error_code listen() {
    CINATRA_LOG_INFO << "begin to listen " << port_;
    using asio::ip::tcp;
    asio::error_code ec;

#if defined(SO_REUSEPORT)
    bool multi_acceptor = reuse_port_ && out_ctx_ == nullptr &&
                          pool_ != nullptr && pool_->pool_size() > 1;
    const size_t acceptor_count = multi_acceptor ? pool_->pool_size() : 1;
#else
    constexpr bool multi_acceptor = false;
    constexpr size_t acceptor_count = 1;
    (void)reuse_port_;
#endif

    // The caller may run an external io_context from several threads, so all
    // acceptor operations in that mode go through one strand. The pool's
    // contexts are single-threaded, so their plain executors already
    // serialize.
    auto executor_at =
        [this](size_t i) -> asio::ip::tcp::acceptor::executor_type {
      if (out_ctx_ != nullptr) {
        return *out_strand_;
      }
      return pool_->get_executor_at(i)->get_asio_executor();
    };

    acceptors_.clear();
    acceptors_.reserve(acceptor_count);
    acceptors_.push_back(std::make_shared<tcp::acceptor>(executor_at(0)));

    asio::ip::tcp::resolver resolver(executor_at(0));
    auto results = resolver.resolve(address_, std::to_string(port_), ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
    if (!ec && inject_failure_ == inject_failure::resolver_error) {
      ec = asio::error::host_not_found;
    }
    if (!ec && inject_failure_ == inject_failure::resolver_empty) {
      results = decltype(results)();
    }
#endif

    if (ec || results.empty()) {
      CINATRA_LOG_ERROR << "bad address: " << address_
                        << " error: " << ec.message();
      acceptors_.clear();
      if (ec) {
        return ec;
      }
      return std::make_error_code(std::errc::address_not_available);
    }

    auto endpoint = results.begin()->endpoint();

    auto listen_one = [this](tcp::acceptor &acceptor, const tcp::endpoint &ep,
                             bool use_reuse_port) -> std::error_code {
      asio::error_code ec;
      acceptor.open(ep.protocol(), ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (!ec && inject_failure_ == inject_failure::open) {
        ec = std::make_error_code(std::errc::operation_not_supported);
      }
#endif
      if (ec) {
        return ec;
      }
#ifdef __GNUC__
      acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (!ec && inject_failure_ == inject_failure::reuse_address) {
        ec = std::make_error_code(std::errc::operation_not_supported);
      }
#endif
      if (ec) {
        return ec;
      }
#endif
#if defined(SO_REUSEPORT)
      if (use_reuse_port) {
        acceptor.set_option(
            asio::detail::socket_option::integer<SOL_SOCKET, SO_REUSEPORT>(1),
            ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
        if (!ec && inject_failure_ == inject_failure::reuse_port) {
          ec = std::make_error_code(std::errc::operation_not_supported);
        }
#endif
        if (ec) {
          return ec;
        }
      }
#else
      (void)use_reuse_port;
#endif
      acceptor.bind(ep, ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (!ec && inject_failure_ == inject_failure::bind) {
        ec = std::make_error_code(std::errc::operation_not_supported);
      }
#endif
      if (ec) {
        return ec;
      }
#ifdef _MSC_VER
      acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (!ec && inject_failure_ == inject_failure::reuse_address) {
        ec = std::make_error_code(std::errc::operation_not_supported);
      }
#endif
      if (ec) {
        return ec;
      }
#endif
      acceptor.listen(asio::socket_base::max_listen_connections, ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (!ec && inject_failure_ == inject_failure::listen) {
        ec = std::make_error_code(std::errc::operation_not_supported);
      }
#endif
      return ec;
    };

    ec = listen_one(*acceptors_.front(), endpoint, multi_acceptor);
#if defined(SO_REUSEPORT)
    if (ec && multi_acceptor) {
      CINATRA_LOG_WARNING
          << "SO_REUSEPORT unavailable, falling back to single acceptor, "
             "error: "
          << ec.message();
      acceptors_.clear();
      multi_acceptor = false;
      acceptors_.push_back(std::make_shared<tcp::acceptor>(executor_at(0)));
      ec = listen_one(*acceptors_.front(), endpoint, false);
    }
#endif
    if (ec) {
      CINATRA_LOG_ERROR << "listen port: " << port_
                        << " error: " << ec.message();
      close_listeners();
      return ec;
    }

    auto end_point = acceptors_.front()->local_endpoint(ec);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
    if (!ec && inject_failure_ == inject_failure::local_endpoint) {
      ec = std::make_error_code(std::errc::operation_not_supported);
    }
#endif
    if (ec) {
      CINATRA_LOG_ERROR << "get local endpoint port: " << port_
                        << " error: " << ec.message();
      close_listeners();
      return ec;
    }
    port_ = end_point.port();

    if (multi_acceptor) {
      tcp::endpoint extra_endpoint(endpoint.address(), port_);
      for (size_t i = 1; i < acceptor_count; ++i) {
        auto acceptor = std::make_shared<tcp::acceptor>(executor_at(i));
        ec = listen_one(*acceptor, extra_endpoint, true);
#ifdef INJECT_FOR_HTTP_SEVER_TEST
        if (!ec && inject_failure_ == inject_failure::extra_acceptor) {
          ec = std::make_error_code(std::errc::operation_not_supported);
        }
#endif
        if (ec) {
          CINATRA_LOG_WARNING
              << "create extra acceptor failed, continuing with "
              << acceptors_.size() << " acceptor(s), error: " << ec.message();
          std::error_code ignore_ec;
          acceptor->close(ignore_ec);
          break;
        }
        acceptors_.push_back(std::move(acceptor));
      }
    }

    CINATRA_LOG_INFO << "listen port " << port_ << " successfully";
    return {};
  }

  async_simple::coro::Lazy<std::error_code> accept(
      asio::ip::tcp::acceptor &acceptor,
      coro_io::ExecutorWrapper<> *fixed_executor = nullptr) {
    // .start() runs the coroutine body on the starting thread until the first
    // suspension. Every operation on the acceptor (the first async_accept as
    // well as the close dispatched by a concurrently failing listener or by
    // stop()) must be serialized through the acceptor's own executor. That is
    // the pool executor in pool mode and a strand in external-context mode,
    // where the caller may run the context on several threads.
    if (out_ctx_ != nullptr && !out_executor_) {
      out_executor_ = std::make_unique<coro_io::ExecutorWrapper<>>(
          out_ctx_->get_executor());
    }
    co_await coro_io::post([] {}, acceptor.get_executor());

    for (;;) {
      // Do not start another accept once shutdown has begun: close_acceptors
      // runs on this same executor, but a new accept could otherwise be
      // issued between the close and the loop exit.
      if (shutting_down_.load(std::memory_order_acquire)) {
        co_return std::error_code{};
      }

      coro_io::ExecutorWrapper<> *executor = fixed_executor;
      if (executor == nullptr && out_ctx_ == nullptr) {
        executor = pool_->get_executor();
      }
      else if (executor == nullptr) {
        if (!out_executor_) {
          out_executor_ = std::make_unique<coro_io::ExecutorWrapper<>>(
              out_ctx_->get_executor());
        }
        executor = out_executor_.get();
      }

      asio::ip::tcp::socket socket(executor->get_asio_executor());
      asio::error_code error;
#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (accept_exception_ &&
          (accept_exception_listener_ == static_cast<size_t>(-1) ||
           (accept_exception_listener_ < acceptors_.size() &&
            &acceptor == acceptors_[accept_exception_listener_].get()))) {
        throw std::runtime_error("injected accept failure");
      }
      if (accept_transient_errors_.fetch_sub(1, std::memory_order_relaxed) >
          0) {
        error = accept_transient_error_;
      }
      else {
        error = co_await coro_io::async_accept(acceptor, socket);
      }
#else
      error = co_await coro_io::async_accept(acceptor, socket);
#endif
      if (error) {
        CINATRA_LOG_INFO << "accept failed, error: " << error.message();
        if (error == asio::error::operation_aborted ||
            error == asio::error::bad_descriptor) {
          co_return error;
        }
        if (is_backoff_accept_error(error)) {
          // Backoff must resume on the acceptor's executor, not on whichever
          // connection executor this iteration picked.
          coro_io::period_timer backoff_timer(acceptor.get_executor());
          backoff_timer.expires_after(std::chrono::milliseconds(100));
          co_await backoff_timer.async_await();
          continue;
        }
        if (!is_retryable_accept_error(error)) {
          co_return error;
        }
        continue;
      }

      uint64_t conn_id = conn_id_.fetch_add(1, std::memory_order_relaxed) + 1;
      CINATRA_LOG_DEBUG << "new connection comming, id: " << conn_id;
      auto conn = std::make_shared<coro_http_connection>(
          executor, std::move(socket), router_);
      if (no_delay_) {
        conn->tcp_socket().set_option(asio::ip::tcp::no_delay(true));
      }
      conn->set_max_http_body_size(max_http_body_len_);
      conn->set_max_http_header_size(max_http_header_size_);
      if (need_shrink_every_time_) {
        conn->set_shrink_to_fit(true);
      }
      bool need_check;
      {
        std::scoped_lock lock(config_mtx_);
        need_check = need_check_;
      }
      if (need_check) {
        conn->set_check_timeout(true);
      }
      if (default_handler_) {
        conn->set_default_handler(default_handler_);
      }

#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (write_failed_forever_) {
        conn->set_write_failed_forever(write_failed_forever_);
      }
      if (read_failed_forever_) {
        conn->set_read_failed_forever(read_failed_forever_);
      }
#endif

#ifdef CINATRA_ENABLE_SSL
      if (use_ssl_) {
        conn->init_ssl(cert_file_, key_file_, passwd_);
      }
#endif

      conn->set_quit_callback(
          [this](const uint64_t &id) {
            std::scoped_lock lock(conn_mtx_);
            if (!connections_.empty())
              connections_.erase(id);
          },
          conn_id);

      {
        std::scoped_lock lock(conn_mtx_);
        connections_.emplace(conn_id, conn);
      }

      start_one(conn).via(conn->get_executor()).detach();
    }
  }

  async_simple::coro::Lazy<void> start_one(
      std::shared_ptr<coro_http_connection> conn) noexcept {
    co_await conn->start();
  }

  void close_acceptors_async() noexcept {
    for (auto &acceptor : acceptors_) {
      // Keep the listener alive until the queued close runs: the server may
      // be destroyed before an external io_context dispatches it.
      try {
        asio::dispatch(acceptor->get_executor(), [acceptor]() {
          asio::error_code ec;
          acceptor->cancel(ec);
          acceptor->close(ec);
        });
      } catch (...) {
        // Dispatching can allocate. Under extreme memory shortage the close
        // cannot be queued from this thread; with normal memory the promise
        // and stop() are guaranteed to make progress.
        try {
          CINATRA_LOG_ERROR << "failed to dispatch acceptor close";
        } catch (...) {
        }
      }
    }
  }

  void close_acceptor() {
    if (acceptors_.empty()) {
      return;
    }

    close_acceptors_async();
    acceptor_close_future_.wait();
  }

  void stop_impl() {
    shutting_down_.store(true, std::memory_order_release);
    stop_background_tasks();

    close_acceptor();

    {
      std::scoped_lock lock(conn_mtx_);
      for (auto &conn : connections_) {
        conn.second->close(false);
      }
      connections_.clear();
    }

    if (out_ctx_ == nullptr) {
      CINATRA_LOG_INFO << "wait for server's thread-pool finish all work.";
      pool_->stop();

      CINATRA_LOG_INFO << "server's thread-pool finished.";
      if (thd_.joinable()) {
        thd_.join();
      }
      CINATRA_LOG_INFO << "stop coro_http_server ok";
    }
    else {
      out_ctx_ = nullptr;
    }
  }

  void close_listeners() {
    asio::error_code ec;
    for (auto &acceptor : acceptors_) {
      acceptor->cancel(ec);
      acceptor->close(ec);
    }
    acceptors_.clear();
  }

  // After a failed io_context_pool::run() the pool is unusable: contexts are
  // stopped, work guards cleared and its run promise consumed. Recreate it
  // (and rebind the timers constructed against its executors) so the server
  // can be started again from idle.
  void reset_pool() {
    auto new_pool = std::make_unique<coro_io::io_context_pool>(thread_num_,
                                                               cpu_affinity_);
    auto *executor = new_pool->get_executor_at(0);
    auto new_check_timer = asio::steady_timer(executor->get_asio_executor());
    auto new_cache_timer = coro_io::period_timer(executor);
    {
      std::scoped_lock lock(config_mtx_);
      // Moving a timer destroys its previous implementation, which still
      // references the old context, so this must happen before the old pool
      // is destroyed.
      check_timer_ = std::move(new_check_timer);
      cache_refresh_timer_ = std::move(new_cache_timer);
    }
    pool_ = std::move(new_pool);
  }

  // Coroutine-based cache refresh loop.
  //
  // Sleep is done via period_timer::async_await() so stop() can wake it up
  // immediately by cancelling the timer. File reads are offloaded to the
  // global block executor via coro_io::post (non-blocking for the event loop),
  // with all data captured by value so 'this' is never accessed from that
  // thread. After the loop exits, the promise is fulfilled so stop() can
  // safely proceed to pool_->stop() without a use-after-free.
  async_simple::coro::Lazy<void> cache_refresh_loop() {
    // After the block-executor hop below the coroutine resumes on that
    // thread; hop back to the timer's executor before touching the timer or
    // any server member again.
    auto timer_executor = cache_refresh_timer_.get_executor();
    while (true) {
      std::chrono::steady_clock::duration interval;
      {
        std::scoped_lock lock(config_mtx_);
        interval = cache_refresh_interval_;
      }
      cache_refresh_timer_.expires_after(interval);
      bool timer_ok = co_await cache_refresh_timer_.async_await();
      if (!timer_ok || stop_timer_.load(std::memory_order_acquire)) {
        break;
      }

      std::error_code mtime_ec;
      auto dir_mtime = fs::last_write_time(static_dir_, mtime_ec);
      if (mtime_ec || dir_mtime == last_dir_mtime_) {
        continue;
      }
      last_dir_mtime_ = dir_mtime;

      // static_dir is a snapshot of static_dir_ on this coroutine's frame.
      // Captured by reference so the lambda closure holds only a pointer —
      // avoids GCC 11 coroutine-parameter SSO bitcopy bug where _M_p is
      // copied but not updated to the new frame location, causing ASAN
      // "attempting free on address which was not malloc()-ed" on destruction.
      // The frame stays alive for the entire co_await suspension, so the
      // reference is valid when the block executor reads it.
      std::string static_dir = static_dir_;
      size_t max_size;
      {
        std::scoped_lock lock(config_mtx_);
        max_size = max_cache_file_size_;
      }
      auto result = co_await coro_io::post([&static_dir, max_size]() {
        using FileMap = std::unordered_map<std::string, std::string>;
        auto new_cache = std::make_shared<FileMap>();
        std::error_code iter_ec;
        for (const auto &file :
             fs::recursive_directory_iterator(static_dir, iter_ec)) {
          if (iter_ec || file.is_directory()) {
            continue;
          }
          size_t filesize = fs::file_size(file, iter_ec);
          if (iter_ec || filesize > max_size) {
            continue;
          }
          std::ifstream ifs(file.path(), std::ios::binary);
          if (ifs.is_open()) {
            std::string content(filesize, '\0');
            ifs.read(content.data(), content.size());
            new_cache->emplace(file.path().string(), std::move(content));
          }
        }
        return new_cache;
      });
      co_await coro_io::post([] {}, timer_executor);

      // Re-check stop flag: stop() may have been called while we were
      // doing file reads on the block executor.
      if (stop_timer_.load(std::memory_order_acquire)) {
        break;
      }

      if (!result.hasError()) {
        store_file_cache(result.value());
      }
    }

    // Signal stop() that this coroutine has fully exited and will no
    // longer access any member of 'this'.
    cache_refresh_stopped_.set_value();
  }

  // Background tasks are created only from async_start(), once the listener
  // and the worker pool are known to be usable. An idle or failed server must
  // never own a timer whose handler references this. Startup runs before any
  // acceptor loop or runtime setter handler exists, so the timers are armed
  // directly here; later arms/cancels are delivered to the timer executor.
  void start_background_tasks() {
    bool arm_check;
    bool start_cache;
    {
      std::scoped_lock lock(config_mtx_);
      arm_check = need_check_;
      start_cache = cache_refresh_enabled_ && !cache_refresh_active_;
      if (start_cache) {
        cache_refresh_stopped_ = std::promise<void>{};
        cache_refresh_done_ = cache_refresh_stopped_.get_future().share();
        cache_refresh_active_ = true;
      }
    }
    if (arm_check) {
      start_check_timer();
    }
    if (start_cache) {
      cache_refresh_loop().start([](auto &&) {
      });
    }
  }

  // Runs `func` on the timer executor, but only while the server is alive.
  // A caller-owned io_context can outlive the server, so a handler left in
  // its queue must not touch members after destruction.
  template <typename Func>
  void post_guarded(const asio::any_io_executor &executor, Func func) {
    auto token = life_token_;
    asio::post(executor, [token, func = std::move(func)]() mutable {
      std::scoped_lock lock(token->mtx);
      if (!token->alive) {
        return;
      }
      func();
    });
  }

  // Cancels the timer on its own executor and waits until its cancellation
  // completions, and any handler queued behind them, have run. The executor
  // is single-threaded (pool mode) or a strand (external context), so the
  // marker posted after cancel() is ordered behind those completions.
  template <typename Timer>
  static void cancel_and_drain(const asio::any_io_executor &executor,
                               Timer &timer) {
    auto barrier = std::make_shared<std::promise<void>>();
    auto done = barrier->get_future();
    bool posted = false;
    try {
      asio::post(executor, [&timer, barrier, executor]() {
        try {
          timer.cancel();
        } catch (...) {
        }
        bool marker_posted = false;
        try {
          asio::post(executor, [barrier]() { barrier->set_value(); });
          marker_posted = true;
        } catch (...) {
        }
        if (!marker_posted) {
          barrier->set_value();
        }
      });
      posted = true;
    } catch (...) {
    }
    if (!posted) {
      // Extreme memory shortage: cancellation could not be queued at all.
      barrier->set_value();
    }
    done.wait();
  }

  // Cancels background timers and waits for their handlers and the cache
  // refresh coroutine to exit. Must be called while the owning executors are
  // still running: the coroutine resumes after the cancellation.
  void stop_background_tasks() {
    stop_timer_.store(true, std::memory_order_release);

    asio::any_io_executor check_executor;
    asio::any_io_executor cache_executor;
    std::shared_future<void> cache_done;
    {
      std::scoped_lock lock(config_mtx_);
      check_executor = check_timer_.get_executor();
      cache_executor = cache_refresh_timer_.get_executor();
      cache_done = cache_refresh_done_;
      cache_refresh_active_ = false;
    }

    cancel_and_drain(check_executor, check_timer_);
    cancel_and_drain(cache_executor, cache_refresh_timer_);
    if (cache_done.valid()) {
      cache_done.wait();
    }
  }

  void start_check_timer() {
    std::chrono::steady_clock::duration duration;
    {
      std::scoped_lock lock(config_mtx_);
      if (!need_check_) {
        return;
      }
      duration = check_duration_;
    }
    check_timer_.expires_after(duration);
    check_timer_.async_wait([this](auto ec) {
      if (ec || stop_timer_.load(std::memory_order_acquire)) {
        return;
      }

      check_timeout();
      start_check_timer();
    });
  }

  void check_timeout() {
    auto cur_time = std::chrono::system_clock::now();
    std::chrono::steady_clock::duration timeout;
    {
      std::scoped_lock lock(config_mtx_);
      timeout = timeout_duration_;
    }

    std::unordered_map<uint64_t, std::shared_ptr<coro_http_connection>> conns;

    {
      std::scoped_lock lock(conn_mtx_);
      for (auto it = connections_.begin();
           it != connections_.end();)  // no "++"!
      {
        if (cur_time - it->second->get_last_rwtime() > timeout) {
          it->second->close(false);
          connections_.erase(it++);
        }
        else {
          ++it;
        }
      }
    }
  }

  std::string build_multiple_range_header(size_t content_len) {
    std::string header_str = "HTTP/1.1 206 Partial Content\r\n";
    header_str.append("Content-Length: ");
    header_str.append(std::to_string(content_len)).append(CRCF);
    header_str.append("Content-Type: multipart/byteranges; boundary=");
    header_str.append(BOUNDARY).append(TWO_CRCF);
    return header_str;
  }

  std::vector<std::string> build_part_heads(auto &ranges, std::string_view mime,
                                            std::string_view file_size_str,
                                            size_t &content_len) {
    std::vector<std::string> multi_heads;
    for (auto [start, end] : ranges) {
      std::string part_header = "--";
      part_header.append(BOUNDARY).append(CRCF);
      part_header.append("Content-Type: ").append(mime).append(CRCF);
      part_header.append("Content-Range: ").append("bytes ");
      part_header.append(std::to_string(start))
          .append("-")
          .append(std::to_string(end))
          .append("/")
          .append(file_size_str)
          .append(TWO_CRCF);
      content_len += part_header.size();
      multi_heads.push_back(std::move(part_header));
      size_t part_size = end + 1 - start + CRCF.size();
      content_len += part_size;
    }
    content_len += (BOUNDARY.size() + 4);
    return multi_heads;
  }

  std::string build_range_header(std::string_view mime,
                                 std::string_view filename,
                                 std::string_view file_size_str,
                                 int status = 200,
                                 std::string_view content_range = "") {
    std::string header_str = "HTTP/1.1 ";
    header_str.append(std::to_string(status));
    header_str.append(" OK\r\nAccept-Ranges: bytes\r\n");
    if (!content_range.empty()) {
      header_str.append(content_range);
    }
    if (mime.starts_with("text/") || mime.starts_with("image/") ||
        mime == "application/javascript" ||
        mime == "application/x-javascript" || mime == "application/json") {
      header_str.append("Content-Disposition: inline\r\n");
    }
    else {
      header_str.append("Content-Disposition: attachment;filename=");
      header_str.append(std::filesystem::path(filename).filename().string())
          .append("\r\n");
    }
    header_str.append("Connection: keep-alive\r\n");
    header_str.append("Content-Type: ").append(mime).append("\r\n");
    header_str.append("Content-Length: ");
    header_str.append(file_size_str).append("\r\n\r\n");
    return header_str;
  }

  async_simple::coro::Lazy<bool> send_single_part(auto &in_file, auto &content,
                                                  auto &req, auto &resp,
                                                  size_t part_size,
                                                  std::string_view more = "") {
    while (true) {
      size_t read_size = (std::min)(part_size, chunked_size_);
      if (read_size == 0) {
        break;
      }
      auto [ec, size] = co_await in_file.async_read(content.data(), read_size);
      if (ec) {
        resp.set_status(status_type::no_content);
        co_await resp.get_conn()->reply();
        co_return false;
      }

      part_size -= read_size;

      bool r = true;
      if (more.empty()) {
        r = co_await req.get_conn()->write_data(
            std::string_view(content.data(), size));
      }
      else {
        std::array<asio::const_buffer, 2> arr{
            asio::buffer(content.data(), size), asio::buffer(more)};
        auto [ec, _] = co_await req.get_conn()->async_write(arr);
        if (ec) {
          r = false;
        }
      }

      if (!r) {
        co_return false;
      }
    }

    co_return true;
  }

  template <class T, class Pred>
  size_t erase_if(std::span<T> &sp, Pred p) {
    auto it = std::remove_if(sp.begin(), sp.end(), p);
    size_t count = sp.end() - it;
    sp = std::span<T>(sp.data(), sp.data() + count);
    return count;
  }

  int remove_result_headers(resp_data &result, std::string_view value) {
    bool r = false;
    return erase_if(result.resp_headers, [&](http_header &header) {
      if (r) {
        return false;
      }

      r = (header.value.find(value) != std::string_view::npos);

      return r;
    });
  }

  void handle_response_header(resp_data &result, std::string &length) {
    int r = remove_result_headers(result, "chunked");
    if (r == 0) {
      r = remove_result_headers(result, "multipart/form-data");
      if (r) {
        length = std::to_string(result.resp_body.size());
        for (auto &[key, val] : result.resp_headers) {
          if (key == "Content-Length") {
            val = length;
            break;
          }
        }
      }
    }
  }

  async_simple::coro::Lazy<void> reply(coro_http_client &client,
                                       std::string_view host,
                                       coro_http_request &req,
                                       coro_http_response &response) {
    uri_t uri;
    std::string proxy_host;

    if (host.find("//") == std::string_view::npos) {
      proxy_host.append("http://").append(host);
      uri.parse_from(proxy_host.data());
    }
    else {
      uri.parse_from(host.data());
    }
    std::unordered_map<std::string, std::string> req_headers{
        {"Host", std::string(uri.host)}};
    std::string forwarded_headers;
    for (auto &[name, value] : req.get_headers()) {
      if (iequal0(name, "Host")) {
        continue;
      }
      if (iequal0(name, "Content-Length")) {
        req_headers["Content-Length"].assign(value);
        continue;
      }
      if (iequal0(name, "Connection")) {
        req_headers["Connection"].assign(value);
        continue;
      }
      forwarded_headers.append(name).append(": ").append(value).append(CRCF);
    }

    auto ctx = req_context<std::string_view>{
        .req_header = std::move(forwarded_headers), .content = req.get_body()};
    auto result = co_await client.async_request(
        req.full_url(), method_type(req.get_method()), std::move(ctx),
        std::move(req_headers));

    std::string length;
    handle_response_header(result, length);
    response.add_header_span(result.resp_headers);

    response.set_status_and_content_view(
        static_cast<status_type>(result.status), result.resp_body);
    co_await response.get_conn()->reply();
    response.set_delay(true);
  }

  void init_address(std::string address) {
#if __has_include(<ylt/easylog.hpp>)
    easylog::logger<>::instance();  // init easylog singleton to make sure
                                    // server destruct before easylog.
#endif

    if (size_t pos = address.find(':'); pos != std::string::npos) {
      auto port_sv = std::string_view(address).substr(pos + 1);

      uint16_t port;
      auto [ptr, ec] = std::from_chars(
          port_sv.data(), port_sv.data() + port_sv.size(), port, 10);
      if (ec != std::errc{}) {
        address_ = std::move(address);
        return;
      }

      port_ = port;
      address = address.substr(0, pos);
    }

    address_ = std::move(address);
  }

 private:
  void store_file_cache(file_cache_ptr cache) {
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
    file_cache_.store(std::move(cache));
#else
    std::atomic_store(&file_cache_, std::move(cache));
#endif
  }

  file_cache_ptr load_file_cache() const {
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
    return file_cache_.load();
#else
    return std::atomic_load(&file_cache_);
#endif
  }

  std::unique_ptr<coro_io::io_context_pool> pool_;
  asio::io_context *out_ctx_ = nullptr;
  std::optional<out_strand_type> out_strand_;
  std::unique_ptr<coro_io::ExecutorWrapper<>> out_executor_ = nullptr;
  uint16_t port_;
  std::string address_;
  size_t thread_num_ = 1;
  bool cpu_affinity_ = false;
  bool reuse_port_ = true;
  std::error_code errc_ = {};
  mutable std::mutex errc_mtx_;
  std::mutex life_mtx_;
  std::condition_variable life_cv_;
  life_state life_ = life_state::idle;
  std::shared_ptr<life_token> life_token_ = std::make_shared<life_token>();
  std::atomic<bool> shutting_down_{false};
  std::vector<std::shared_ptr<asio::ip::tcp::acceptor>> acceptors_;
  std::thread thd_;
  std::promise<void> acceptor_close_waiter_;
  std::future<void> acceptor_close_future_ =
      acceptor_close_waiter_.get_future();
  bool no_delay_ = true;

  std::atomic<uint64_t> conn_id_{0};
  std::unordered_map<uint64_t, std::shared_ptr<coro_http_connection>>
      connections_;
  std::mutex conn_mtx_;
  // Guards the configuration fields read by timer handlers and the cache
  // refresh coroutine, and the cache refresh bookkeeping below. Not held
  // while waiting on the refresh coroutine.
  std::mutex config_mtx_;
  std::chrono::steady_clock::duration check_duration_ =
      std::chrono::seconds(15);
  std::chrono::steady_clock::duration timeout_duration_{};
  asio::steady_timer check_timer_;
  bool need_check_ = false;
  std::atomic<bool> stop_timer_ = false;

  std::string static_dir_router_path_ = "";
  std::string static_dir_ = "";
  size_t chunked_size_ = 1024 * 10;

#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
  std::atomic<file_cache_ptr> file_cache_;
#else
  file_cache_ptr file_cache_;
#endif
  coro_io::period_timer cache_refresh_timer_;
  std::chrono::steady_clock::duration cache_refresh_interval_ =
      std::chrono::seconds(5);
  size_t max_cache_file_size_ = 3 * 1024 * 1024;
  fs::file_time_type last_dir_mtime_{};
  bool cache_refresh_enabled_ = false;
  bool cache_refresh_active_ = false;
  std::promise<void> cache_refresh_stopped_;
  std::shared_future<void> cache_refresh_done_;
  file_resp_format_type format_type_ = file_resp_format_type::range;
#ifdef CINATRA_ENABLE_SSL
  std::string cert_file_;
  std::string key_file_;
  std::string passwd_;
  bool use_ssl_ = false;
#endif
  coro_http_router router_;
  bool need_shrink_every_time_ = false;
  std::function<async_simple::coro::Lazy<void>(coro_http_request &,
                                               coro_http_response &)>
      default_handler_ = nullptr;
  int64_t max_http_body_len_ = MAX_HTTP_BODY_SIZE;
  size_t max_http_header_size_ = 8 * 1024;
#ifdef INJECT_FOR_HTTP_SEVER_TEST
  bool write_failed_forever_ = false;
  bool read_failed_forever_ = false;
  inject_failure inject_failure_ = inject_failure::none;
  bool accept_exception_ = false;
  size_t accept_exception_listener_ = static_cast<size_t>(-1);
  std::atomic<int> accept_transient_errors_{0};
  asio::error_code accept_transient_error_ = asio::error::would_block;
  size_t inject_launch_failure_index_ = static_cast<size_t>(-1);
  bool inject_completion_failure_ = false;
  std::chrono::milliseconds inject_launch_delay_{0};
#endif
};

using http_server = coro_http_server;
using request = coro_http_request;
using response = coro_http_response;
}  // namespace cinatra
