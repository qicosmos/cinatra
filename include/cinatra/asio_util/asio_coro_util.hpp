#pragma once
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <chrono>
#include <deque>

#ifdef CINATRA_ENABLE_SSL
#include <asio/ssl.hpp>
#endif

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

namespace asio_util {

template <typename ExecutorImpl = asio::io_context::executor_type>
class ExecutorWrapper : public async_simple::Executor {
 private:
  ExecutorImpl executor_;

 public:
  ExecutorWrapper(ExecutorImpl executor) : executor_(executor) {}

  using context_t = std::remove_cvref_t<decltype(executor_.context())>;

  virtual bool schedule(Func func) override {
    asio::post(executor_, std::move(func));
    return true;
  }

  virtual bool checkin(Func func, void *ctx) override {
    using context_t = std::remove_cvref_t<decltype(executor_.context())>;
    asio::post(*(context_t *)ctx, func);
    return true;
  }
  virtual void *checkout() override { return &executor_.context(); }

  bool currentThreadInExecutor() const override { return false; }

  async_simple::ExecutorStat stat() const override { return {}; }

  context_t &context() { return executor_.context(); }

  auto get_executor() { return executor_; }

 private:
  void schedule(Func func, Duration dur) override {
    auto timer = std::make_shared<asio::steady_timer>(executor_, dur);
    timer->async_wait([fn = std::move(func), timer](auto ec) {
      fn();
    });
  }
};

template <typename Arg, typename Derived>
class callback_awaitor_base {
 private:
  template <typename Op>
  class callback_awaitor_impl {
   public:
    callback_awaitor_impl(Derived &awaitor, const Op &op) noexcept
        : awaitor(awaitor), op(op) {}
    constexpr bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) noexcept {
      awaitor.coro_ = handle;
      op(awaitor_handler{awaitor});
    }
    auto coAwait(async_simple::Executor *executor) const noexcept {
      return *this;
    }
    decltype(auto) await_resume() noexcept {
      if constexpr (std::is_void_v<Arg>) {
        return;
      }
      else {
        return std::move(awaitor.arg_);
      }
    }

   private:
    Derived &awaitor;
    const Op &op;
  };

 public:
  class awaitor_handler {
   public:
    awaitor_handler(Derived &obj) : obj(obj) {}
    template <typename... Args>
    void set_value_then_resume(Args &&...args) const {
      set_value(std::forward<Args>(args)...);
      resume();
    }
    template <typename... Args>
    void set_value(Args &&...args) const {
      if constexpr (!std::is_void_v<Arg>) {
        obj.arg_ = {std::forward<Args>(args)...};
      }
    }
    void resume() const { obj.coro_.resume(); }

   private:
    Derived &obj;
  };
  template <typename Op>
  callback_awaitor_impl<Op> await_resume(const Op &op) noexcept {
    return callback_awaitor_impl<Op>{static_cast<Derived &>(*this), op};
  }

 private:
  std::coroutine_handle<> coro_;
};

template <typename Arg>
class callback_awaitor
    : public callback_awaitor_base<Arg, callback_awaitor<Arg>> {
  friend class callback_awaitor_base<Arg, callback_awaitor<Arg>>;

 private:
  Arg arg_;
};

template <>
class callback_awaitor<void>
    : public callback_awaitor_base<void, callback_awaitor<void>> {};

