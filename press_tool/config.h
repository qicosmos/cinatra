#pragma once

#include <stdint.h>

#include <asio.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace cinatra::press_tool {
struct press_config {
  int connections;
  int threads_num;
  std::chrono::steady_clock::duration press_interval;
  std::string url;
};

struct thread_counter {
  std::thread thd;
  std::shared_ptr<asio::io_context> ioc;
  std::vector<std::shared_ptr<cinatra::coro_http_client>> conns;
  //  uint64_t connections;
  uint64_t complete;
  uint64_t requests;
  uint64_t bytes;

  uint64_t errors;
  uint64_t max_request_time;
  uint64_t min_request_time = UINT32_MAX;
};
}  // namespace cinatra::press_tool