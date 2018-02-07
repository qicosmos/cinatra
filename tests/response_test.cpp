#include "unit_test.hpp"
#include <cinatra/response.hpp>

TEST_CASE(response) {
	cinatra::response res;
	TEST_CHECK(res.get_header_value("Content") == "");
	res.add_header("Content", "cinatra");
	TEST_CHECK(res.get_header_value("Content") == "cinatra");
};

