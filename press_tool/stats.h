#pragma once

#include <stdint.h>

struct request_stats {
  uint64_t total_resp_size;
  uint64_t number_requests;
  uint64_t number_errors;
};
