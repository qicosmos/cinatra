#include <stdexcept>
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include "cinatra/metric/counter.hpp"
using namespace cinatra;

TEST_CASE("test counter") {
    counter_t c("get_count", "get counter", {});
    c.inc();
    CHECK(c.values().begin()->second.value==1);
    c.inc();
    CHECK(c.values().begin()->second.value==2);
    c.inc({}, 0);
    CHECK(c.values().begin()->second.value==2);

    CHECK_THROWS_AS(c.inc({}, -2), std::invalid_argument);

    c.update({}, 10);
    CHECK(c.values().begin()->second.value==10);

    c.update({}, 0);
    CHECK(c.values().begin()->second.value==0);
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP