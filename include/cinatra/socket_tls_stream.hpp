#pragma once

#include <array>
#include <asio/compose.hpp>
#include <asio/defer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl.hpp>
#include <cerrno>
#include <memory>
#include <openssl/err.h>
#include <sys/socket.h>
#include <system_error>
#include <type_traits>

namespace cinatra::detail {

class socket_tls_stream {
 public:
  using executor_type = asio::ip::tcp::socket::executor_type;
  using lowest_layer_type = asio::ip::tcp::socket::lowest_layer_type;

  socket_tls_stream(asio::ip::tcp::socket &socket, asio::ssl::context &context)
      : socket_(socket), ssl_(SSL_new(context.native_handle()), SSL_free) {
    if (!ssl_) {
      throw_ssl_error();
    }
    const auto *method = socket_bio_method();
    BIO *bio = method ? BIO_new(method) : nullptr;
    if (!bio) {
      throw_ssl_error();
    }
    BIO_set_data(bio, &socket);
    BIO_set_init(bio, 1);
    SSL_set_bio(ssl_.get(), bio, bio);
    SSL_set_accept_state(ssl_.get());
    SSL_set_mode(ssl_.get(), SSL_MODE_ENABLE_PARTIAL_WRITE |
                                 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                 SSL_MODE_RELEASE_BUFFERS);
    SSL_set_read_ahead(ssl_.get(), 1);
  }

  executor_type get_executor() noexcept { return socket_.get_executor(); }
  lowest_layer_type &lowest_layer() noexcept { return socket_.lowest_layer(); }

  template <typename Token>
  auto async_handshake(asio::ssl::stream_base::handshake_type, Token &&token) {
    return asio::async_compose<Token, void(std::error_code)>(
        operation<kind::handshake>{*this}, token, socket_);
  }

  template <typename Buffers, typename Token>
  auto async_read_some(const Buffers &buffers, Token &&token) {
    return asio::async_compose<Token, void(std::error_code, size_t)>(
        operation<kind::read, Buffers>{*this, buffers}, token, socket_);
  }

  template <typename Buffers, typename Token>
  auto async_write_some(const Buffers &buffers, Token &&token) {
    return asio::async_compose<Token, void(std::error_code, size_t)>(
        operation<kind::write, Buffers>{*this, buffers}, token, socket_);
  }

 private:
  [[noreturn]] static void throw_ssl_error() {
    throw std::system_error(
        std::error_code(static_cast<int>(ERR_get_error()),
                        asio::error::get_ssl_category()));
  }

  static const BIO_METHOD *socket_bio_method() {
    static const auto method = [] {
      std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> value(
          BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK,
                       "cinatra TCP socket"),
          BIO_meth_free);
      if (value &&
          (!BIO_meth_set_read(value.get(), &read_socket) ||
           !BIO_meth_set_write(value.get(), &write_socket) ||
           !BIO_meth_set_ctrl(value.get(), [](BIO *, int cmd, long, void *) {
             return cmd == BIO_CTRL_FLUSH ? 1L : 0L;
           }))) {
        value.reset();
      }
      return value;
    }();
    return method.get();
  }

  static int read_socket(BIO *bio, char *data, int size) {
    BIO_clear_retry_flags(bio);
    auto &socket = *static_cast<asio::ip::tcp::socket *>(BIO_get_data(bio));
    ssize_t result;
    do {
      result = ::recv(socket.native_handle(), data, size, MSG_DONTWAIT);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      BIO_set_retry_read(bio);
    }
    return static_cast<int>(result);
  }

