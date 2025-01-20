#include <async_simple/coro/Collect.h>
#include <stdint.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "../include/cinatra.hpp"
#include "async_simple/Try.h"
#include "cmdline/cmdline.h"
#include "config.h"
#include "util.h"

#ifdef PRESS_TOOL_UNITTESTS
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#endif

using namespace cinatra::press_tool;

press_config init_conf(const cmdline::parser& parser) {
  press_config conf{};
  conf.connections = parser.get<int>("connections");
  conf.threads_num = parser.get<int>("threads");
  if (conf.threads_num > conf.connections) {
    std::cerr << "number of connections must be >= threads\n";
    exit(1);
  }

  std::string duration_str = parser.get<std::string>("duration");
  if (duration_str.size() < 2) {
    std::cerr << parser.usage();
    exit(1);
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

  if (parser.rest().empty()) {
    std::cerr << "lack of url";
    exit(1);
  }

  std::string headers = parser.get<std::string>("headers");
  if (!headers.empty()) {
    std::vector<std::string> header_lists;
    split(headers, " && ", header_lists);

    for (auto& header : header_lists) {
      std::vector<std::string> header_pair;
      split(header, ": ", header_pair);
      if (header_pair.size() == 2)
        conf.add_headers[header_pair[0]] = header_pair[1];
    }
  }

  conf.url = parser.rest().back();

  return conf;
}

async_simple::coro::Lazy<void> create_clients(const press_config& conf,
                                              std::vector<thread_counter>& v) {
  // create clients
  size_t retry_times = 10;
  cinatra::resp_data result{};
  for (int i = 0; i < conf.connections; ++i) {
    size_t next = i % conf.threads_num;
    auto& thd_counter = v[next];
    auto client = std::make_shared<cinatra::coro_http_client>(
        thd_counter.ioc->get_executor());

    int j = 0;
    for (j = 0; j < retry_times; ++j) {
      if (conf.add_headers.begin() != conf.add_headers.end()) {
        for (auto& single_header : conf.add_headers)
          client->add_header(single_header.first, single_header.second);
      }
      result = co_await client->connect(conf.url);
      if (result.status != 200) {
        client->reset();
        std::cout << "create client " << i + 1 << " failed, retry " << j + 1
                  << " times\n";
        continue;
      }

      break;
    }

    if (result.status != 200) {
      std::cerr << "connect " << conf.url << " for " << j << " times, "
                << " failed: " << result.net_err.message() << "\n";
      exit(1);
    }

    thd_counter.conns.push_back(std::move(client));
  }

  std::cout << "create " << conf.connections << " connections"
            << " successfully\n";
}

async_simple::coro::Lazy<void> press(thread_counter& counter,
                                     const std::string& path,
                                     std::atomic_bool& stop) {
  size_t err_count = 0;
  size_t conn_num = counter.conns.size();
  std::vector<async_simple::coro::Lazy<cinatra::resp_data>> futures;
  while (!stop) {
    for (auto& conn : counter.conns) {
      if (err_count == conn_num) {
        co_return;
      }
      if (counter.has_net_err) {
        continue;
      }

      futures.push_back(conn->async_get(path));
    }

    auto start = std::chrono::steady_clock::now();
    auto results = co_await async_simple::coro::collectAll(std::move(futures));
    auto elasped = std::chrono::steady_clock::now() - start;
    auto latency =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elasped).count() /
        conn_num;

    for (auto& item : results) {
      auto& result = item.value();
      counter.requests++;
      if (result.status == 200) {
        counter.complete++;
        counter.bytes += result.total;

        if (counter.max_request_time < latency)
          counter.max_request_time = latency;
        if (counter.min_request_time > latency)
          counter.min_request_time = latency;
      }
      else {
        if (stop) {
          counter.requests--;
        }
        else {
          counter.errors++;
        }

        counter.has_net_err = bool(result.net_err);
        if (counter.has_net_err) {
          err_count++;
        }
      }
    }

    futures.clear();
  }
}
#ifndef PRESS_TOOL_UNITTESTS
/*
 * eg: -c 1 -d 15s -t 1 http://localhost/
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
      "headers", 'H',
      "HTTP headers to add to request, e.g. \"User-Agent: coro_http_press\"\n"
      "            add multiple http headers in a request need to be separated "
      "by ' && '\n"
      "            e.g. \"User-Agent: coro_http_press && x-frame-options: "
      "SAMEORIGIN\"",
      false, "");

  parser.parse_check(argc, argv);

  press_config conf = init_conf(parser);

  if (conf.connections <= 0) {
    std::cout << "connection number is negative: " << conf.connections << "\n";
    std::exit(1);
  }

  // create threads
  std::vector<thread_counter> v;
  std::vector<std::shared_ptr<asio::io_context::work>> works;
  for (int i = 0; i < conf.threads_num; ++i) {
    auto ioc = std::make_shared<asio::io_context>();
    works.push_back(std::make_shared<asio::io_context::work>(*ioc));
    std::thread thd([ioc] {
      ioc->run();
    });
    v.push_back({.thd = std::move(thd), .ioc = ioc});
  }

  // create clients
  async_simple::coro::syncAwait(create_clients(conf, v));

  // create parallel request
  std::vector<async_simple::coro::Lazy<void>> futures;
  std::atomic_bool stop = false;
  std::string path = conf.url.substr(conf.url.rfind('/'));
  for (auto& counter : v) {
    futures.push_back(press(counter, path, stop));
  }

  // start timer
  bool has_timeout = false;
  asio::io_context timer_ioc;
  asio::steady_timer timer(timer_ioc, conf.press_interval);
  timer.async_wait([&stop, &v, &has_timeout](std::error_code ec) {
    if (ec) {
      return;
    }

    has_timeout = true;
    stop = true;
    for (auto& counter : v) {
      for (auto& conn : counter.conns) {
        conn->set_bench_stop();
        conn->close();
      }
    }
  });
  std::thread timer_thd([&timer_ioc] {
    timer_ioc.run();
  });

  // wait finish
  auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(conf.press_interval)
          .count();
  std::cout << "Running " << seconds << "s "
            << "test @ " << conf.url << "\n";
  std::cout << "  " << conf.threads_num << " threads and " << conf.connections
            << " connections\n";
  auto beg = std::chrono::steady_clock::now();
  auto wait_finish =
      [futures =
           std::move(futures)]() mutable -> async_simple::coro::Lazy<void> {
    co_await async_simple::coro::collectAll(std::move(futures));
  };
  async_simple::coro::syncAwait(wait_finish());
  if (!has_timeout) {
    timer_ioc.post([&timer] {
      asio::error_code ec;
      timer.cancel(ec);
    });
  }

  timer_thd.join();
  auto end = std::chrono::steady_clock::now();
  double dur_s =
      std::chrono::duration_cast<std::chrono::seconds>(end - beg).count();

  // statistic
  uint64_t total = 0;
  uint64_t complete = 0;
  uint64_t errors = 0;
  int64_t total_resp_size = 0;
  uint64_t max_latency = 0.0;
  uint64_t min_latency = UINT32_MAX;
  uint64_t errors_requests = 0;
  for (auto& counter : v) {
    total += counter.requests;
    complete += counter.complete;
    errors += counter.errors;
    total_resp_size += counter.bytes;
    errors_requests += counter.errors;
    if (max_latency < counter.max_request_time)
      max_latency = counter.max_request_time;
    if (min_latency > counter.min_request_time)
      min_latency = counter.min_request_time;
  }

  uint64_t avg_latency = (max_latency + min_latency) / 2;
  double variation = 0.0;
  double avg_latency_ms = (double)avg_latency / 1000000;
  for (auto& counter : v) {
    double counter_avg =
        (counter.max_request_time + counter.min_request_time) / 2000000;
    double counter_pow = pow(avg_latency_ms - counter_avg, 2);
    variation += counter_pow;
  }

  variation /= v.size();
  double stdev = sqrt(variation);

  double qps = double(complete) / seconds;
  std::cout << "  Thread Status   Avg   Max   Variation   Stdev\n";
  std::cout << "    Latency   " << std::fixed << std::setprecision(3)
            << double(avg_latency) / 1000000 << "ms"
            << "     " << double(max_latency) / 1000000 << "ms"
            << "     " << variation << "ms"
            << "     " << stdev << "ms\n";
  std::cout << "  " << complete << " requests in " << dur_s << "s"
            << ", " << bytes_to_string(total_resp_size) << " read"
            << ", total: " << total << ", errors: " << errors << "\n";
  std::cout << "Requests/sec:     " << qps << "\n";
  std::cout << "Transfer/sec:     " << bytes_to_string(total_resp_size / dur_s)
            << "\n";

  // stop and clean
  works.clear();
  for (auto& counter : v) {
    if (counter.thd.joinable())
      counter.thd.join();
  }
  return 0;
}
#endif

#ifdef PRESS_TOOL_UNITTESTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

std::string last_n(std::string input, int n) {
  return input.substr(input.size() - n);
}

TEST_CASE("test bytes_to_string function") {
  uint64_t bytes = 1023;
  std::string result = bytes_to_string(bytes);
  CHECK(result == "1023.000000bytes");

  bytes = 1024;
  result = bytes_to_string(bytes);
  CHECK(result == "1024.000000bytes");

  bytes = 1025;
  result = bytes_to_string(bytes);
  CHECK(last_n(result, 2) == "KB");

  bytes = 3 * 1024 * 1024;
  result = bytes_to_string(bytes);
  CHECK(result == "3.000000MB");

  bytes = (uint64_t)(3 * GB_BYTE);
  result = bytes_to_string(bytes);
  CHECK(result == "3.000000GB");
}

TEST_CASE("test multiple delimiters split function") {
  std::string headers = "User-Agent: coro_http_press";
  std::vector<std::string> header_lists;
  split(headers, " && ", header_lists);
  CHECK(header_lists.size() == 1);
  CHECK(header_lists[0] == "User-Agent: coro_http_press");

  header_lists.clear();
  headers = "User-Agent: coro_http_press && Connection: keep-alive";
  split(headers, " && ", header_lists);
  CHECK(header_lists.size() == 2);
  CHECK(header_lists[0] == "User-Agent: coro_http_press");
  CHECK(header_lists[1] == "Connection: keep-alive");

  header_lists.clear();
  headers = "User-Agent: coro_http_press&& Connection: keep-alive";
  split(headers, " && ", header_lists);
  CHECK(header_lists.size() != 2);
  CHECK(header_lists[0] ==
        "User-Agent: coro_http_press&& Connection: keep-alive");
}
// doctest comments
// 'function' : must be 'attribute' - see issue #182
DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char** argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP
#endif