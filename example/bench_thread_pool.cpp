// ThreadPool benchmark — lock-free (ThreadPool) vs mutex-based (MutexPool)
//
// Build: default cmake build (no special flags needed)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <vector>

#include "cinatra/thread_pool.hpp"

// ═════════════════════════════════════════════════════════════════════════════
// Baseline: classic mutex + condition_variable thread pool
// ═════════════════════════════════════════════════════════════════════════════
class MutexPool {
 public:
  using Task = std::function<void()>;

  explicit MutexPool(size_t n_threads = std::thread::hardware_concurrency())
      : stop_(false) {
    for (size_t i = 0; i < n_threads; ++i)
      workers_.emplace_back([this] { loop(); });
  }

  ~MutexPool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_)
      if (w.joinable()) w.join();
  }

  MutexPool(const MutexPool&) = delete;
  MutexPool& operator=(const MutexPool&) = delete;

  void post(Task task) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      queue_.push(std::move(task));
    }
    cv_.notify_one();
  }

  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args) {
    using R = std::invoke_result_t<F, Args...>;
    auto pkg = std::make_shared<std::packaged_task<R()>>(
        [f = std::forward<F>(f),
         ...args = std::forward<Args>(args)]() mutable {
          return std::invoke(f, std::forward<Args>(args)...);
        });
    auto fut = pkg->get_future();
    post([pkg = std::move(pkg)] { (*pkg)(); });
    return fut;
  }

  size_t thread_count() const { return workers_.size(); }

 private:
  void loop() {
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        task = std::move(queue_.front());
        queue_.pop();
      }
      try {
        task();
      } catch (...) {
      }
    }
  }

  std::vector<std::thread> workers_;
  std::queue<Task> queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_;
};

// ═════════════════════════════════════════════════════════════════════════════
// Generic benchmark functions (templated on pool type)
// ═════════════════════════════════════════════════════════════════════════════
using Clock = std::chrono::high_resolution_clock;

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