  static int write_socket(BIO *bio, const char *data, int size) {
    BIO_clear_retry_flags(bio);
    auto &socket = *static_cast<asio::ip::tcp::socket *>(BIO_get_data(bio));
    ssize_t result;
    do {
      result = ::send(socket.native_handle(), data, size,
                      MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      BIO_set_retry_write(bio);
    }
    return static_cast<int>(result);
  }

  static std::error_code ssl_error(int error, int system_error) {
    const auto ssl_error = ERR_get_error();
    if (ssl_error != 0) {
      if (ERR_GET_REASON(ssl_error) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
        return asio::ssl::error::stream_truncated;
      }
      return {static_cast<int>(ssl_error), asio::error::get_ssl_category()};
    }
    if (error == SSL_ERROR_ZERO_RETURN) {
      return asio::error::eof;
    }
    if (error == SSL_ERROR_SYSCALL) {
      if (system_error != 0) {
        return {system_error, std::system_category()};
      }
      return asio::ssl::error::stream_truncated;
    }
    return asio::ssl::error::unexpected_result;
  }

  enum class kind { handshake, read, write };
  struct no_buffers {};

  template <kind Kind, typename Buffers = no_buffers>
  struct operation {
    socket_tls_stream &stream;
    [[no_unique_address]] Buffers buffers{};
    enum class phase { start, running, complete };
    phase state = phase::start;
    std::error_code result_error;
    size_t result_size = 0;

    template <typename Self>
    void operator()(Self &self, std::error_code ec = {}) {
      const bool initial = state == phase::start;
      if (state == phase::complete) {
        complete(self, result_error, result_size);
        return;
      }
      if (initial) {
        state = phase::running;
        const auto executor = stream.get_executor();
        const auto *context_executor =
            executor.target<asio::io_context::executor_type>();
        if (!context_executor || !context_executor->running_in_this_thread()) {
          asio::post(executor, std::move(self));
          return;
        }
      }
      size_t transferred = 0;
      if (!ec) {
        if (!stream.socket_.is_open()) {
          ec = asio::error::bad_descriptor;
        }
        else {
          ERR_clear_error();
          errno = 0;
          ::SSL *ssl = stream.ssl_.get();
          const int result = perform(ssl, transferred);
          const int system_error = errno;
          if (result != 1) {
            const int error = SSL_get_error(ssl, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
              auto wait = error == SSL_ERROR_WANT_READ
                              ? asio::ip::tcp::socket::wait_read
                              : asio::ip::tcp::socket::wait_write;
              stream.socket_.async_wait(wait, std::move(self));
              return;
            }
            ec = ssl_error(error, system_error);
          }
        }
      }
      if (initial) {
        // A TLS call that finishes during initiation still completes
        // asynchronously. The wait path already guarantees that.
        state = phase::complete;
        result_error = ec;
        result_size = transferred;
        if constexpr (Kind == kind::write) {
          asio::defer(stream.get_executor(), std::move(self));
        }
        else {
          asio::post(stream.get_executor(), std::move(self));
        }
        return;
      }
      complete(self, ec, transferred);
    }

    template <typename Self>
    static void complete(Self &self, std::error_code ec, size_t transferred) {
      if constexpr (Kind == kind::handshake) {
        self.complete(ec);
      }
      else {
        self.complete(ec, transferred);
      }
    }

    int perform(::SSL *ssl, size_t &transferred) {
      if constexpr (Kind == kind::handshake) {
        return SSL_do_handshake(ssl);
      }
      else if constexpr (Kind == kind::read) {
        for (auto it = asio::buffer_sequence_begin(buffers),
                  end = asio::buffer_sequence_end(buffers);
             it != end; ++it) {
          auto buffer = asio::mutable_buffer(*it);
          if (buffer.size() != 0) {
            return SSL_read_ex(ssl, buffer.data(), buffer.size(), &transferred);
          }
        }
        return 1;
      }
      else {
        auto it = asio::buffer_sequence_begin(buffers);
        const auto end = asio::buffer_sequence_end(buffers);
        while (it != end && asio::const_buffer(*it).size() == 0) {
          ++it;
        }
        if (it == end) {
          return 1;
        }
        const auto first = asio::const_buffer(*it);
        if (++it == end || first.size() >= 16 * 1024) {
          return SSL_write_ex(ssl, first.data(), first.size(), &transferred);
        }
        std::array<unsigned char, 16 * 1024> record;
        const auto used = asio::buffer_copy(asio::buffer(record), buffers);
        return SSL_write_ex(ssl, record.data(), used, &transferred);
      }
    }
  };

  asio::ip::tcp::socket &socket_;
  std::unique_ptr<::SSL, decltype(&SSL_free)> ssl_;
};

}  // namespace cinatra::detail
