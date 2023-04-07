#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../include/cinatra.hpp"
#include "async_simple/Try.h"
#include "cmdline.h"
#include "config.h"
#include "utils.h"

int main(int argc, char* argv[]) {
  cmdline::parser a;
  a.add<int>("connections", 'c',
             "total number of HTTP connections to keep open with"
             "                   each thread handling N = connections/threads",
             true, 0);
  a.add<std::string>("duration", 'd', "duration of the test, e.g. 2s, 2m, 2h",
                     false, "15s");
  a.add<int>("threads", 't', "total number of threads to use", false, 1);
  a.add<std::string>("header", 'H',
                     "HTTP header to add to request, e.g. \"User-Agent: wrk\"",
                     false, "");

  a.parse_check(argc, argv);

  return 0;
}