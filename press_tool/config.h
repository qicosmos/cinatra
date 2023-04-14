#pragma once

#include <stdint.h>

#include <asio.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <map>

namespace cinatra::press_tool {
struct press_config {
  int connections;
  int threads_num;
  std::chrono::steady_clock::duration press_interval;
  std::string url;
  int read_fix = 0;
  std::map<std::string, std::string> add_headers;
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
  bool has_net_err = false;
};
}  // namespace cinatra::press_tool