#include "unit_test.hpp"
#include <cinatra/utils.hpp>
#include <string_view>
TEST_CASE(trim_left) {
	std::string str{"           left"};
	TEST_CHECK(cinatra::trim_left(str) == "left");
}

TEST_CASE(trim_right) {
	std::string str{"right            "};
	TEST_CHECK(cinatra::trim_right(str) == "right");
}