// ── latency: single producer, submit + get ──────────────────────────────────
template <typename Pool>
static Stats bench_latency_1p(Pool& pool, size_t n_tasks) {
  std::vector<double> latencies;
  latencies.reserve(n_tasks);
  for (size_t i = 0; i < n_tasks; ++i) {
    auto t0 = Clock::now();
    auto fut = pool.submit([] { return 42; });
    fut.get();
    auto t1 = Clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  return compute(latencies);
}

// ── latency: single producer, submit + get with work ────────────────────────
template <typename Pool>
static Stats bench_latency_1p_work(Pool& pool, size_t n_tasks) {
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

// ── latency: multi-producer ─────────────────────────────────────────────────
template <typename Pool>
static Stats bench_latency_mp(Pool& pool, size_t n_tasks, size_t n_producers) {
  size_t per_producer = n_tasks / n_producers;
  std::vector<std::vector<double>> all_latencies(n_producers);
  std::vector<std::thread> producers;
  producers.reserve(n_producers);
  std::atomic<bool> go{false};

  for (size_t p = 0; p < n_producers; ++p) {
    all_latencies[p].reserve(per_producer);
    producers.emplace_back([&pool, &all_latencies, per_producer, p, &go] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t i = 0; i < per_producer; ++i) {
        auto t0 = Clock::now();
        auto fut = pool.submit([] { return 42; });
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
  for (auto& v : all_latencies)
    merged.insert(merged.end(), v.begin(), v.end());
  return compute(merged);
}

// ── throughput: single producer ─────────────────────────────────────────────
template <typename Pool>
static double bench_throughput_1p(Pool& pool, size_t n_tasks) {
  std::atomic<size_t> done{0};
  auto t0 = Clock::now();
  for (size_t i = 0; i < n_tasks; ++i) {
    pool.post([&done] { done.fetch_add(1, std::memory_order_relaxed); });
  }
  while (done.load(std::memory_order_acquire) < n_tasks)
    std::this_thread::yield();
  auto t1 = Clock::now();
  return n_tasks / std::chrono::duration<double>(t1 - t0).count();
}

// ── throughput: multi-producer ───────────────────────────────────────────────
template <typename Pool>
static double bench_throughput_mp(Pool& pool, size_t n_tasks,
                                  size_t n_producers) {
  std::atomic<size_t> done{0};
  size_t per_producer = n_tasks / n_producers;
  size_t total = per_producer * n_producers;

  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  producers.reserve(n_producers);

  for (size_t p = 0; p < n_producers; ++p) {
    producers.emplace_back([&pool, &done, per_producer, &go] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t i = 0; i < per_producer; ++i) {
        pool.post([&done] { done.fetch_add(1, std::memory_order_relaxed); });
      }
    });
  }

  auto t0 = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : producers) t.join();
  while (done.load(std::memory_order_acquire) < total)
    std::this_thread::yield();
  auto t1 = Clock::now();

  return total / std::chrono::duration<double>(t1 - t0).count();
}

// ═════════════════════════════════════════════════════════════════════════════
// Run all benchmarks for a given pool
// ═════════════════════════════════════════════════════════════════════════════
template <typename Pool>
static void run_benchmarks(const char* pool_name, size_t n_consumers,
                           size_t n_producers, size_t warmup, size_t n_latency,
                           size_t n_through, int rounds) {
  std::printf(
      "============================================================\n");
  std::printf("  [%s]  %zu consumer(s)  x  %zu producer(s)\n", pool_name,
              n_consumers, n_producers);
  std::printf(
      "============================================================\n");

  Pool pool(n_consumers);

  // warmup
  std::printf("[warmup] %zu submit() calls...\n", warmup);
  for (size_t i = 0; i < warmup; ++i) pool.submit([] { return 0; }).get();
  std::printf("[warmup] done\n\n");

  for (int r = 1; r <= rounds; ++r) {
    std::printf("── Round %d/%d ──\n", r, rounds);

    if (n_producers == 1) {
      auto lat = bench_latency_1p(pool, n_latency);
      print_stats("1P latency", lat);

      auto lat_w = bench_latency_1p_work(pool, n_latency);
      print_stats("1P lat+work", lat_w);

      double tp = bench_throughput_1p(pool, n_through);
      std::printf("  %-20s  %.2f M tasks/sec\n", "1P throughput", tp / 1e6);
    } else {
      char label[64], tp_label[64];

      auto lat = bench_latency_mp(pool, n_latency, n_producers);
      std::snprintf(label, sizeof(label), "%zuP latency", n_producers);
      print_stats(label, lat);

      double tp = bench_throughput_mp(pool, n_through, n_producers);
      std::snprintf(tp_label, sizeof(tp_label), "%zuP throughput", n_producers);
      std::printf("  %-20s  %.2f M tasks/sec\n", tp_label, tp / 1e6);
    }
    std::printf("\n");
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════════════════════
int main() {
  const size_t HW = std::thread::hardware_concurrency();
  const size_t WARMUP = 10000;
  const size_t N_LATENCY = 50000;
  const size_t N_THROUGH = 200000;
  const int ROUNDS = 3;

  struct Config {
    size_t consumers;
    size_t producers;
  };
  Config configs[] = {
      {HW, 1},
      {HW, 4},
      {HW / 2, 1},
  };

  std::printf("Thread Pool Benchmark: Lock-free vs Mutex-based\n");
  std::printf("  hardware_concurrency: %zu\n", HW);
  std::printf("  warmup: %zu   rounds: %d\n", WARMUP, ROUNDS);
  std::printf("  latency tasks/round: %zu   throughput tasks/round: %zu\n\n",
              N_LATENCY, N_THROUGH);

  for (auto& cfg : configs) {
    if (cfg.consumers == 0 || cfg.producers == 0) continue;

    // Lock-free pool
    run_benchmarks<ThreadPool>("LockFree", cfg.consumers, cfg.producers, WARMUP,
                               N_LATENCY, N_THROUGH, ROUNDS);

    // Mutex-based pool
    run_benchmarks<MutexPool>("Mutex", cfg.consumers, cfg.producers, WARMUP,
                              N_LATENCY, N_THROUGH, ROUNDS);

    std::printf("\n");
  }

  return 0;
}
