#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>

#include "asio/io_context.hpp"
#include "cinatra/cinatra_log_wrapper.hpp"
#include "utils.hpp"

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
  client(asio::io_context& ctx)
      : io_context_(ctx), socket_(ctx), resolver_(ctx) {}

  ~client() { close(); }

  bool connect(std::string_view host, std::string_view port) {
    auto endpoints = resolver_.resolve(host, port);
    std::error_code ec;
    asio::connect(socket_, endpoints, ec);
    if (ec) {
      std::cerr << "connect host: " << host << ":" << port << " failed\n";
      return false;
    }

    if (upgrade_to_ssl()) {
      return false;
    }

    // 读取服务器欢迎信息
    return read_response();
  }

  bool send_email(const email_data& email) {
    // 1. EHLO 命令
    bool r = send_command("EHLO localhost");
    if (!r) {
      return false;
    }
    // 2. 认证
    r = send_command("AUTH LOGIN");
    if (!r) {
      return false;
    }
    r = send_command(cinatra::base64_encode(email.user_name));
    if (!r) {
      return false;
    }
    r = send_command(cinatra::base64_encode(email.auth_pwd));
    if (!r) {
      return false;
    }

    // 3. 设置发件人
    r = send_command("MAIL FROM: <" + email.from_email + ">");
    if (!r) {
      return false;
    }

    // 4. 设置收件人
    for (const auto& recipient : email.to_email) {
      r = send_command("RCPT TO: <" + recipient + ">");
      if (!r) {
        return false;
      }
    }

    // 5. 发送数据
    r = send_command("DATA");
    if (!r) {
      return false;
    }

    // 6. 邮件内容
    std::ostringstream email_content;

    // 邮件头
    email_content << "From: " << email.from_email << "\r\n";

    // 显示所有收件人
    email_content << "To: ";
    for (size_t i = 0; i < email.to_email.size(); ++i) {
      if (i > 0)
        email_content << ", ";
      email_content << email.to_email[i];
    }
    email_content << "\r\n";

    email_content << "Subject: " << email.subject << "\r\n";
    email_content << "\r\n";

    // 邮件正文
    email_content << email.text << "\r\n";

    // 发送
    r = send_raw(email_content.str());
    if (!r) {
      return false;
    }
    r = send_raw("\r\n.\r\n");
    if (!r) {
      return false;
    }

    // 8. 退出
    r = send_command("QUIT");
    return r;
  }

 private:
  bool read_response() {
    std::error_code ec;
    size_t n = asio::read_until(*ssl_socket_, response_, "\r\n", ec);
    if (ec) {
      CINATRA_LOG_ERROR << "网络错误，读失败, " << ec.message();
      return false;
    }
    if (n < 3) {
      CINATRA_LOG_ERROR << "无效的服务器响应";
      response_.consume(response_.size());
      return false;
    }

    std::string_view content((const char*)response_.data().data(), n);
    response_.consume(response_.size());

    char code = content[0];
    if (code != '2' && code != '3') {
      CINATRA_LOG_ERROR << "SMTP 错误: " << content;
      return false;
    }

    CINATRA_LOG_DEBUG << content;

    return true;
  }

  bool send_raw(std::string cmd) {
    std::error_code ec;
    cmd.append("\r\n");
    asio::write(*ssl_socket_, asio::buffer(cmd), ec);
    if (ec) {
      return false;
    }

    return true;
  }

  bool send_command(std::string cmd) {
    bool r = send_raw(std::move(cmd));
    if (!r) {
      return false;
    }

    return read_response();
  }

  bool upgrade_to_ssl() {
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

    std::error_code ec;
    ssl_socket_->handshake(asio::ssl::stream_base::client, ec);
    if (ec) {
      CINATRA_LOG_ERROR << "SSL handshake error: " << ec.message();
    }

    return false;
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
  asio::io_context& io_context_;
  asio::ip::tcp::socket socket_;
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_socket_;
  asio::ip::tcp::resolver resolver_;

  asio::streambuf response_;
};

static inline auto get_smtp_client(asio::io_context& ctx) {
  return smtp::client(ctx);
}

}  // namespace cinatra::smtp
