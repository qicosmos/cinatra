#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../include/cinatra.hpp"
#include "async_simple/Try.h"
#include "cmdline.h"
#include "config.h"

press_config init_conf(const cmdline::parser& parser) {
  press_config conf{};
  conf.connections = parser.get<int>("connections");
  conf.threads_num = parser.get<int>("threads");

  std::string duration_str = parser.get<std::string>("duration");
  if (duration_str.size() < 2) {
    throw std::invalid_argument("invalid arguments");
  }

  bool is_ms = duration_str.substr(duration_str.size() - 2) == "ms";
  std::string_view pre(duration_str.data(), duration_str.size() - 1);
  int tm = atoi(pre.data());
  if (is_ms) {
    conf.press_interval = std::chrono::milliseconds(tm);
  }
  if (duration_str.back() == 's') {
    conf.press_interval = std::chrono::seconds(tm);
  }
  else if (duration_str.back() == 'm') {
    conf.press_interval = std::chrono::minutes(tm);
  }
  else {
    conf.press_interval = std::chrono::hours(tm);
  }

  return conf;
}
/*
 * eg: -c 1 -d 15s -t 1
 */
int main(int argc, char* argv[]) {
  cmdline::parser parser;
  parser.add<int>(
      "connections", 'c',
      "total number of HTTP connections to keep open with"
      "                   each thread handling N = connections/threads",
      true, 0);
  parser.add<std::string>(
      "duration", 'd', "duration of the test, e.g. 2s, 2m, 2h", false, "15s");
  parser.add<int>("threads", 't', "total number of threads to use", false, 1);
  parser.add<std::string>(
      "header", 'H', "HTTP header to add to request, e.g. \"User-Agent: wrk\"",
      false, "");

  parser.parse_check(argc, argv);

  press_config conf = init_conf(parser);

  return 0;
}