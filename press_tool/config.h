#pragma once

#include <stdint.h>

#include <chrono>
#include <string>
#include <thread>

#include "stats.h"

struct press_config {
  int connections;
  int threads_num;
  std::chrono::steady_clock::duration press_interval;
};

struct thread_counter {
  std::thread::id id;
  uint64_t connections;
  uint64_t complete;
  uint64_t requests;
  uint64_t bytes;
  uint64_t start;
};