inline async_simple::coro::Lazy<std::error_code> async_accept(
    asio::ip::tcp::acceptor &acceptor, asio::ip::tcp::socket &socket) noexcept {
  callback_awaitor<std::error_code> awaitor;

  co_return co_await awaitor.await_resume([&](auto handler) {
    acceptor.async_accept(socket, [&, handler](const auto &ec) mutable {
      handler.set_value_then_resume(ec);
    });
  });
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<std::error_code, size_t>>
async_read_some(Socket &socket, AsioBuffer &&buffer) noexcept {
  callback_awaitor<std::pair<std::error_code, size_t>> awaitor;
  co_return co_await awaitor.await_resume([&](auto handler) {
    socket.async_read_some(buffer, [&, handler](const auto &ec, auto size) {
      handler.set_value_then_resume(ec, size);
    });
  });
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
    Socket &socket, AsioBuffer &&buffer) noexcept {
  callback_awaitor<std::pair<std::error_code, size_t>> awaitor;
  co_return co_await awaitor.await_resume([&](auto handler) {
    asio::async_read(socket, buffer, [&, handler](const auto &ec, auto size) {
      handler.set_value_then_resume(ec, size);
    });
  });
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
    Socket &socket, AsioBuffer &buffer, size_t size_to_read) noexcept {
  callback_awaitor<std::pair<std::error_code, size_t>> awaitor;
  co_return co_await awaitor.await_resume([&](auto handler) {
    asio::async_read(socket, buffer, asio::transfer_exactly(size_to_read),
                     [&, handler](const auto &ec, auto size) {
                       handler.set_value_then_resume(ec, size);
                     });
  });
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<std::error_code, size_t>>
async_read_until(Socket &socket, AsioBuffer &buffer,
                 asio::string_view delim) noexcept {
  callback_awaitor<std::pair<std::error_code, size_t>> awaitor;
  co_return co_await awaitor.await_resume([&](auto handler) {
    asio::async_read_until(socket, buffer, delim,
                           [&, handler](const auto &ec, auto size) {
                             handler.set_value_then_resume(ec, size);
                           });
  });
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_write(
    Socket &socket, AsioBuffer &&buffer) noexcept {
  callback_awaitor<std::pair<std::error_code, size_t>> awaitor;
  co_return co_await awaitor.await_resume([&](auto handler) {
    asio::async_write(socket, buffer, [&, handler](const auto &ec, auto size) {
      handler.set_value_then_resume(ec, size);
    });
  });
}

template <typename executor_t>
inline async_simple::coro::Lazy<std::error_code> async_connect(
    const executor_t &executor, asio::ip::tcp::socket &socket,
    const std::string &host, const std::string &port) noexcept {
  callback_awaitor<std::error_code> awaitor;
  asio::ip::tcp::resolver resolver(executor);
  asio::ip::tcp::resolver::iterator iterator;
  auto ec = co_await awaitor.await_resume([&](auto handler) {
    resolver.async_resolve(host, port, [&, handler](auto ec, auto it) {
      iterator = it;
      handler.set_value_then_resume(ec);
    });
  });

  if (ec) {
    co_return ec;
  }

  co_return co_await awaitor.await_resume([&](auto handler) {
    asio::async_connect(socket, iterator,
                        [&, handler](const auto &ec, const auto &) mutable {
                          handler.set_value_then_resume(ec);
                        });
  });
}

template <typename Socket>
inline async_simple::coro::Lazy<void> async_close(Socket &socket) noexcept {
  callback_awaitor<void> awaitor;
  auto &executor = socket.get_executor();
  co_return co_await awaitor.await_resume([&](auto handler) {
    asio::post(executor, [&, handler]() {
      asio::error_code ignored_ec;
      socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored_ec);
      socket.close(ignored_ec);
      handler.resume();
    });
  });
}

#ifdef CINATRA_ENABLE_SSL
inline async_simple::coro::Lazy<std::error_code> async_handshake(
    auto &ssl_stream, asio::ssl::stream_base::handshake_type type) noexcept {
  callback_awaitor<std::error_code> awaitor;
  co_return co_await awaitor.await_resume([&](auto handler) {
    ssl_stream->async_handshake(type, [&, handler](const auto &ec) {
      handler.set_value_then_resume(ec);
    });
  });
}
#endif

class period_timer : public asio::steady_timer {
 public:
  template <typename executor_t>
  period_timer(const executor_t &executor) : asio::steady_timer(executor) {}
  template <typename executor_t, typename Rep, typename Period>
  period_timer(const executor_t &executor,
               const std::chrono::duration<Rep, Period> &timeout_duration)
      : asio::steady_timer(executor, timeout_duration) {}
  async_simple::coro::Lazy<bool> async_await() noexcept {
    callback_awaitor<bool> awaitor;
    co_return co_await awaitor.await_resume([&](auto handler) {
      this->async_wait([&, handler](const auto &ec) {
        handler.set_value_then_resume(!ec);
      });
    });
  }
};

}  // namespace asio_util
