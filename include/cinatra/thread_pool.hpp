#pragma once

// Low-latency MPMC thread pool based on moodycamel::ConcurrentQueue
//
// Key optimizations:
//  1. Per-worker ConsumerToken  — each worker remembers its last dequeue
//  position,
//     avoids scanning all sub-queues from scratch every time
//  2. Per-worker ProducerToken  — pool threads submitting recursive tasks get a
//     dedicated sub-queue, no implicit-producer hash lookup
//  3. External ProducerHandle   — long-lived external producers carry their own
//  token
//  4. Bulk dequeue              — amortizes overhead across up to BULK tasks
//  per loop
//  5. Adaptive wait             — spin → yield → sleep, zero syscall on hot
//  path
//  6. Pre-allocated capacity    — constructor reserves memory upfront, avoids
//     runtime malloc on first enqueues (ring-buffer-like steady state)
//  7. False-sharing guards      — queue head/tail already aligned inside moody;
//     Worker fields padded to separate cache lines
//
// Thread safety notes:
//  - Do NOT call post/submit after ThreadPool destructor begins.
//  - ProducerHandle must be destroyed before the ThreadPool that created it.

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cinatra/ylt/util/concurrentqueue.h"

using namespace ylt::detail;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <immintrin.h>
#define TP_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define TP_PAUSE() __asm__ volatile("yield")
#else
#define TP_PAUSE() std::this_thread::yield()
#endif

// ── Tunable traits
// ────────────────────────────────────────────────────────────
//
//  BLOCK_SIZE          tasks per internal block (power-of-2).
//                      Larger  → fewer allocations, higher steady-state memory.
//                      Smaller → lower memory, more allocation calls under
//                      burst.
//
//  MAX_SUBQUEUE_SIZE   soft cap per producer sub-queue.  Once reached moody
//                      stops allocating new blocks for that producer, giving
//                      bounded-ish behaviour similar to a ring buffer.
//
struct ThreadPoolTraits : moodycamel::ConcurrentQueueDefaultTraits {
  static constexpr size_t BLOCK_SIZE = 256;
  static constexpr size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 32;
  static constexpr size_t MAX_SUBQUEUE_SIZE = 65536;
};

// ── ThreadPool
// ────────────────────────────────────────────────────────────────
class ThreadPool {
 public:
  using Task = std::function<void()>;
  using Queue = moodycamel::ConcurrentQueue<Task, ThreadPoolTraits>;

  // ── ProducerHandle ────────────────────────────────────────────────────
  // Wrap a ProducerToken so external (non-pool) threads can submit without
  // paying the implicit-producer hash lookup on every enqueue call.
  //
  // Usage:
  //   auto ph = pool.make_producer();
  //   pool.post(ph, [] { ... });
  //
  // Lifetime: must be destroyed before the owning ThreadPool.
  //
  struct ProducerHandle {
    moodycamel::ProducerToken tok;
    explicit ProducerHandle(Queue& q) : tok(q) {}
    ProducerHandle(const ProducerHandle&) = delete;
    ProducerHandle& operator=(const ProducerHandle&) = delete;
    ProducerHandle(ProducerHandle&&) = default;
  };

  // ── Construction ─────────────────────────────────────────────────────
  //
  //  n_threads   worker thread count (default: hardware_concurrency)
  //  capacity    initial queue capacity hint; pre-allocates
  //              capacity / BLOCK_SIZE blocks upfront so the first burst
  //              of tasks hits pre-allocated memory (ring-buffer steady state)
  //
  explicit ThreadPool(size_t n_threads = std::thread::hardware_concurrency(),
                      size_t capacity = 1u << 16)
      : queue_(capacity), stop_(false), n_threads_(n_threads) {
    // Use unique_ptr so Worker addresses are stable regardless of
    // vector reallocation — tokens reference the queue internally
    // and must not be moved after construction.
    workers_.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i)
      workers_.push_back(std::make_unique<Worker>(queue_));

    for (size_t i = 0; i < n_threads; ++i)
      workers_[i]->thread = std::thread([this, i] {
        loop(i);
      });
  }

  ~ThreadPool() {
    stop_.store(true, std::memory_order_release);
    for (auto& w : workers_)
      if (w->thread.joinable())
        w->thread.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // ── Submit — returns std::future ──────────────────────────────────────

  // No token: uses implicit producer (convenient, slight hash-lookup cost)
  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args) {
    return submit_impl(nullptr, std::forward<F>(f),
                       std::forward<Args>(args)...);
  }

  // With external ProducerHandle: zero hash-lookup, dedicated sub-queue
  template <typename F, typename... Args>
  auto submit(ProducerHandle& ph, F&& f, Args&&... args) {
    return submit_impl(&ph.tok, std::forward<F>(f),
                       std::forward<Args>(args)...);
  }

  // ── Post — fire-and-forget (no future, lower overhead) ───────────────

  void post(Task task) { enqueue_one(nullptr, std::move(task)); }

  void post(ProducerHandle& ph, Task task) {
    enqueue_one(&ph.tok, std::move(task));
  }

  // Pool threads calling post() automatically use their own ptok.
  // Useful for recursive / continuation tasks spawned inside a task body.
  void post_auto(Task task) {
    int id = current_worker_id_;
    moodycamel::ProducerToken* pt = (id >= 0) ? &workers_[id]->ptok : nullptr;
    enqueue_one(pt, std::move(task));
  }

  // ── Bulk post — amortises enqueue cost across N tasks ─────────────────
  // `first` must be an iterator/pointer to Task (or convertible).
  template <typename InputIt>
  bool post_bulk(ProducerHandle& ph, InputIt first, size_t count) {
    return queue_.enqueue_bulk(ph.tok, first, count);
  }

  template <typename InputIt>
  bool post_bulk_auto(InputIt first, size_t count) {
    int id = current_worker_id_;
    if (id >= 0)
      return queue_.enqueue_bulk(workers_[id]->ptok, first, count);
    else
      return queue_.enqueue_bulk(first, count);
  }

  // ── Misc ──────────────────────────────────────────────────────────────
  ProducerHandle make_producer() { return ProducerHandle(queue_); }
  size_t thread_count() const { return n_threads_; }

 private:
  // ── Tunables ──────────────────────────────────────────────────────────
  static constexpr size_t BULK = 64;  // max tasks drained per loop iteration
  static constexpr int SP1 = 300;     // spin count before switching to yield
  static constexpr int SP2 = 2000;    // yield count before switching to sleep
  static constexpr auto SLEEP_US = std::chrono::microseconds(50);

  // ── Worker ────────────────────────────────────────────────────────────
  // ctok and ptok are the hot fields; pad them onto separate cache lines
  // so a stealer reading ctok doesn't invalidate the cache line holding ptok.
  struct Worker {
    alignas(64) moodycamel::ConsumerToken ctok;  // hot: dequeue path
    alignas(64) moodycamel::ProducerToken ptok;  // hot: recursive-post path
    std::thread thread;                          // cold: management only

    explicit Worker(Queue& q) : ctok(q), ptok(q) {}

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;
  };

  // ── Helpers ───────────────────────────────────────────────────────────
  // Spin-retry on enqueue failure (backpressure when queue is near capacity).
  // moodycamel only moves the element AFTER all capacity/allocation checks
  // pass, so on failure the item is untouched and safe to retry.
  void enqueue_one(moodycamel::ProducerToken* pt, Task&& t) {
    for (int attempt = 0;; ++attempt) {
      bool ok =
          pt ? queue_.enqueue(*pt, std::move(t)) : queue_.enqueue(std::move(t));
      if (ok)
        return;
      if (attempt < 16) {
        std::this_thread::yield();
      }
      else {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
  }

  template <typename F, typename... Args>
  auto submit_impl(moodycamel::ProducerToken* pt, F&& f, Args&&... args) {
    using R = std::invoke_result_t<F, Args...>;
    auto pkg = std::make_shared<std::packaged_task<R()>>(
        [f = std::forward<F>(f),
         ... args = std::forward<Args>(args)]() mutable {
          return std::invoke(f, std::forward<Args>(args)...);
        });
    auto fut = pkg->get_future();
    enqueue_one(pt, [pkg = std::move(pkg)] {
      (*pkg)();
    });
    return fut;
  }

  // ── Worker loop ───────────────────────────────────────────────────────
  void loop(size_t id) {
    current_worker_id_ = static_cast<int>(id);  // let post_auto() find us
    auto& w = *workers_[id];
    Task batch[BULK];
    int spin = 0;

    while (true) {
      // ① Bulk dequeue — one atomic op amortised over BULK tasks
      size_t n = queue_.try_dequeue_bulk(w.ctok, batch, BULK);
      if (n > 0) {
        spin = 0;
        for (size_t i = 0; i < n; ++i) {
          try {
            batch[i]();
          } catch (...) {
            // Task threw — swallow to keep the worker alive.
            // For submit() tasks the exception is already captured
            // inside packaged_task and delivered via future::get().
          }
        }
        continue;
      }

      // ② Queue empty — check stop flag
      if (stop_.load(std::memory_order_relaxed))
        break;

      // ③ Adaptive wait: spin → yield → sleep
      //    Keeps latency low under burst while saving CPU when idle.
      //    Once in sleep phase, stay there until new work arrives.
      if (spin < SP1) {
        TP_PAUSE();  // x86: reduces speculative pressure
        ++spin;
      }
      else if (spin < SP2) {
        std::this_thread::yield();
        ++spin;
      }
      else {
        std::this_thread::sleep_for(SLEEP_US);
        // stay at spin == SP2, keep sleeping until work arrives
      }
    }

    // ④ Drain: execute any tasks enqueued before/during shutdown.
    //    Use tokenless try_dequeue to scan ALL sub-queues, not just
    //    the ones this worker's ctok is biased toward.
    Task t;
    while (queue_.try_dequeue(t)) {
      try {
        t();
      } catch (...) {
      }
    }
  }

  // ── Data ──────────────────────────────────────────────────────────────
  Queue queue_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<bool> stop_;
  size_t n_threads_;

  // Each pool thread stores its own index here so post_auto() can
  // pick the right ptok without a map lookup.
  static inline thread_local int current_worker_id_ = -1;
};
