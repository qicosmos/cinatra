#include <stdexcept>
#include <string>

#include "cinatra/ylt/metric/gauge.hpp"
#include "cinatra/ylt/metric/metric.hpp"
#define DOCTEST_CONFIG_IMPLEMENT
#include <random>

#include "cinatra/ylt/metric/counter.hpp"
#include "cinatra/ylt/metric/histogram.hpp"
#include "cinatra/ylt/metric/metric_manager.hpp"
#include "cinatra/ylt/metric/summary.hpp"
#include "cinatra/ylt/metric/system_metric.hpp"
#include "doctest/doctest.h"
using namespace ylt;
using namespace ylt::metric;

struct metrc_tag {};

struct test_tag {};

TEST_CASE("serialize zero") {
  counter_t c("test", "");
  gauge_t g("test1", "");
  std::string str;
  c.serialize(str);
  CHECK(str.empty());
  g.serialize(str);
  CHECK(str.empty());
  c.inc();
  c.serialize(str);
  CHECK(!str.empty());
  str.clear();
  g.inc();
  g.serialize(str);
  CHECK(!str.empty());
  c.update(0);
  c.serialize(str);
  CHECK(!str.empty());
  str.clear();
  g.dec();
  g.serialize(str);
  CHECK(!str.empty());
  str.clear();

  dynamic_counter_1t c1("test", "", {"url"});
  c1.serialize(str);
  CHECK(str.empty());
  dynamic_gauge_1t g1("test", "", {"url"});
  g1.serialize(str);
  CHECK(str.empty());
  c1.inc({"/test"});
  c1.serialize(str);
  CHECK(!str.empty());
  str.clear();
  g1.inc({"/test"});
  g1.serialize(str);
  CHECK(!str.empty());
  str.clear();

  c1.update({"/test"}, 0);
  c1.serialize(str);
  CHECK(!str.empty());
  str.clear();

  g1.dec({"/test"});
  g1.serialize(str);
  CHECK(!str.empty());
  str.clear();

#ifdef CINATRA_ENABLE_METRIC_JSON
  c1.serialize_to_json(str);
  CHECK(!str.empty());
  str.clear();
  g1.serialize_to_json(str);
  CHECK(!str.empty());
  str.clear();
#endif

  histogram_t h("test", "help", {5.23, 10.54, 20.0, 50.0, 100.0});
  h.serialize(str);
  CHECK(str.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  h.serialize_to_json(str);
#endif
  CHECK(str.empty());
  h.observe(23);
  h.serialize(str);
  CHECK(!str.empty());
  str.clear();

  std::map<std::string, std::string> customMap = {};
  auto summary = std::make_shared<summary_t>(
      "test", "help",
      summary_t::Quantiles{
          {0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
      customMap);
  async_simple::coro::syncAwait(summary->serialize_async(str));
  CHECK(str.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  async_simple::coro::syncAwait(summary->serialize_to_json_async(str));
  CHECK(str.empty());
#endif
  summary->observe(0);
  async_simple::coro::syncAwait(summary->serialize_async(str));
  CHECK(!str.empty());
  str.clear();
#ifdef CINATRA_ENABLE_METRIC_JSON
  async_simple::coro::syncAwait(summary->serialize_to_json_async(str));
  CHECK(!str.empty());
  str.clear();
#endif
}

TEST_CASE("test metric manager") {
  auto c = std::make_shared<counter_t>("test1", "");
  auto g = std::make_shared<gauge_t>("test2", "");
  auto& inst_s = static_metric_manager<metrc_tag>::instance();
  static_metric_manager<metrc_tag>::instance().register_metric(c);
  static_metric_manager<metrc_tag>::instance().register_metric(g);
  auto pair = inst_s.create_metric_static<counter_t>("test1", "");
  CHECK(pair.first == std::errc::invalid_argument);
  auto v1 = inst_s.get_metric_by_label({});
  CHECK(v1.size() == 2);
  auto v2 = inst_s.get_metric_by_name("test1");
  CHECK(v2 != nullptr);

  c->inc();
  g->inc();

  inst_s.create_metric_static<counter_t>(
      "test_counter", "", std::map<std::string, std::string>{{"url", "/"}});
  auto ms = inst_s.filter_metrics_by_label_value(std::regex("/"));
  CHECK(ms.size() == 1);

  {
    std::string str = inst_s.serialize_static();
    std::cout << str << "\n";
#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json = inst_s.serialize_to_json_static();
    std::cout << json << "\n";
#endif
  }

  {
    metric_filter_options options;
    options.name_regex = ".*test.*";
    auto v5 = inst_s.filter_metrics_static(options);
    CHECK(v5.size() == 3);
    options.label_regex = "url";
    auto v6 = inst_s.filter_metrics_static(options);
    CHECK(v6.size() == 1);
  }

  auto dc = std::make_shared<dynamic_counter_t>(
      std::string("test3"), std::string(""),
      std::array<std::string, 2>{"url", "code"});
  dynamic_metric_manager<metrc_tag>::instance().register_metric(dc);
  auto& inst_d = dynamic_metric_manager<metrc_tag>::instance();
  auto pair1 = inst_d.create_metric_dynamic<dynamic_counter_t>(
      std::string("test3"), std::string(""), std::array<std::string, 2>{});
  CHECK(pair1.first == std::errc::invalid_argument);
  dc->inc({"/", "200"});

  {
    std::string str = inst_d.serialize_dynamic();
    std::cout << str << "\n";

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json = inst_d.serialize_to_json_dynamic();
    std::cout << json << "\n";
#endif

    using root_manager = metric_collector_t<static_metric_manager<metrc_tag>,
                                            dynamic_metric_manager<metrc_tag>>;
    str = root_manager::serialize();
    std::cout << str << "\n";
#ifdef CINATRA_ENABLE_METRIC_JSON
    json = root_manager::serialize_to_json();
    std::cout << json << "\n";
#endif
  }

  auto v3 = inst_d.get_metric_by_label({{"url", "/"}, {"code", "200"}});
  CHECK(v3.size() == 1);

  auto v4 = inst_d.get_metric_by_label_name({"url", "code"});
  CHECK(v4.size() == 1);

  inst_d.remove_metric(dc);
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_metric(dc->str_name());
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_metric(std::vector<std::shared_ptr<dynamic_metric>>{dc});
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_metric({dc->str_name()});
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_metric_by_label({{"code", "400"}});
  CHECK(inst_d.metric_count() == 1);
  inst_d.remove_metric_by_label({{"code", "200"}});
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_label_value({{"code", "400"}});
  CHECK(inst_d.metric_count() == 1);
  inst_d.remove_label_value({{"code", "200"}});
  CHECK(dc->label_value_count() == 0);
  dc->inc({"/", "200"});

  CHECK(dc->label_value_count() == 1);
  inst_d.remove_label_value({{"url", "/"}});
  CHECK(dc->label_value_count() == 0);
  dc->inc({"/", "200"});

  CHECK(dc->label_value_count() == 1);
  inst_d.remove_label_value({{"url", "/"}, {"code", "200"}});
  CHECK(dc->label_value_count() == 0);
  dc->inc({"/", "200"});

  inst_d.remove_metric_by_label_name(std::vector<std::string>{"url", "code"});
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_metric_by_label_name("url");
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  inst_d.remove_metric_by_label_name("code");
  CHECK(inst_d.metric_count() == 0);
  inst_d.register_metric(dc);

  auto pair2 = inst_d.create_metric_dynamic<dynamic_counter_t>(
      "test4", "", std::array<std::string, 2>{"method", "code"});

  metric_filter_options options;
  options.name_regex = ".*test.*";
  auto v5 = inst_d.filter_metrics_dynamic(options);
  CHECK(v5.size() == 2);
  options.label_regex = "method";
  auto v6 = inst_d.filter_metrics_dynamic(options);
  CHECK(v6.size() == 1);

  options.label_value_regex = "200";

  auto v7 = inst_d.filter_metrics_dynamic(options);
  CHECK(v7.size() == 0);

  pair2.second->inc({"200"});
  auto v8 = inst_d.filter_metrics_dynamic(options);
  CHECK(v8.size() == 1);
}

TEST_CASE("test dynamic counter") {
  basic_dynamic_counter<int64_t, 2> c("test", "", {"url", "code"});
  c.inc({"/", "200"});
  c.inc({"/test", "200"});
  auto v1 = c.value({"/", "200"});
  auto v2 = c.value({"/test", "200"});
  CHECK(v1 == 1);
  CHECK(v2 == 1);

  {
    std::string str;
    c.serialize(str);
    std::cout << str << "\n";

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    c.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }

  basic_dynamic_counter<int64_t, 0> c1("test1", "", {});
  c1.inc({});
  auto v3 = c1.value({});
  CHECK(v3 == 1);

  {
    std::string str;
    c1.serialize(str);
    std::cout << str << "\n";

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    c1.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }

  basic_dynamic_gauge<int64_t, 1> g("test_gauge", "", {"url"});
  g.inc({"/"});
  CHECK(g.value({"/"}) == 1);

  g.dec({"/"});
  CHECK(g.value({"/"}) == 0);

  basic_dynamic_gauge<int64_t, 0> g1("test_gauge1", "", {});
  g1.inc({});
  CHECK(g1.value({}) == 1);
  g1.dec({});
  CHECK(g1.value({}) == 0);

  dynamic_gauge_t g2("test_g2", "", {"url", "code"});
  g2.inc({"/", "200"});
  CHECK(g2.value({"/", "200"}) == 1);
}

TEST_CASE("test static counter") {
  basic_static_counter<int64_t> c("test", "");
  c.inc();
  c.inc();
  auto v = c.value();
  CHECK(v == 2);

  {
    std::string str;
    c.serialize(str);
    std::cout << str << "\n";

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    c.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }

  basic_static_counter<int64_t> c1(
      "test", "",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  c1.inc();
  c1.inc();
  auto v1 = c1.value();
  CHECK(v1 == 2);

  {
    std::string str;
    c1.serialize(str);
    std::cout << str << "\n";

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    c1.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }

  basic_static_gauge<int64_t> g("test", "");
  g.inc();
  g.inc();
  auto v3 = g.value();
  CHECK(v3 == 2);
  g.dec();
  CHECK(g.value() == 1);

  basic_static_gauge<int64_t> g1("test", "",
                                 std::map<std::string, std::string>{});
  g1.inc();
  g1.inc();
  auto v4 = g1.value();
  CHECK(v4 == 2);
  g1.dec();
  CHECK(g1.value() == 1);
}

TEST_CASE("test static histogram") {
  {
    histogram_t h("test", "help", {5.23, 10.54, 20.0, 50.0, 100.0});
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

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    h.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }

  {
    histogram_t h(
        "test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
        std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
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

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    h.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }

  {
    histogram_t h(
        "test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
        std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});

    std::string str;
    h.serialize(str);
    std::cout << str;

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string json;
    h.serialize_to_json(json);
    std::cout << json << "\n";
#endif
  }
}

TEST_CASE("test dynamic histogram") {
  dynamic_histogram_t h("test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
                        {"method", "url"});
  h.observe({"GET", "/"}, 23);
  auto counts = h.get_bucket_counts();
  CHECK(counts[3]->value({"GET", "/"}) == 1);
  h.observe({"GET", "/"}, 42);
  CHECK(counts[3]->value({"GET", "/"}) == 2);
  h.observe({"GET", "/"}, 60);
  CHECK(counts[4]->value({"GET", "/"}) == 1);
  h.observe({"GET", "/"}, 120);
  CHECK(counts[5]->value({"GET", "/"}) == 1);
  h.observe({"GET", "/"}, 1);
  CHECK(counts[0]->value({"GET", "/"}) == 1);

  h.observe({"POST", "/"}, 23);
  CHECK(counts[3]->value({"POST", "/"}) == 1);
  h.observe({"POST", "/"}, 42);
  CHECK(counts[3]->value({"POST", "/"}) == 2);
  h.observe({"POST", "/"}, 60);
  CHECK(counts[4]->value({"POST", "/"}) == 1);
  h.observe({"POST", "/"}, 120);
  CHECK(counts[5]->value({"POST", "/"}) == 1);
  h.observe({"POST", "/"}, 1);
  CHECK(counts[0]->value({"POST", "/"}) == 1);

  std::string str;
  h.serialize(str);
  std::cout << str;

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string str_json;
  h.serialize_to_json(str_json);
  std::cout << str_json << "\n";
#endif
}

struct my_tag {};
using my_manager = static_metric_manager<my_tag>;

auto g_pair = my_manager::instance().create_metric_static<counter_t>(
    "test_g_counter", "");

TEST_CASE("test no lable") {
  {
    std::map<std::string, std::string> customMap = {};
    auto summary = std::make_shared<summary_t>(
        "test", "help",
        summary_t::Quantiles{
            {0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
        customMap);
    summary->observe(100);
  }
  auto g_counter = g_pair.second;
  g_counter->inc();
  CHECK(g_counter->value() == 1);
  {
    gauge_t g{"test_gauge", "help", 10};
    g.inc();
    g.inc();

    std::string str;
    g.serialize(str);
    CHECK(str.find("test_gauge 2") != std::string::npos);

    g.dec();
    CHECK(g.value() == 1);

    counter_t c{"test_counter", "help", 10};
    c.inc();
    c.inc();
    std::string str1;
    c.serialize(str1);
    CHECK(str1.find("test_counter 2") != std::string::npos);

    auto r = c.reset();
    CHECK(r == 2);
    CHECK(c.value() == 0);

    r = g.update(10);
    CHECK(r == 1);
    CHECK(g.value() == 10);
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

    c.update(10);
    CHECK(c.value() == 10);

    c.update(0);
    CHECK(c.value() == 0);
  }
}

TEST_CASE("test with atomic") {
  counter_t c(
      "get_count", "get counter",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});

  c.inc();
  c.inc(2);
  CHECK(c.value() == 3);
  c.update(10);
  CHECK(c.value() == 10);

  gauge_t g(
      "get_qps", "get qps",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  g.inc();
  g.inc(2);
  CHECK(g.value() == 3);
  g.dec();
  g.dec(1);
  CHECK(g.value() == 1);

  std::string str;
  c.serialize(str);
  std::cout << str;
  std::string str1;
  g.serialize(str1);
  std::cout << str1;
  CHECK(str.find("} 10") != std::string::npos);
  CHECK(str1.find("} 1") != std::string::npos);

  {
    gauge_t g("get_qps", "get qps",
              std::map<std::string, std::string>{{"method", "POST"},
                                                 {"url", "/test"}});
    g.inc();
    g.inc();
    CHECK(g.value() == 2);
    CHECK(g.value() == 2);
    g.dec();
    CHECK(g.value() == 1);
    CHECK(g.value() == 1);
    g.dec();
    CHECK(g.value() == 0);
    CHECK(g.value() == 0);
  }
}

TEST_CASE("test counter with dynamic labels value") {
  {
    auto c = std::make_shared<dynamic_counter_t>(
        "get_count", "get counter",
        std::array<std::string, 2>{"method", "code"});
    CHECK(c->name() == "get_count");
    auto g = std::make_shared<dynamic_gauge_t>(
        std::string("get_count"), std::string("get counter"),
        std::array<std::string, 2>{"method", "code"});
    CHECK(g->name() == "get_count");
    CHECK(g->metric_name() == "gauge");
  }

  {
    dynamic_counter_t c(std::string("get_count"), std::string("get counter"),
                        std::array<std::string, 2>{"method", "code"});
    CHECK(c.labels_name() == std::vector<std::string>{"method", "code"});
    c.inc({"GET", "200"}, 1);
    auto values = c.value_map();
    CHECK(values[{"GET", "200"}].value() == 1);
    c.inc({"GET", "200"}, 2);
    values = c.value_map();
    CHECK(values[{"GET", "200"}].value() == 3);

    std::string str;
    c.serialize(str);
    std::cout << str;
    CHECK(str.find("# TYPE get_count counter") != std::string::npos);
    CHECK(str.find("get_count{method=\"GET\",code=\"200\"} 3") !=
          std::string::npos);

    c.update({"GET", "200"}, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    values = c.value_map();
    CHECK(values[{"GET", "200"}].value() == 20);
  }
}

TEST_CASE("test gauge") {
  {
    gauge_t g("get_count", "get counter");
    CHECK(g.metric_type() == MetricType::Gauge);
    CHECK(g.labels_name().empty());
    g.inc();
    CHECK(g.value() == 1);
    g.inc();
    CHECK(g.value() == 2);
    g.inc(0);

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string str_json;
    g.serialize_to_json(str_json);
    std::cout << str_json << "\n";
    CHECK(str_json.find("\"value\":2") != std::string::npos);
#endif

    g.dec();
    CHECK(g.value() == 1);
    g.dec();
    CHECK(g.value() == 0);
  }

  {
    dynamic_gauge_3t g("get_count", "get counter", {"method", "code", "url"});
    CHECK(g.labels_name() == std::vector<std::string>{"method", "code", "url"});
    // method, status code, url
    g.inc({"GET", "200", "/"}, 1);
    auto values = g.value_map();
    CHECK(values[{"GET", "200", "/"}].value() == 1);
    g.inc({"GET", "200", "/"}, 2);
    values = g.value_map();
    CHECK(values[{"GET", "200", "/"}].value() == 3);

    g.inc({"POST", "200", "/"}, 4);

#ifdef CINATRA_ENABLE_METRIC_JSON
    std::string str_json;
    g.serialize_to_json(str_json);
    std::cout << str_json << "\n";
    CHECK(str_json.find("\"code\":\"200\"") != std::string::npos);
#endif

    std::string str;
    g.serialize(str);
    std::cout << str;
    CHECK(str.find("# TYPE get_count gauge") != std::string::npos);
    CHECK(str.find("get_count{method=\"GET\",code=\"200\",url=\"/\"} 3") !=
          std::string::npos);

    g.dec({"GET", "200", "/"}, 1);
    values = g.value_map();
    CHECK(values[{"GET", "200", "/"}].value() == 2);
    g.dec({"GET", "200", "/"}, 2);
    values = g.value_map();
    CHECK(values[{"GET", "200", "/"}].value() == 0);
  }
}

TEST_CASE("test histogram") {
  histogram_t h("test", "help", {5.23, 10.54, 20.0, 50.0, 100.0});
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
  std::cout << str << "\n";
  CHECK(str.find("test_count") != std::string::npos);
  CHECK(str.find("test_sum") != std::string::npos);
  CHECK(str.find("test_bucket{le=\"5.23") != std::string::npos);
  CHECK(str.find("test_bucket{le=\"+Inf\"}") != std::string::npos);

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string str_json;
  h.serialize_to_json(str_json);
  std::cout << str_json << "\n";
  CHECK(str_json.find("\"5.23\":1") != std::string::npos);
#endif
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

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string str;
  async_simple::coro::syncAwait(summary.serialize_async(str));
  std::cout << str;
  CHECK(async_simple::coro::syncAwait(summary.get_count()) == 50);
  CHECK(async_simple::coro::syncAwait(summary.get_sum()) > 0);
  CHECK(str.find("test_summary") != std::string::npos);
  CHECK(str.find("test_summary_count") != std::string::npos);
  CHECK(str.find("test_summary_sum") != std::string::npos);
  CHECK(str.find("test_summary{quantile=\"") != std::string::npos);

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string str_json;
  async_simple::coro::syncAwait(summary.serialize_to_json_async(str_json));
  std::cout << str_json << "\n";
  CHECK(str_json.find("\"0.9\":") != std::string::npos);
#endif
}

TEST_CASE("test register metric") {
  auto c = std::make_shared<counter_t>(std::string("get_count"),
                                       std::string("get counter"));
  default_static_metric_manager::instance().register_metric(c);
  CHECK_FALSE(default_static_metric_manager::instance().register_metric(c));

  auto g = std::make_shared<gauge_t>(std::string("get_guage_count"),
                                     std::string("get counter"));
  default_static_metric_manager::instance().register_metric(g);

  auto map1 = default_static_metric_manager::instance().metric_map();
  for (auto& [k, v] : map1) {
    bool r = k == "get_count" || k == "get_guage_count";
    break;
  }

  CHECK(default_static_metric_manager::instance().metric_count() >= 2);

  c->inc();
  g->inc();

  auto map = default_static_metric_manager::instance().metric_map();
  CHECK(map["get_count"]->as<counter_t>()->value() == 1);
  CHECK(map["get_guage_count"]->as<gauge_t>()->value() == 1);

  auto s = default_static_metric_manager::instance().serialize_static();
  std::cout << s << "\n";
  CHECK(s.find("get_count 1") != std::string::npos);
  CHECK(s.find("get_guage_count 1") != std::string::npos);

  auto m =
      default_static_metric_manager::instance().get_metric_static<counter_t>(
          "get_count");
  CHECK(m->as<counter_t>()->value() == 1);

  auto m1 =
      default_static_metric_manager::instance().get_metric_static<gauge_t>(
          "get_guage_count");
  CHECK(m1->as<gauge_t>()->value() == 1);
}

template <size_t id>
struct test_id_t {};

TEST_CASE("test remove metric and serialize metrics") {
  using metric_mgr = dynamic_metric_manager<test_id_t<1>>;

  metric_mgr::instance().create_metric_dynamic<dynamic_counter_2t>(
      "test_counter", "", std::array<std::string, 2>{});
  metric_mgr::instance().create_metric_dynamic<dynamic_counter_2t>(
      "test_counter2", "", std::array<std::string, 2>{});

  size_t count = metric_mgr::instance().metric_count();
  CHECK(count == 2);

  metric_mgr::instance().remove_metric("test_counter");
  count = metric_mgr::instance().metric_count();
  CHECK(count == 1);

  metric_mgr::instance().remove_metric("test_counter2");
  count = metric_mgr::instance().metric_count();
  CHECK(count == 0);

  using metric_mgr2 = static_metric_manager<test_id_t<2>>;
  auto c = metric_mgr2::instance().create_metric_static<counter_t>(
      "test_static_counter", "");
  auto c2 = metric_mgr2::instance().create_metric_static<counter_t>(
      "test_static_counter2", "");
  c.second->inc();
  c2.second->inc();

#ifdef CINATRA_ENABLE_METRIC_JSON
  auto s = metric_mgr2::instance().serialize_to_json_static();
  std::cout << s << "\n";
#endif
  count = metric_mgr2::instance().metric_count();
  CHECK(count == 2);
}

TEST_CASE("test filter metrics static") {
  using metric_mgr = static_metric_manager<test_id_t<3>>;
  auto c = metric_mgr::instance().create_metric_static<counter_t>(
      "test_static_counter", "",
      std::map<std::string, std::string>{{"method", "GET"}});
  auto c2 = metric_mgr::instance().create_metric_static<counter_t>(
      "test_static_counter2", "",
      std::map<std::string, std::string>{{"url", "/"}});
  c.second->inc();
  c2.second->inc();

  metric_filter_options options;
  options.name_regex = ".*counter.*";
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.size() == 2);

    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_static_counter") != std::string::npos);
    std::cout << s << "\n";
  }

  options.label_regex = ".*ur.*";
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.size() == 1);
    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_static_counter2") != std::string::npos);
    std::cout << s << "\n";
  }

  options.name_regex = "no_such_name";
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.empty());
    auto s = manager_helper::serialize(metrics);
    CHECK(s.empty());
  }

  options = {};
  options.label_regex = "no_such_label";
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.empty());
    auto s = manager_helper::serialize(metrics);
    CHECK(s.empty());
  }

  // don't filter
  options = {};
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.size() == 2);
  }

  // black
  options.label_regex = ".*ur.*";
  options.is_white = false;
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.size() == 1);
    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_static_counter") != std::string::npos);
    CHECK(s.find("test_static_counter2") == std::string::npos);
  }

  options = {};
  options.label_regex = ".*ur.*";
  options.is_white = false;
  {
    auto metrics = metric_mgr::instance().filter_metrics_static(options);
    CHECK(metrics.size() == 1);
    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_static_counter") != std::string::npos);
    CHECK(s.find("method") != std::string::npos);
    CHECK(s.find("test_static_counter2") == std::string::npos);
    CHECK(s.find("url") == std::string::npos);
  }
}

TEST_CASE("test filter metrics dynamic") {
  using metric_mgr = dynamic_metric_manager<test_id_t<4>>;
  auto [ec, c] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_1t>(
          "test_dynamic_counter", "", std::array<std::string, 1>{"method"});
  auto [ec2, c2] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_1t>(
          "test_dynamic_counter2", "", std::array<std::string, 1>{"url"});
  c->inc({"GET"});
  c->inc({"POST"});
  c2->inc({"/"});
  c2->inc({"/test"});

  metric_filter_options options;
  options.name_regex = ".*counter.*";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 2);

    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_dynamic_counter") != std::string::npos);
    std::cout << s << "\n";
  }

  options.label_regex = ".*ur.*";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 1);
    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_dynamic_counter2") != std::string::npos);
    std::cout << s << "\n";
  }

  options.name_regex = "no_such_name";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.empty());
    auto s = manager_helper::serialize(metrics);
    CHECK(s.empty());
  }

  options = {};
  options.label_regex = "no_such_label";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.empty());
    auto s = manager_helper::serialize(metrics);
    CHECK(s.empty());
  }

  // don't filter
  options = {};
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 2);
  }

  // black
  options.label_regex = ".*ur.*";
  options.is_white = false;
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 1);
    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_dynamic_counter") != std::string::npos);
    CHECK(s.find("test_dynamic_counter2") == std::string::npos);
  }

  options = {};
  options.label_regex = ".*ur.*";
  options.is_white = false;
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 1);
    auto s = manager_helper::serialize(metrics);
    CHECK(s.find("test_dynamic_counter") != std::string::npos);
    CHECK(s.find("method") != std::string::npos);
    CHECK(s.find("test_dynamic_counter2") == std::string::npos);
    CHECK(s.find("url") == std::string::npos);
  }
}

TEST_CASE("test get metric by static labels and label") {
  using metric_mgr = static_metric_manager<test_id_t<9>>;
  metric_mgr::instance().create_metric_static<counter_t>(
      "http_req_test", "",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  metric_mgr::instance().create_metric_static<gauge_t>(
      "http_req_test1", "",
      std::map<std::string, std::string>{{"method", "POST"}, {"url", "/"}});
  metric_mgr::instance().create_metric_static<counter_t>(
      "http_req_test2", "",
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/test"}});

  auto v = metric_mgr::instance().get_metric_by_label(
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/test"}});
  CHECK(v[0]->name() == "http_req_test2");

  v = metric_mgr::instance().get_metric_by_label(
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  CHECK(v[0]->name() == "http_req_test");

  auto [ec, h1] = metric_mgr::instance().create_metric_static<histogram_t>(
      "http_req_static_hist", "help",
      std::vector<double>{5.23, 10.54, 20.0, 50.0, 100.0},
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});

  h1->observe(23);

  std::string str1;
  h1->serialize(str1);
  std::cout << str1;
  CHECK(str1.find("method=\"GET\",url=\"/\",le=") != std::string::npos);

  auto map =
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}};
  auto [ec1, s1] = metric_mgr::instance().create_metric_static<summary_t>(
      "http_req_static_summary", "help",
      summary_t::Quantiles{
          {0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
  s1->observe(23);

  auto vec = metric_mgr::instance().get_metric_by_label(map);
  CHECK(vec.size() == 3);

  {
    using metric_mgr2 = static_metric_manager<test_id_t<19>>;
    auto [ec, s2] = metric_mgr2::instance().create_metric_static<summary_t>(
        "http_req_static_summary2", "help",
        summary_t::Quantiles{
            {0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
        map);
    s2->observe(23);

    auto vec = metric_mgr2::instance().get_metric_by_label(map);
    CHECK(vec.size() == 1);
  }
}

TEST_CASE("test get metric by dynamic labels") {
  using metric_mgr = dynamic_metric_manager<test_id_t<10>>;
  auto [ec, c] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "http_req_static", "", std::array<std::string, 2>{"method", "code"});

  auto [ec1, c1] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "http_req_static1", "", std::array<std::string, 2>{"method", "code"});

  auto [ec2, c2] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "http_req_static2", "", std::array<std::string, 2>{"method", "code"});

  auto [ec3, c3] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "http_req_static3", "", std::array<std::string, 2>{"method", "code"});

  c->inc({"POST", "200"});
  c1->inc({"GET", "200"});
  c2->inc({"POST", "301"});
  c3->inc({"POST", "400"});

  auto [ec4, c4] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "http_req_static4", "", std::array<std::string, 2>{"host", "url"});

  auto [ec5, c5] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "http_req_static5", "", std::array<std::string, 2>{"host", "url"});

  c4->inc({"shanghai", "/"});
  c5->inc({"shanghai", "/test"});

  auto vec =
      metric_mgr::instance().filter_metrics_by_label_value(std::regex("POST"));
  CHECK(vec.size() == 3);

  vec = metric_mgr::instance().filter_metrics_by_label_value(std::regex("GET"));
  CHECK(vec.size() == 1);

  vec = metric_mgr::instance().filter_metrics_by_label_value(
      std::regex("shanghai"));
  CHECK(vec.size() == 2);

  vec = metric_mgr::instance().filter_metrics_by_label_value(std::regex("/"));
  CHECK(vec.size() == 1);

  vec =
      metric_mgr::instance().filter_metrics_by_label_value(std::regex("/test"));
  CHECK(vec.size() == 1);

  vec =
      metric_mgr::instance().filter_metrics_by_label_value(std::regex("/none"));
  CHECK(vec.size() == 0);

  vec =
      metric_mgr::instance().filter_metrics_by_label_value(std::regex("HEAD"));
  CHECK(vec.size() == 0);

  auto [ec6, h1] =
      metric_mgr::instance().create_metric_dynamic<dynamic_histogram_2t>(
          "http_req_static_hist", "help",
          std::vector<double>{5.23, 10.54, 20.0, 50.0, 100.0},
          std::array<std::string, 2>{"method", "url"});

  h1->observe({"GET", "/"}, 23);

  auto [ec7, s1] =
      metric_mgr::instance().create_metric_dynamic<dynamic_summary_2>(
          "http_req_static_summary", "help",
          summary_t::Quantiles{
              {0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
          std::array<std::string, 2>{"method", "url"});
  s1->observe({"GET", "/"}, 23);

  vec = metric_mgr::instance().filter_metrics_by_label_value(std::regex("GET"));
  CHECK(vec.size() >= 2);

  auto str = metric_mgr::instance().serialize_dynamic();
  std::cout << str;

#ifdef CINATRA_ENABLE_METRIC_JSON
  auto json_str = metric_mgr::instance().serialize_to_json_dynamic();
  std::cout << json_str << "\n";
#endif
}

TEST_CASE("test histogram serialize with dynamic labels") {
  dynamic_histogram_2t h("test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
                         std::array<std::string, 2>{"method", "url"});
  h.observe({"GET", "/"}, 23);
  auto counts = h.get_bucket_counts();
  CHECK(counts[3]->value({"GET", "/"}) == 1);
  h.observe({"GET", "/"}, 42);
  CHECK(counts[3]->value({"GET", "/"}) == 2);
  h.observe({"GET", "/"}, 60);
  CHECK(counts[4]->value({"GET", "/"}) == 1);
  h.observe({"GET", "/"}, 120);
  CHECK(counts[5]->value({"GET", "/"}) == 1);
  h.observe({"GET", "/"}, 1);
  CHECK(counts[0]->value({"GET", "/"}) == 1);

  h.observe({"POST", "/"}, 23);
  CHECK(counts[3]->value({"POST", "/"}) == 1);
  h.observe({"POST", "/"}, 42);
  CHECK(counts[3]->value({"POST", "/"}) == 2);
  h.observe({"POST", "/"}, 60);
  CHECK(counts[4]->value({"POST", "/"}) == 1);
  h.observe({"POST", "/"}, 120);
  CHECK(counts[5]->value({"POST", "/"}) == 1);
  h.observe({"POST", "/"}, 1);
  CHECK(counts[0]->value({"POST", "/"}) == 1);

  std::string str;
  h.serialize(str);
  std::cout << str;

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string str_json;
  h.serialize_to_json(str_json);
  std::cout << str_json << "\n";
#endif
}

TEST_CASE("test histogram serialize with static labels default") {
  histogram_t h(
      "test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
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
}

TEST_CASE("test histogram serialize with static labels") {
  histogram_t h(
      "test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}});
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

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string str_json;
  h.serialize_to_json(str_json);
  std::cout << str_json << "\n";
#endif
}

TEST_CASE("test summary with dynamic labels") {
  basic_dynamic_summary<2> summary{
      "test_summary",
      "summary help",
      {{0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
      {"method", "url"}};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distr(1, 100);
  for (int i = 0; i < 50; i++) {
    summary.observe({"GET", "/"}, distr(gen));
    summary.observe({"POST", "/test"}, distr(gen));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  double sum;
  uint64_t count;
  auto rates = async_simple::coro::syncAwait(
      summary.get_rates({"GET", "/"}, sum, count));
  std::cout << rates.size() << "\n";

  std::string str;
  async_simple::coro::syncAwait(summary.serialize_async(str));
  std::cout << str;

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string json_str;
  async_simple::coro::syncAwait(summary.serialize_to_json_async(json_str));
  std::cout << json_str << "\n";
#endif
}

TEST_CASE("test summary with static labels") {
  summary_t summary{
      "test_summary",
      "summary help",
      {{0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}},
      std::map<std::string, std::string>{{"method", "GET"}, {"url", "/"}}};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distr(1, 100);
  for (int i = 0; i < 50; i++) {
    summary.observe(distr(gen));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  double sum;
  uint64_t count;
  auto rates = async_simple::coro::syncAwait(summary.get_rates(sum, count));
  std::cout << rates.size() << "\n";

  auto rates1 = async_simple::coro::syncAwait(summary.get_rates(sum, count));
  CHECK(rates == rates1);

  std::string str;
  async_simple::coro::syncAwait(summary.serialize_async(str));
  std::cout << str;

#ifdef CINATRA_ENABLE_METRIC_JSON
  std::string json_str;
  async_simple::coro::syncAwait(summary.serialize_to_json_async(json_str));
  std::cout << json_str << "\n";
#endif
}

TEST_CASE("test serialize with emptry metrics") {
  std::string s1;

  auto h1 = std::make_shared<dynamic_histogram_1t>(
      "get_count2", "help", std::vector<double>{5.23, 10.54, 20.0, 50.0, 100.0},
      std::array<std::string, 1>{"method"});
  h1->serialize(s1);
  CHECK(s1.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  h1->serialize_to_json(s1);
  CHECK(s1.empty());
#endif

  auto h2 = std::make_shared<histogram_t>(
      "get_count2", "help",
      std::vector<double>{5.23, 10.54, 20.0, 50.0, 100.0});
  h2->serialize(s1);
  CHECK(s1.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  h2->serialize_to_json(s1);
  CHECK(s1.empty());
#endif

  auto h3 = std::make_shared<histogram_t>(
      "get_count2", "help", std::vector<double>{5.23, 10.54, 20.0, 50.0, 100.0},
      std::map<std::string, std::string>{{"method", "/"}});
  h3->serialize(s1);
  CHECK(s1.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  h3->serialize_to_json(s1);
  CHECK(s1.empty());
#endif

  auto c1 = std::make_shared<counter_t>(std::string("get_count"),
                                        std::string("get counter"));
  c1->serialize(s1);
  CHECK(s1.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  c1->serialize_to_json(s1);
  CHECK(s1.empty());
#endif

  auto c2 = std::make_shared<counter_t>(
      std::string("get_count"), std::string("get counter"),
      std::map<std::string, std::string>{{"method", "GET"}});
  c2->serialize(s1);
  CHECK(s1.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  c2->serialize_to_json(s1);
  CHECK(s1.empty());
#endif

  auto c3 = std::make_shared<dynamic_counter_1t>(
      std::string("get_count"), std::string("get counter"),
      std::array<std::string, 1>{"method"});
  c3->serialize(s1);
  CHECK(s1.empty());
#ifdef CINATRA_ENABLE_METRIC_JSON
  c3->serialize_to_json(s1);
  CHECK(s1.empty());
#endif

  {
    std::string str;
    h1->observe({"POST"}, 1);
    h1->serialize(str);
    CHECK(!str.empty());
    str.clear();
#ifdef CINATRA_ENABLE_METRIC_JSON
    h1->serialize_to_json(str);
    CHECK(!str.empty());
#endif
  }

  {
    std::string str;
    h2->observe(1);
    h2->serialize(str);
    CHECK(!str.empty());
    str.clear();
#ifdef CINATRA_ENABLE_METRIC_JSON
    h1->serialize_to_json(str);
    CHECK(!str.empty());
#endif
  }

  {
    std::string str;
    c1->inc();
    c1->serialize(str);
    CHECK(!str.empty());
    str.clear();
#ifdef CINATRA_ENABLE_METRIC_JSON
    c1->serialize_to_json(str);
    CHECK(!str.empty());
#endif
  }

  {
    std::string str;
    c2->inc();
    c2->serialize(str);
    CHECK(!str.empty());
    str.clear();
#ifdef CINATRA_ENABLE_METRIC_JSON
    c2->serialize_to_json(str);
    CHECK(!str.empty());
#endif
  }

  {
    std::string str;
    c3->inc({"POST"});
    c3->serialize(str);
    CHECK(!str.empty());
    str.clear();
#ifdef CINATRA_ENABLE_METRIC_JSON
    c3->serialize_to_json(str);
    CHECK(!str.empty());
#endif
  }
}

TEST_CASE("test serialize with multiple threads") {
  {
    dynamic_histogram_d h("test", "help", {5.23, 10.54, 20.0, 50.0, 100.0},
                          std::array<std::string, 2>{"url", "code"});
    h.observe({"/", "code"}, 23);
  }
  auto c = std::make_shared<dynamic_counter_1t>(
      std::string("get_count"), std::string("get counter"),
      std::array<std::string, 1>{"method"});
  auto g = std::make_shared<dynamic_counter_1t>(
      std::string("get_count1"), std::string("get counter"),
      std::array<std::string, 1>{"method"});

  auto h1 = std::make_shared<dynamic_histogram_1t>(
      "get_count2", "help", std::vector<double>{5.23, 10.54, 20.0, 50.0, 100.0},
      std::array<std::string, 1>{"method"});

  auto c1 = std::make_shared<dynamic_counter_1t>(
      std::string("get_count3"), std::string("get counter"),
      std::array<std::string, 1>{"method"});

  using test_metric_manager = dynamic_metric_manager<test_id_t<20>>;

  test_metric_manager::instance().register_metric({c, g, h1, c1});

  c->inc({"POST"}, 1);
  g->inc({"GET"}, 1);
  h1->observe({"HEAD"}, 1);

  auto s = test_metric_manager::instance().serialize_dynamic();
  std::cout << s;
  CHECK(!s.empty());
  CHECK(s.find("get_count") != std::string::npos);
  CHECK(s.find("get_count1") != std::string::npos);
  CHECK(s.find("get_count2") != std::string::npos);
  CHECK(s.find("get_count3") == std::string::npos);

#ifdef CINATRA_ENABLE_METRIC_JSON
  auto json = test_metric_manager::instance().serialize_to_json_dynamic();
  std::cout << json << "\n";
  CHECK(!json.empty());
  CHECK(json.find("get_count") != std::string::npos);
  CHECK(json.find("get_count1") != std::string::npos);
  CHECK(json.find("get_count2") != std::string::npos);
#endif
}

#if defined(__GNUC__)
TEST_CASE("test system metric") {
  start_system_metric();
  metric::detail::ylt_stat();

  auto s = system_metric_manager::instance().serialize_static();
  std::cout << s;
  CHECK(!s.empty());

#ifdef CINATRA_ENABLE_METRIC_JSON
  auto json = system_metric_manager::instance().serialize_to_json_static();
  std::cout << json << "\n";
  CHECK(!json.empty());
#endif

  using metric_manager = dynamic_metric_manager<test_id_t<21>>;
  auto c = metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
      std::string("test_qps"), "", std::array<std::string, 1>{"url"});
  c.second->inc({"/"}, 42);
  using root = metric_collector_t<metric_manager, default_static_metric_manager,
                                  system_metric_manager>;
  s.clear();
  s = root::serialize();
  std::cout << s;
  CHECK(!s.empty());

#ifdef CINATRA_ENABLE_METRIC_JSON
  json.clear();
  json = root::serialize_to_json();
  std::cout << json << "\n";
  CHECK(!json.empty());
#endif
}

TEST_CASE("test metric capacity") {
  std::cout << g_user_metric_count << "\n";
  using test_metric_manager = dynamic_metric_manager<test_id_t<21>>;
  set_metric_capacity(g_user_metric_count + 2);
  auto c =
      test_metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
          std::string("counter"), "", std::array<std::string, 1>{});
  CHECK(c.second != nullptr);
  auto c1 =
      test_metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
          std::string("counter1"), "", std::array<std::string, 1>{});
  CHECK(c1.second != nullptr);
  auto c2 =
      test_metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
          std::string("counter2"), "", std::array<std::string, 1>{});
  CHECK(c2.second == nullptr);
  set_metric_capacity(10000000);

  auto process_memory_resident =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_memory_resident");
  std::cout << (int64_t)process_memory_resident->value() << "\n";

  auto process_memory_virtual =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_memory_virtual");
  std::cout << (int64_t)process_memory_virtual->value() << "\n";
}
#endif

TEST_CASE("test remove dynamic metric") {
  using test_metric_manager = dynamic_metric_manager<test_id_t<22>>;
  auto pair =
      test_metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
          std::string("counter"), "", std::array<std::string, 1>{});
  CHECK(pair.second != nullptr);
  auto pair1 =
      test_metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
          std::string("counter1"), "", std::array<std::string, 1>{});
  CHECK(pair1.second != nullptr);
  auto pair2 =
      test_metric_manager::instance().create_metric_dynamic<dynamic_counter_1t>(
          std::string("counter2"), "", std::array<std::string, 1>{});
  CHECK(pair2.second != nullptr);

  auto c = pair.second;
  auto c1 = pair1.second;
  auto c2 = pair2.second;

  test_metric_manager::instance().remove_metric(c);
  CHECK(test_metric_manager::instance().metric_count() == 2);
  test_metric_manager::instance().remove_metric(c1);
  CHECK(test_metric_manager::instance().metric_count() == 1);
  test_metric_manager::instance().remove_metric(c2);
  CHECK(test_metric_manager::instance().metric_count() == 0);

  test_metric_manager::instance().register_metric({c, c1, c2});
  CHECK(test_metric_manager::instance().metric_count() == 3);
  test_metric_manager::instance().remove_metric("counter");
  CHECK(test_metric_manager::instance().metric_count() == 2);
  test_metric_manager::instance().remove_metric(
      std::vector<std::string>{"counter1", "counter2"});
  CHECK(test_metric_manager::instance().metric_count() == 0);

  test_metric_manager::instance().register_metric({c, c1, c2});
  CHECK(test_metric_manager::instance().metric_count() == 3);
  test_metric_manager::instance().remove_metric({c1, c2});
  CHECK(test_metric_manager::instance().metric_count() == 1);
  auto r = test_metric_manager::instance().register_metric({c, c1});
  CHECK(!r);
  CHECK(test_metric_manager::instance().metric_count() == 1);

  r = test_metric_manager::instance().register_metric({c1, c});
  CHECK(!r);
  CHECK(test_metric_manager::instance().metric_count() == 2);
}

TEST_CASE("testFilterMetricsDynamicWithMultiLabel") {
  using metric_mgr = dynamic_metric_manager<test_id_t<31>>;
  auto [ec, c] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "test_dynamic_counter", "",
          std::array<std::string, 2>{"method", "bucket"});
  auto [ec2, c2] =
      metric_mgr::instance().create_metric_dynamic<dynamic_counter_t>(
          "test_dynamic_counter2", "",
          std::array<std::string, 2>{"url", "bucket"});
  c->inc({"GET", "bucket1"});
  c->inc({"POST", "bucket2"});
  c2->inc({"/", "bucket1"});
  c2->inc({"/test", "bucket3"});

  auto counter = metric_mgr::instance().get_metric_dynamic<dynamic_counter_t>(
      "test_dynamic_counter");
  CHECK(counter != nullptr);

  metric_filter_options options;
  options.name_regex = ".*counter.*";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 2);

    auto s = metric_mgr::instance().serialize(metrics);
    CHECK(s.find("test_dynamic_counter") != std::string::npos);
    std::cout << s << "\n";
  }

  options.label_regex = "bucket";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 2);
    auto s = metric_mgr::instance().serialize(metrics);
    CHECK(s.find("test_dynamic_counter2") != std::string::npos);
    std::cout << s << "\n";
  }

  options.label_regex = "method";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 1);
    auto s = metric_mgr::instance().serialize(metrics);
    CHECK(s.find("test_dynamic_counter") != std::string::npos);
    std::cout << s << "\n";
  }

  options.label_regex = "url";
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 1);
    auto s = metric_mgr::instance().serialize(metrics);
    CHECK(s.find("test_dynamic_counter2") != std::string::npos);
    std::cout << s << "\n";
  }

  // black
  options.label_regex = ".*bucket.*";
  options.is_white = false;
  {
    auto metrics = metric_mgr::instance().filter_metrics_dynamic(options);
    CHECK(metrics.size() == 0);
  }
}

TEST_CASE("test metric manager clean expired label") {
  set_label_max_age(std::chrono::seconds(1), std::chrono::seconds(1));
  auto& inst = dynamic_metric_manager<test_tag>::instance();
  auto pair = inst.create_metric_dynamic<dynamic_counter_1t>(
      std::string("some_counter"), "", std::array<std::string, 1>{"url"});
  auto c = pair.second;
  c->inc({"/"});
  c->inc({"/test"});
  CHECK(c->label_value_count() == 2);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  c->inc({"/index"});
  size_t count = c->label_value_count();
  CHECK(count == 1);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char** argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP