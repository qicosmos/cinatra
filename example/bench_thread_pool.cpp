// ThreadPool benchmark — configurable producers/consumers, warmup + multi-round
//
// Build: default cmake build (no special flags needed)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>

#include "cinatra/thread_pool.hpp"

using Clock = std::chrono::high_resolution_clock;

// ── helpers ──────────────────────────────────────────────────────────────────

struct Stats {
  double avg_ns;
  double p50_ns;
  double p99_ns;
  double min_ns;
  double max_ns;
};

static Stats compute(std::vector<double>& samples) {
  std::sort(samples.begin(), samples.end());
  size_t n = samples.size();
  double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  return {
      sum / n,         samples[n / 2], samples[static_cast<size_t>(n * 0.99)],
      samples.front(), samples.back(),
  };
}

static void print_stats(const char* label, const Stats& s) {
  std::printf(
      "  %-20s  avg=%8.0f ns  p50=%8.0f ns  p99=%8.0f ns  "
      "min=%8.0f ns  max=%8.0f ns\n",
      label, s.avg_ns, s.p50_ns, s.p99_ns, s.min_ns, s.max_ns);
}

// ── Benchmark: single-producer latency (submit round-trip) ───────────────────

static Stats bench_latency_1p(ThreadPool& pool, size_t n_tasks) {
  std::vector<double> latencies;
  latencies.reserve(n_tasks);

  for (size_t i = 0; i < n_tasks; ++i) {
    auto t0 = Clock::now();
    auto fut = pool.submit([] {
      return 42;
    });
    fut.get();
    auto t1 = Clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  return compute(latencies);
}

// ── Benchmark: single-producer latency with work ─────────────────────────────

static Stats bench_latency_1p_work(ThreadPool& pool, size_t n_tasks) {
  std::vector<double> latencies;
  latencies.reserve(n_tasks);

  for (size_t i = 0; i < n_tasks; ++i) {
    auto t0 = Clock::now();
    auto fut = pool.submit([i] {
      volatile double x = static_cast<double>(i);
      for (int j = 0; j < 20; ++j) x = std::sin(x + 1.0);
      return x;
    });
    fut.get();
    auto t1 = Clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  return compute(latencies);
}

// ── Benchmark: multi-producer latency ────────────────────────────────────────

static Stats bench_latency_mp(ThreadPool& pool, size_t n_tasks,
                              size_t n_producers) {
  size_t per_producer = n_tasks / n_producers;
  std::vector<std::vector<double>> all_latencies(n_producers);
  std::vector<std::thread> producers;
  producers.reserve(n_producers);

  std::atomic<bool> go{false};

  for (size_t p = 0; p < n_producers; ++p) {
    all_latencies[p].reserve(per_producer);
    producers.emplace_back([&pool, &all_latencies, per_producer, p, &go] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (size_t i = 0; i < per_producer; ++i) {
        auto t0 = Clock::now();
        auto fut = pool.submit([] {
          return 42;
        });
        fut.get();
        auto t1 = Clock::now();
        all_latencies[p].push_back(
            std::chrono::duration<double, std::nano>(t1 - t0).count());
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& t : producers) t.join();

  std::vector<double> merged;
  merged.reserve(per_producer * n_producers);
  for (auto& v : all_latencies) merged.insert(merged.end(), v.begin(), v.end());
  return compute(merged);
}

// ── Benchmark: single-producer throughput ────────────────────────────────────

static double bench_throughput_1p(ThreadPool& pool, size_t n_tasks) {
  std::atomic<size_t> done{0};

  auto t0 = Clock::now();
  auto ph = pool.make_producer();
  for (size_t i = 0; i < n_tasks; ++i) {
    pool.post(ph, [&done] {
      done.fetch_add(1, std::memory_order_relaxed);
    });
  }
  while (done.load(std::memory_order_acquire) < n_tasks) {
    std::this_thread::yield();
  }
  auto t1 = Clock::now();

  return n_tasks / std::chrono::duration<double>(t1 - t0).count();
}

// ── Benchmark: multi-producer throughput ─────────────────────────────────────

static double bench_throughput_mp(ThreadPool& pool, size_t n_tasks,
                                  size_t n_producers) {
  std::atomic<size_t> done{0};
  size_t per_producer = n_tasks / n_producers;
  size_t total = per_producer * n_producers;

  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  producers.reserve(n_producers);

  for (size_t p = 0; p < n_producers; ++p) {
    producers.emplace_back([&pool, &done, per_producer, &go] {
      auto ph = pool.make_producer();
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (size_t i = 0; i < per_producer; ++i) {
        pool.post(ph, [&done] {
          done.fetch_add(1, std::memory_order_relaxed);
        });
      }
    });
  }

  auto t0 = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : producers) t.join();

  while (done.load(std::memory_order_acquire) < total) {
    std::this_thread::yield();
  }
  auto t1 = Clock::now();

  return total / std::chrono::duration<double>(t1 - t0).count();
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
  const size_t HW = std::thread::hardware_concurrency();
  const size_t WARMUP = 10000;
  const size_t N_LATENCY = 50000;
  const size_t N_THROUGH = 200000;
  const int ROUNDS = 5;

  struct Config {
    size_t consumers;
    size_t producers;
  };
  Config configs[] = {
      {HW, 1}, {HW, 4}, {HW, HW}, {HW / 2, 1}, {HW / 2, HW / 2},
  };
  std::vector<Config> valid_configs;
  for (auto& c : configs) {
    if (c.consumers > 0 && c.producers > 0)
      valid_configs.push_back(c);
  }

  std::printf("ThreadPool benchmark\n");
  std::printf("  hardware_concurrency: %zu\n", HW);
  std::printf("  warmup: %zu   rounds: %d\n", WARMUP, ROUNDS);
  std::printf("  latency tasks/round: %zu   throughput tasks/round: %zu\n\n",
              N_LATENCY, N_THROUGH);

  for (auto& cfg : valid_configs) {
    std::printf(
        "============================================================\n");
    std::printf("  %zu consumer(s)  x  %zu producer(s)\n", cfg.consumers,
                cfg.producers);
    std::printf(
        "============================================================\n");

    ThreadPool pool(cfg.consumers);

    // ── Warmup
    std::printf("[warmup] %zu submit() calls...\n", WARMUP);
    for (size_t i = 0; i < WARMUP; ++i)
      pool.submit([] {
            return 0;
          })
          .get();
    std::printf("[warmup] done\n\n");

    for (int r = 1; r <= ROUNDS; ++r) {
      std::printf("── Round %d/%d ──\n", r, ROUNDS);

      if (cfg.producers == 1) {
        auto lat = bench_latency_1p(pool, N_LATENCY);
        print_stats("1P latency", lat);

        auto lat_w = bench_latency_1p_work(pool, N_LATENCY);
        print_stats("1P lat+work", lat_w);

        double tp = bench_throughput_1p(pool, N_THROUGH);
        std::printf("  %-20s  %.2f M tasks/sec\n", "1P throughput", tp / 1e6);
      }
      else {
        auto lat = bench_latency_mp(pool, N_LATENCY, cfg.producers);
        char label[64];
        std::snprintf(label, sizeof(label), "%zuP latency", cfg.producers);
        print_stats(label, lat);

        double tp = bench_throughput_mp(pool, N_THROUGH, cfg.producers);
        char tp_label[64];
        std::snprintf(tp_label, sizeof(tp_label), "%zuP throughput",
                      cfg.producers);
        std::printf("  %-20s  %.2f M tasks/sec\n", tp_label, tp / 1e6);
      }

      std::printf("\n");
    }
  }

  return 0;
}
