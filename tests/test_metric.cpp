#include <stdexcept>
#include <string>

#include "cinatra/ylt/metric/gauge.hpp"
#include "cinatra/ylt/metric/metric.hpp"
#define DOCTEST_CONFIG_IMPLEMENT
#include <random>

#include "cinatra/ylt/metric/counter.hpp"
#include "cinatra/ylt/metric/histogram.hpp"
#include "cinatra/ylt/metric/summary.hpp"
#include "doctest/doctest.h"
using namespace cinatra;

TEST_CASE("test no lable") {
  {
    gauge_t g{"test_gauge", "help"};
    g.inc();
    g.inc();

    std::string str;
    g.serialize_atomic(str);
    CHECK(str.find("test_gauge 2") != std::string::npos);

    g.dec();
    CHECK(g.atomic_value() == 1);
    CHECK_THROWS_AS(g.dec({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(g.inc({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(g.update({}, 1), std::invalid_argument);

    counter_t c{"test_counter", "help"};
    c.inc();
    c.inc();
    std::string str1;
    c.serialize_atomic(str1);
    CHECK(str1.find("test_counter 2") != std::string::npos);
  }
  {
    counter_t c("get_count", "get counter");
    CHECK(c.metric_type() == MetricType::Counter);
    CHECK(c.labels_name().empty());
    c.inc();
    CHECK(c.atomic_value() == 1);
    c.inc();
    CHECK(c.atomic_value() == 2);
    c.inc(0);

    CHECK(c.atomic_value() == 2);

    CHECK_THROWS_AS(c.inc(-2), std::invalid_argument);
    CHECK_THROWS_AS(c.inc({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(c.update({}, 1), std::invalid_argument);

    c.update(10);
    CHECK(c.atomic_value() == 10);

    c.update(0);
    CHECK(c.atomic_value() == 0);
  }
}

TEST_CASE("test with atomic") {
  counter_t c(
      "get_count", "get counter",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  std::vector<std::string> labels_value{"GET", "/"};
  c.inc(labels_value);
  c.inc(labels_value, 2);
  CHECK(c.atomic_value(labels_value) == 3);
  CHECK_THROWS_AS(c.inc({"GET", "/test"}), std::invalid_argument);
  CHECK_THROWS_AS(c.inc({"POST", "/"}), std::invalid_argument);
  c.update(labels_value, 10);
  CHECK(c.atomic_value(labels_value) == 10);

  gauge_t g(
      "get_qps", "get qps",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  g.inc(labels_value);
  g.inc(labels_value, 2);
  CHECK(g.atomic_value(labels_value) == 3);
  CHECK_THROWS_AS(g.inc({"GET", "/test"}), std::invalid_argument);
  CHECK_THROWS_AS(g.inc({"POST", "/"}), std::invalid_argument);
  g.dec(labels_value);
  g.dec(labels_value, 1);
  CHECK(g.atomic_value(labels_value) == 1);

  std::string str;
  c.serialize_atomic(str);
  std::cout << str;
  std::string str1;
  g.serialize_atomic(str1);
  std::cout << str1;
  CHECK(str.find("} 10") != std::string::npos);
  CHECK(str1.find("} 1") != std::string::npos);
}

TEST_CASE("test counter with dynamic labels value") {
  {
    auto c = std::make_shared<counter_t>(
        "get_count", "get counter", std::vector<std::string>{"method", "code"});
    CHECK(c->name() == "get_count");
    auto g = std::make_shared<gauge_t>(
        "get_count", "get counter", std::vector<std::string>{"method", "code"});
    CHECK(g->name() == "get_count");
    CHECK(g->metric_name() == "guage");
  }

  {
    counter_t c("get_count", "get counter",
                std::vector<std::string>{"method", "code"});
    CHECK(c.labels_name() == std::vector<std::string>{"method", "code"});
    c.inc({"GET", "200"}, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto values = async_simple::coro::syncAwait(c.async_value_map());
    CHECK(values[{"GET", "200"}] == 1);
    c.inc({"GET", "200"}, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    values = async_simple::coro::syncAwait(c.async_value_map());
    CHECK(values[{"GET", "200"}] == 3);

    std::string str;
    async_simple::coro::syncAwait(c.serialize_async(str));
    std::cout << str;
    CHECK(str.find("# TYPE get_count counter") != std::string::npos);
    CHECK(str.find("get_count{method=\"GET\",code=\"200\"} 3") !=
          std::string::npos);

    CHECK_THROWS_AS(c.inc({"GET", "200", "/"}, 2), std::invalid_argument);

    c.update({"GET", "200"}, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    values = async_simple::coro::syncAwait(c.async_value_map());
    CHECK(values[{"GET", "200"}] == 20);
  }
}

TEST_CASE("test guage") {
  {
    gauge_t g("get_count", "get counter");
    CHECK(g.metric_type() == MetricType::Guage);
    CHECK(g.labels_name().empty());
    g.inc();
    CHECK(g.atomic_value() == 1);
    g.inc();
    CHECK(g.atomic_value() == 2);
    g.inc(0);

    g.dec();
    CHECK(g.atomic_value() == 1);
    g.dec();
    CHECK(g.atomic_value() == 0);
  }

  {
    gauge_t g("get_count", "get counter", {"method", "code", "url"});
    CHECK(g.labels_name() == std::vector<std::string>{"method", "code", "url"});
    // method, status code, url
    g.inc({"GET", "200", "/"}, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto values = async_simple::coro::syncAwait(g.async_value_map());
    CHECK(values[{"GET", "200", "/"}] == 1);
    g.inc({"GET", "200", "/"}, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    values = async_simple::coro::syncAwait(g.async_value_map());
    CHECK(values[{"GET", "200", "/"}] == 3);

    std::string str;
    async_simple::coro::syncAwait(g.serialize_async(str));
    std::cout << str;
    CHECK(str.find("# TYPE get_count guage") != std::string::npos);
    CHECK(str.find("get_count{method=\"GET\",code=\"200\",url=\"/\"} 3") !=
          std::string::npos);

    CHECK_THROWS_AS(g.dec({"GET", "200"}, 1), std::invalid_argument);

    g.dec({"GET", "200", "/"}, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    values = async_simple::coro::syncAwait(g.async_value_map());
    CHECK(values[{"GET", "200", "/"}] == 2);
    g.dec({"GET", "200", "/"}, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    values = async_simple::coro::syncAwait(g.async_value_map());
    CHECK(values[{"GET", "200", "/"}] == 0);
  }
}

TEST_CASE("test histogram") {
  histogram_t h("test", "help", {5.0, 10.0, 20.0, 50.0, 100.0});
  h.observe(23);
  auto counts = h.get_bucket_counts();
  CHECK(counts[3]->atomic_value() == 1);
  h.observe(42);
  CHECK(counts[3]->atomic_value() == 2);
  h.observe(60);
  CHECK(counts[4]->atomic_value() == 1);
  h.observe(120);
  CHECK(counts[5]->atomic_value() == 1);
  h.observe(1);
  CHECK(counts[0]->atomic_value() == 1);
  std::string str;
  h.serialize_atomic(str);
  std::cout << str;
  CHECK(str.find("test_count") != std::string::npos);
  CHECK(str.find("test_sum") != std::string::npos);
  CHECK(str.find("test_bucket{le=\"5") != std::string::npos);
  CHECK(str.find("test_bucket{le=\"+Inf\"}") != std::string::npos);
}

TEST_CASE("test summary") {
  summary_t summary{"test_summary",
                    "summary help",
                    {{0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}}};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distr(1, 100);
  for (int i = 0; i < 50; i++) {
    summary.observe(distr(gen));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::string str;
  async_simple::coro::syncAwait(summary.serialize_async(str));
  std::cout << str;
  CHECK(summary.get_count() == 50);
  CHECK(summary.get_sum() > 0);
  CHECK(str.find("test_summary") != std::string::npos);
  CHECK(str.find("test_summary_count") != std::string::npos);
  CHECK(str.find("test_summary_sum") != std::string::npos);
  CHECK(str.find("test_summary{quantile=\"") != std::string::npos);
}

TEST_CASE("test register metric") {
  auto c = std::make_shared<counter_t>(std::string("get_count"),
                                       std::string("get counter"));
  default_metric_manger::regiter_metric<false>(c);
  CHECK_THROWS_AS(default_metric_manger::regiter_metric(c),
                  std::invalid_argument);

  auto g = std::make_shared<gauge_t>(std::string("get_guage_count"),
                                     std::string("get counter"));
  default_metric_manger::regiter_metric<false>(g);

  CHECK(default_metric_manger::metric_count<false>() == 2);
  CHECK(default_metric_manger::metric_keys<false>().size() == 2);

  c->inc();
  g->inc();

  auto map = default_metric_manger::metric_map<false>();
  CHECK(map["get_count"]->atomic_value() == 1);
  CHECK(map["get_guage_count"]->atomic_value() == 1);

  auto s =
      async_simple::coro::syncAwait(default_metric_manger::serialize<false>());
  std::cout << s << "\n";
  CHECK(s.find("get_count 1") != std::string::npos);
  CHECK(s.find("get_guage_count 1") != std::string::npos);

  auto m = default_metric_manger::get_metric<false>("get_count");
  CHECK(m->atomic_value() == 1);

  auto m1 = default_metric_manger::get_metric<false>("get_guage_count");
  CHECK(m1->atomic_value() == 1);

  {
    // because the first regiter_metric is set no lock, so visit
    // default_metric_manger with lock will throw.
    auto c1 = std::make_shared<counter_t>(std::string(""), std::string(""));
    CHECK_THROWS_AS(default_metric_manger::regiter_metric(c1),
                    std::invalid_argument);
    CHECK_THROWS_AS(default_metric_manger::metric_count(),
                    std::invalid_argument);
    CHECK_THROWS_AS(default_metric_manger::metric_keys(),
                    std::invalid_argument);
    CHECK_THROWS_AS(default_metric_manger::metric_map(), std::invalid_argument);
    CHECK_THROWS_AS(default_metric_manger::get_metric(""),
                    std::invalid_argument);
  }
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP