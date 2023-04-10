#include <async_simple/coro/Collect.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "../include/cinatra.hpp"
#include "async_simple/Try.h"
#include "cmdline.h"
#include "config.h"
#include "util.h"

using namespace cinatra::press_tool;

press_config init_conf(const cmdline::parser& parser) {
  press_config conf{};
  conf.connections = parser.get<int>("connections");
  conf.threads_num = parser.get<int>("threads");

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

  conf.url = parser.rest().back();

  return conf;
}

async_simple::coro::Lazy<void> create_clients(const press_config& conf,
                                              std::vector<thread_counter>& v) {
  // create clients
  for (int i = 0; i < conf.connections; ++i) {
    size_t next = i % conf.threads_num;
    auto& thd_counter = v[next];
    auto client = std::make_shared<cinatra::coro_http_client>(
        thd_counter.ioc->get_executor());
    auto result = co_await client->async_get(conf.url);
    if (result.status != 200) {
      std::cerr << "connect " << conf.url
                << " failed: " << result.net_err.message() << "\n";
      exit(1);
    }
    thd_counter.conns.push_back(std::make_shared<cinatra::coro_http_client>(
        thd_counter.ioc->get_executor()));
  }
}

async_simple::coro::Lazy<void> press(thread_counter& counter,
                                     const std::string& url,
                                     std::atomic_bool& stop) {
  while (!stop) {
    for (auto& conn : counter.conns) {
      auto start = std::chrono::steady_clock::now();
      cinatra::resp_data result = co_await conn->async_get(url);
      auto elasped = std::chrono::steady_clock::now() - start;
      auto latency =
          std::chrono::duration_cast<std::chrono::milliseconds>(elasped);
      counter.requests++;
      if (result.status == 200) {
        counter.complete++;
        counter.bytes += result.total;
        if (counter.max_request_time <
            std::chrono::duration<double>(latency).count())
          counter.max_request_time =
              std::chrono::duration<double>(latency).count();
        if (counter.min_request_time >
            std::chrono::duration<double>(latency).count())
          counter.min_request_time =
              std::chrono::duration<double>(latency).count();
      }
      else {
        counter.errors++;
        std::cerr << "request failed: " << result.net_err.message() << "\n";
      }
    }
  }

  for (auto& conn : counter.conns) {
    conn->set_bench_stop();
    conn->async_close();
  }
}

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
      "header", 'H',
      "HTTP header to add to request, e.g. \"User-Agent: coro_http_press\"",
      false, "");

  parser.parse_check(argc, argv);

  press_config conf = init_conf(parser);

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
  for (auto& counter : v) {
    futures.push_back(press(counter, conf.url, stop));
  }

  // start timer
  asio::io_context timer_ioc;
  asio::steady_timer timer(timer_ioc, conf.press_interval);
  timer.async_wait([&stop](std::error_code ec) {
    stop = true;
  });
  std::thread timer_thd([&timer_ioc] {
    timer_ioc.run();
  });

  // wait finish
  async_simple::coro::syncAwait(
      async_simple::coro::collectAll(std::move(futures)));

  timer_thd.join();

  // statistic
  auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(conf.press_interval)
          .count();
  std::cout << "Running "
            << std::chrono::duration_cast<std::chrono::seconds>(
                   conf.press_interval)
                   .count()
            << "s "
            << "test @ " << conf.url << "\n";
  std::cout << "  " << conf.threads_num << " threads and " << conf.connections
            << " connections\n";

  uint64_t complete = 0;
  int64_t total_resp_size = 0;
  double max_latency = 0.0;
  double min_latency = INT32_MAX;
  uint64_t errors_requests = 0;
  for (auto& counter : v) {
    complete += counter.requests;
    total_resp_size += counter.bytes;
    errors_requests += counter.errors;
    if (max_latency < counter.max_request_time)
      max_latency = counter.max_request_time;
    if (min_latency > counter.min_request_time)
      min_latency = counter.min_request_time;
  }

  double total_avg_latency = 0;
  double avg_latency = (max_latency + min_latency) / 2;
  for (auto& counter : v) {
    double cur_avg_latency =
        (counter.max_request_time + counter.max_request_time) / 2;
    total_avg_latency +=
        (cur_avg_latency - avg_latency) * (cur_avg_latency - avg_latency);
  }

  double stdev = std::sqrt(double(total_avg_latency) / v.size());

  double qps = double(complete) / seconds;
  std::cout << "Thread Status   Avg   Stdev   Max\n";
  std::cout << "Latency   " << avg_latency << "ms"
            << "     " << stdev << "ms"
            << "     " << max_latency << "ms\n";
  std::cout << "  " << complete << " requests in " << seconds << "s"
            << " read: " << bytes_to_string(total_resp_size) << "\n";
  std::cout << "Requests/sec:     " << qps << "\n";

  // stop and clean
  works.clear();
  for (auto& counter : v) {
    if (counter.thd.joinable())
      counter.thd.join();
  }
  return 0;
}