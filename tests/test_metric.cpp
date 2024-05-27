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
    g.serialize(str);
    CHECK(str.find("test_gauge 2") != std::string::npos);

    g.dec();
    CHECK(g.value() == 1);
    CHECK_THROWS_AS(g.dec({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(g.inc({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(g.update({}, 1), std::invalid_argument);

    counter_t c{"test_counter", "help"};
    c.inc();
    c.inc();
    std::string str1;
    c.serialize(str1);
    CHECK(str1.find("test_counter 2") != std::string::npos);
  }
  {
    counter_t c("get_count", "get counter");
    CHECK(c.metric_type() == MetricType::Counter);
    CHECK(c.labels_name().empty());
    c.inc();
    CHECK(c.value() == 1);
    c.inc();
    CHECK(c.value() == 2);
    c.inc(0);

    CHECK(c.value() == 2);

    CHECK_THROWS_AS(c.inc(-2), std::invalid_argument);
    CHECK_THROWS_AS(c.inc({}, 1), std::invalid_argument);
    CHECK_THROWS_AS(c.update({}, 1), std::invalid_argument);

    c.update(10);
    CHECK(c.value() == 10);

    c.update(0);
    CHECK(c.value() == 0);
  }
}

TEST_CASE("test counter") {
  {
    auto c = std::make_shared<counter_t>("get_count", "get counter",
                                         std::vector{"method", "code"});
    CHECK(c->name() == "get_count");
    auto g = std::make_shared<gauge_t>("get_count", "get counter",
                                       std::vector{"method", "code"});
    CHECK(g->name() == "get_count");
    CHECK(g->metric_name() == "guage");
  }

  {
    counter_t c("get_count", "get counter", {"method", "code"});
    CHECK(c.labels_name() == std::vector<std::string>{"method", "code"});
    c.inc({"GET", "200"}, 1);
    CHECK(c.values()[{"GET", "200"}].value == 1);
    c.inc({"GET", "200"}, 2);
    CHECK(c.values()[{"GET", "200"}].value == 3);

    std::string str;
    c.serialize(str);
    std::cout << str;
    CHECK(str.find("# TYPE get_count counter") != std::string::npos);
    CHECK(str.find("get_count{method=\"GET\",code=\"200\"} 3") !=
          std::string::npos);

    CHECK_THROWS_AS(c.inc({"GET", "200", "/"}, 2), std::invalid_argument);

    c.update({"GET", "200"}, 20);
    CHECK(c.values()[{"GET", "200"}].value == 20);
    c.reset();
    CHECK(c.values()[{"GET", "200"}].value == 0);
    CHECK(c.values().begin()->second.value == 0);
  }
}

TEST_CASE("test guage") {
  {
    gauge_t g("get_count", "get counter");
    CHECK(g.metric_type() == MetricType::Guage);
    CHECK(g.labels_name().empty());
    g.inc();
    CHECK(g.value() == 1);
    g.inc();
    CHECK(g.value() == 2);
    g.inc(0);

    g.dec();
    CHECK(g.value() == 1);
    g.dec();
    CHECK(g.value() == 0);
  }

  {
    gauge_t g("get_count", "get counter", {"method", "code", "url"});
    CHECK(g.labels_name() == std::vector<std::string>{"method", "code", "url"});
    // method, status code, url
    g.inc({"GET", "200", "/"}, 1);
    CHECK(g.values()[{"GET", "200", "/"}].value == 1);
    g.inc({"GET", "200", "/"}, 2);
    CHECK(g.values()[{"GET", "200", "/"}].value == 3);

    std::string str;
    g.serialize(str);
    std::cout << str;
    CHECK(str.find("# TYPE get_count guage") != std::string::npos);
    CHECK(str.find("get_count{method=\"GET\",code=\"200\",url=\"/\"} 3") !=
          std::string::npos);

    CHECK_THROWS_AS(g.dec({"GET", "200"}, 1), std::invalid_argument);

    g.dec({"GET", "200", "/"}, 1);
    CHECK(g.values()[{"GET", "200", "/"}].value == 2);
    g.dec({"GET", "200", "/"}, 2);
    CHECK(g.values()[{"GET", "200", "/"}].value == 0);
  }
}

TEST_CASE("test histogram") {
  histogram_t h("test", "help", {5.0, 10.0, 20.0, 50.0, 100.0});
  h.observe(23);
  auto counts = h.get_bucket_counts();
  CHECK(counts[3]->value() == 1);
  h.observe(42);
  CHECK(counts[3]->value() == 2);
  h.observe(60);
  CHECK(counts[4]->value() == 1);
  h.observe(120);
  CHECK(counts[5]->value() == 1);
  h.observe(1);
  CHECK(counts[0]->value() == 1);
  std::string str;
  h.serialize(str);
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

  std::string str;
  summary.serialize(str);
  std::cout << str;
  CHECK(str.find("test_summary") != std::string::npos);
  CHECK(str.find("test_summary_count") != std::string::npos);
  CHECK(str.find("test_summary_sum") != std::string::npos);
  CHECK(str.find("test_summary{quantile=\"") != std::string::npos);
}

TEST_CASE("test register metric") {
  auto c = std::make_shared<counter_t>(std::string("get_count"),
                                       std::string("get counter"));
  metric_t::regiter_metric(c);
  CHECK_THROWS_AS(metric_t::regiter_metric(c), std::invalid_argument);

  auto g = std::make_shared<gauge_t>(std::string("get_guage_count"),
                                     std::string("get counter"));
  metric_t::regiter_metric(g);

  CHECK(metric_t::metric_count() == 2);
  CHECK(metric_t::metric_keys().size() == 2);

  c->inc();
  g->inc();

  auto map = metric_t::metric_map();
  CHECK(map["get_count"]->value() == 1);
  CHECK(map["get_guage_count"]->value() == 1);

  auto s = metric_t::serialize();
  std::cout << s << "\n";
  CHECK(s.find("get_count 1") != std::string::npos);
  CHECK(s.find("get_guage_count 1") != std::string::npos);

  metric_t::remove_metric("get_count");
  CHECK(metric_t::metric_count() == 1);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP