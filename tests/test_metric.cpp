#include "cinatra/metric/guage.hpp"
#define DOCTEST_CONFIG_IMPLEMENT
#include "cinatra/metric/counter.hpp"
#include "doctest/doctest.h"
using namespace cinatra;

TEST_CASE("test counter") {
  counter_t c("get_count", "get counter", {});
  c.inc();
  CHECK(c.values().begin()->second.value == 1);
  c.inc();
  CHECK(c.values().begin()->second.value == 2);
  c.inc({}, 0);

  CHECK(c.values().begin()->second.value == 2);

  CHECK_THROWS_AS(c.inc({}, -2), std::invalid_argument);

  c.update({}, 10);
  CHECK(c.values().begin()->second.value == 10);

  c.update({}, 0);
  CHECK(c.values().begin()->second.value == 0);

  c.inc({"GET", "200"}, 1);
  CHECK(c.values()[{"GET", "200"}].value == 1);
  c.inc({"GET", "200"}, 2);
  CHECK(c.values()[{"GET", "200"}].value == 3);

  c.update({"GET", "200"}, 20);
  CHECK(c.values()[{"GET", "200"}].value == 20);
  c.reset();
  CHECK(c.values()[{"GET", "200"}].value == 0);
  CHECK(c.values().begin()->second.value == 0);
}

TEST_CASE("test guage") {
  guage_t g("get_count", "get counter", {});
  g.inc();
  CHECK(g.values().begin()->second.value == 1);
  g.inc();
  CHECK(g.values().begin()->second.value == 2);
  g.inc({}, 0);

  g.dec();
  CHECK(g.values().begin()->second.value == 1);
  g.dec();
  CHECK(g.values().begin()->second.value == 0);

  g.inc({"GET", "200"}, 1);
  CHECK(g.values()[{"GET", "200"}].value == 1);
  g.inc({"GET", "200"}, 2);
  CHECK(g.values()[{"GET", "200"}].value == 3);

  g.dec({"GET", "200"}, 1);
  CHECK(g.values()[{"GET", "200"}].value == 2);
  g.dec({"GET", "200"}, 2);
  CHECK(g.values()[{"GET", "200"}].value == 0);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP