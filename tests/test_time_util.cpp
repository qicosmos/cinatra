#include <chrono>
#include <ctime>
#include <string>
#include <string_view>
#define DOCTEST_CONFIG_IMPLEMENT
#include <iostream>
#ifdef __linux__
#include <future>
#include <thread>
#endif

#include "cinatra/time_util.hpp"
#include "doctest/doctest.h"
using namespace std::chrono_literals;

#ifdef __linux__
namespace {
struct utc_conversion_hook {
  std::promise<void> converted;
  std::shared_future<void> resume;
};
thread_local utc_conversion_hook *conversion_hook = nullptr;

void after_utc_conversion() {
  if (auto *hook = conversion_hook) {
    conversion_hook = nullptr;
    hook->converted.set_value();
    hook->resume.wait();
  }
}
}  // namespace

extern "C" std::tm *__real_gmtime(const std::time_t *);
extern "C" std::tm *__real_gmtime_r(const std::time_t *, std::tm *);

extern "C" std::tm *__wrap_gmtime(const std::time_t *time) {
  auto *result = __real_gmtime(time);
  after_utc_conversion();
  return result;
}

extern "C" std::tm *__wrap_gmtime_r(const std::time_t *time, std::tm *buffer) {
  auto *result = __real_gmtime_r(time, buffer);
  after_utc_conversion();
  return result;
}

TEST_CASE("UTC conversion retains its own calendar fields") {
  std::promise<void> resume;
  utc_conversion_hook hook{{}, resume.get_future().share()};
  auto converted = hook.converted.get_future();
  std::string first;
  std::thread worker([&] {
    conversion_hook = &hook;
    char buffer[32];
    first =
        cinatra::get_local_time_str<0>(buffer, 946684800, "%Y-%m-%d %H:%M:%S");
  });
  const auto status = converted.wait_for(5s);
  std::string second;
  if (status == std::future_status::ready) {
    char buffer[32];
    second =
        cinatra::get_local_time_str<0>(buffer, 1609459200, "%Y-%m-%d %H:%M:%S");
  }
  resume.set_value();
  worker.join();
  REQUIRE(status == std::future_status::ready);
  CHECK(first == "2000-01-01 00:00:00");
  CHECK(second == "2021-01-01 00:00:00");
}
#endif

class ScopedTimer {
 public:
  ScopedTimer(const char *name)
      : m_name(name), m_beg(std::chrono::high_resolution_clock::now()) {}
  ScopedTimer(const char *name, uint64_t &ns) : ScopedTimer(name) {
    m_ns = &ns;
  }
  ~ScopedTimer() {
    auto end = std::chrono::high_resolution_clock::now();
    auto dur =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_beg);
    if (m_ns)
      *m_ns = dur.count();

    std::cout << m_name << " : " << dur.count() << " ns\n";
  }

 private:
  const char *m_name;
  std::chrono::time_point<std::chrono::high_resolution_clock> m_beg;
  uint64_t *m_ns = nullptr;
};

void test_local_time_performance() {
  auto local = cinatra::get_local_time_str();
  std::cout << local << "\n";

  int Count = 100000;
  std::string_view format = "%Y-%m-%d %H:%M:%S";
  char buf[32];
  for (int i = 0; i < 10; i++) {
    std::cout << "========= time: " << std::to_string(i) << " ==========\n";
    {
      ScopedTimer timer("localtime fast");
      for (int j = 0; j < Count; j++) {
        cinatra::get_local_time_str(buf, std::time(nullptr), format);
      }
    }

    // {
    //   ScopedTimer timer("localtime     ");
    //   for (int j = 0; j < Count; j++) {
    //     cinatra::get_local_time_str(buf, std::time(nullptr));
    //   }
    // }
  }
}

void test_gmt_time_performance() {
  int Count = 100000;
  char buf[32];
  for (int i = 0; i < 10; i++) {
    std::cout << "========= time: " << std::to_string(i) << " ==========\n";
    {
      ScopedTimer timer("gmttime fast");
      for (int j = 0; j < Count; j++) {
        cinatra::get_gmt_time_str();
      }
    }

    // {
    //   ScopedTimer timer("gmttime     ");
    //   for (int j = 0; j < Count; j++) {
    //     cinatra::get_gmt_time_str2(buf, std::time(nullptr));
    //   }
    // }
  }
}

TEST_CASE("test get time string") {
  test_local_time_performance();
  test_gmt_time_performance();

  auto s = cinatra::get_local_time_str();
  std::cout << s << "\n";

  // char buf[32];
  // auto gmt = cinatra::get_gmt_time_str2(buf, std::time(nullptr));
  // std::cout << gmt << "\n";

  auto gmt1 = cinatra::get_gmt_time_str();
  std::cout << gmt1 << "\n";
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP
