#ifndef CINATRA_WRK_CONFIG_HPP
#define CINATRA_WRK_CONFIG_HPP

#include <chrono>
#include <string>
#include <stdint.h>
#include "stats.h"

struct coro_config {
    int coroutines;
    int duration;
    int timeout_duration;
    request_stats *stats;
};

#endif  // CINATRA_CINATRA_HPP