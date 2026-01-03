#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>

#include "cinatra/cinatra_log_wrapper.hpp"
#include "utils.hpp"
#include "ylt/coro_io/coro_io.hpp"

namespace cinatra::smtp {
struct email_data {
  std::string from_email;
  std::vector<std::string> to_email;
  std::string subject;
  std::string text;
  std::string user_name;
  std::string auth_pwd;
  std::string filepath;
};

class client {
 public:
  client(coro_io::ExecutorWrapper<>* ctx)
      : io_context_(ctx), socket_(ctx->get_asio_executor()) {}

  ~client() { close(); }

  async_simple::coro::Lazy<bool> connect(const std::string& host,
                                         const std::string& port) {
    std::error_code ec =
        co_await coro_io::async_connect(io_context_, socket_, host, port);

    if (ec) {
      std::cerr << "connect host: " << host << ":" << port << " failed\n";
      co_return false;
    }

    if (!(co_await upgrade_to_ssl())) {
      co_return false;
    }

    // 读取服务器欢迎信息
    co_return co_await read_response();
  }

  async_simple::coro::Lazy<bool> send_email(const email_data& email) {
    // 1. EHLO 命令
    bool r = co_await send_command("EHLO localhost");
    if (!r) {
      co_return false;
    }
    // 2. 认证
    r = co_await send_command("AUTH LOGIN");
    if (!r) {
      co_return false;
    }
    r = co_await send_command(cinatra::base64_encode(email.user_name));
    if (!r) {
      co_return false;
    }
    r = co_await send_command(cinatra::base64_encode(email.auth_pwd));
    if (!r) {
      co_return false;
    }

    // 3. 设置发件人
    r = co_await send_command("MAIL FROM: <" + email.from_email + ">");
    if (!r) {
      co_return false;
    }

    // 4. 设置收件人
    for (const auto& recipient : email.to_email) {
      r = co_await send_command("RCPT TO: <" + recipient + ">");
      if (!r) {
        co_return false;
      }
    }

    // 5. 发送数据
    r = co_await send_command("DATA");
    if (!r) {
      co_return false;
    }

    // 6. 邮件内容
    std::string email_content;

    // 邮件头
    email_content.append("From: ").append(email.from_email).append("\r\n");

    // 显示所有收件人
    email_content.append("To: ");
    for (size_t i = 0; i < email.to_email.size(); ++i) {
      if (i > 0)
        email_content.append(", ");
      email_content.append(email.to_email[i]);
    }
    email_content.append("\r\n");

    email_content.append("Subject: ").append(email.subject).append("\r\n");
    email_content.append("\r\n");

    // 邮件正文
    email_content.append(email.text).append("\r\n");

    // 发送邮件正文
    r = co_await send_raw(std::move(email_content));
    if (!r) {
      co_return false;
    }
    // 7. 邮件结束
    r = co_await send_raw("\r\n.\r\n");
    if (!r) {
      co_return false;
    }

    // 8. 退出
    r = co_await send_command("QUIT");
    co_return r;
  }

 private:
  async_simple::coro::Lazy<bool> read_response() {
    auto [ec, n] =
        co_await coro_io::async_read_until(*ssl_socket_, response_, "\r\n");
    if (ec) {
      CINATRA_LOG_ERROR << "网络错误，读失败, " << ec.message();
      co_return false;
    }
    if (n < 3) {
      CINATRA_LOG_ERROR << "无效的服务器响应";
      response_.consume(response_.size());
      co_return false;
    }

    std::string_view content((const char*)response_.data().data(), n);
    response_.consume(response_.size());

    char code = content[0];
    if (code != '2' && code != '3') {
      CINATRA_LOG_ERROR << "SMTP 错误: " << content;
      co_return false;
    }

    CINATRA_LOG_DEBUG << content;

    co_return true;
  }

  async_simple::coro::Lazy<bool> send_raw(std::string cmd) {
    auto [ec, _] =
        co_await coro_io::async_write(*ssl_socket_, asio::buffer(cmd));
    if (ec) {
      CINATRA_LOG_ERROR << ec.message();
      co_return false;
    }
    co_return true;
  }

  async_simple::coro::Lazy<bool> send_command(std::string cmd) {
    cmd.append("\r\n");
    bool r = co_await send_raw(std::move(cmd));
    if (!r) {
      co_return false;
    }

    co_return co_await read_response();
  }

  async_simple::coro::Lazy<bool> upgrade_to_ssl() {
    asio::ssl::context ctx(asio::ssl::context::sslv23);
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ctx.verify_fail_if_no_peer_cert);

    ssl_socket_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(
        socket_, ctx);
    ssl_socket_->set_verify_mode(asio::ssl::verify_none);
    ssl_socket_->set_verify_callback([](auto preverified, auto& ctx) {
      char subject_name[256];
      X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
      X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);

      return preverified;
    });

    std::error_code ec = co_await coro_io::async_handshake(
        ssl_socket_, asio::ssl::stream_base::client);
    if (ec) {
      CINATRA_LOG_ERROR << "SSL handshake error: " << ec.message();
      co_return false;
    }

    co_return true;
  }

  void close() {
    if (ssl_socket_ == nullptr) {
      return;
    }

    std::error_code ignore_ec;
    ssl_socket_->shutdown(ignore_ec);
    ssl_socket_ = nullptr;

    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket_.close(ignore_ec);
  }

 private:
  coro_io::ExecutorWrapper<>* io_context_;
  asio::ip::tcp::socket socket_;
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_socket_;

  asio::streambuf response_;
};

static inline smtp::client get_smtp_client(coro_io::ExecutorWrapper<>* ctx) {
  return smtp::client(ctx);
}

}  // namespace cinatra::smtp
