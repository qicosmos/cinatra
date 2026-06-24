#include <any>
#include <asio/io_context.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "cinatra/coro_http_server.hpp"
#include "cinatra/http2/h2_connection.hpp"

namespace {

uint16_t parse_port(const char* text) {
  auto value = std::strtoul(text, nullptr, 10);
  if (value == 0 || value > 65535) {
    throw std::runtime_error("invalid port");
  }
  return static_cast<uint16_t>(value);
}

int parse_duration_seconds(const char* text) {
  auto value = std::strtol(text, nullptr, 10);
  if (value <= 0) {
    throw std::runtime_error("invalid duration");
  }
  return static_cast<int>(value);
}

size_t parse_thread_count(const char* text) {
  auto value = std::strtoul(text, nullptr, 10);
  if (value == 0) {
    throw std::runtime_error("invalid thread count");
  }
  return static_cast<size_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
#ifndef CINATRA_ENABLE_SSL
  std::cerr << "http2_conformance_server requires CINATRA_ENABLE_SSL\n";
  return 1;
#else
  uint16_t port = 8080;
  int duration_seconds = 300;
  size_t thread_count = std::thread::hardware_concurrency();
  if (thread_count == 0) {
    thread_count = 1;
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = parse_port(argv[++i]);
    }
    else if (arg == "--duration" && i + 1 < argc) {
      duration_seconds = parse_duration_seconds(argv[++i]);
    }
    else if (arg == "--threads" && i + 1 < argc) {
      thread_count = parse_thread_count(argv[++i]);
    }
    else if (arg == "--single-thread") {
      thread_count = 1;
    }
    else if (arg == "--tls") {
      // Kept for compatibility; this conformance server is TLS-only.
    }
    else {
      std::cerr << "usage: http2_conformance_server [--port N] "
                   "[--duration S] [--threads N] [--single-thread] [--tls]\n";
      return 2;
    }
  }

  cinatra::coro_http_server server(thread_count, port);
  server.set_http2_mode(cinatra::http2_mode::required);
  server.set_enable_http2_connect_protocol(true);
  server.set_default_handler(
      [](cinatra::coro_http_request& req,
         cinatra::coro_http_response& resp) -> async_simple::coro::Lazy<void> {
        bool needs_flow_control_probe_body = false;
        if (auto metadata = req.get_user_data(); metadata.has_value()) {
          if (auto* http2_metadata =
                  std::any_cast<cinatra::http2::common_request_metadata>(
                      &metadata)) {
            needs_flow_control_probe_body =
                http2_metadata->needs_flow_control_probe_body;
          }
        }
        resp.set_status_and_content(cinatra::status_type::ok,
                                    needs_flow_control_probe_body ? "x" : "");
        co_return;
      });

  server.init_ssl("include/cinatra/server.crt", "include/cinatra/server.key",
                  "test");

  server.async_start();
  auto bound_port = server.port();
  std::cout << "http2_conformance_server listening on "
            << "https://127.0.0.1:" << bound_port << " threads=" << thread_count
            << '\n';
  std::cout.flush();

  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));

  server.stop();
  return 0;
#endif
}
