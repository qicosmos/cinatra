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

  std::string headers = parser.get<std::string>("headers");
  if (!headers.empty()) {
    std::vector<std::string> header_lists;
    split(headers, " && ", header_lists);

    for (auto &header : header_lists) {
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
        for (auto &single_header : conf.add_headers)
          client->add_header(single_header.first, single_header.second);
      }
      result = co_await client->async_get(conf.url);
      if (result.status != 200) {
        client->reset();
        std::cout << "create client " << i + 1 << " failed, retry " << j + 1
                  << " times\n";
        continue;
      }

      std::cout << "create client " << i + 1 << " successfully\n";
      break;
    }

    if (result.status != 200) {
      std::cerr << "connect " << conf.url << " for " << j << " times, "
                << " failed: " << result.net_err.message() << "\n";
      exit(1);
    }
    thd_counter.conns.push_back(std::move(client));
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
          std::chrono::duration_cast<std::chrono::nanoseconds>(elasped).count();
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
      }
    }
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
      "headers", 'H',
      "HTTP headers to add to request, e.g. \"User-Agent: coro_http_press\"\n"
      "            add multiple http headers in a request need to be separated by ' && '\n"
      "            e.g. \"User-Agent: coro_http_press && x-frame-options: SAMEORIGIN\"",
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
  timer.async_wait([&stop, &v](std::error_code ec) {
    stop = true;
    for (auto& counter : v) {
      for (auto& conn : counter.conns) {
        conn->set_bench_stop();
        conn->async_close();
      }
    }
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

  double qps = double(complete) / seconds;
  std::cout << "Thread Status   Avg   Max\n";
  std::cout << "Latency   " << std::setprecision(3)
            << double(avg_latency) / 1000000 << "ms"
            << "     " << std::setprecision(3) << double(max_latency) / 1000000
            << "ms\n";
  std::cout << "  " << complete << " requests in " << seconds << "s"
            << " read: " << bytes_to_string(total_resp_size)
            << ", total: " << total << ", errors: " << errors << "\n";
  std::cout << "Requests/sec:     " << std::setprecision(8) << qps << "\n";

  // stop and clean
  works.clear();
  for (auto& counter : v) {
    if (counter.thd.joinable())
      counter.thd.join();
  }
  return 0;
}