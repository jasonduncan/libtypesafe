#include <catch2/catch_test_macros.hpp>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;

TEST_CASE("Headers look up names case-insensitively") {
  ts::Headers headers{{"Content-Type", "application/json"}};
  CHECK(headers.get("content-type") == "application/json");
  CHECK(headers.contains("CONTENT-TYPE"));
  CHECK_FALSE(headers.get("accept"));
}

TEST_CASE("Headers::set replaces any casing in place, last write wins") {
  ts::Headers headers{{"A", "1"}, {"B", "2"}};
  headers.set("a", "3");
  REQUIRE(headers.size() == 2);
  CHECK(headers.begin()->first == "a");
  CHECK(headers.begin()->second == "3");
}

TEST_CASE("Headers::merge and erase") {
  ts::Headers base{{"X-One", "1"}, {"X-Two", "2"}};
  base.merge({{"x-two", "override"}, {"X-Three", "3"}});
  CHECK(base.get("X-Two") == "override");
  CHECK(base.get("X-Three") == "3");
  base.erase("X-ONE");
  CHECK_FALSE(base.contains("x-one"));
  CHECK(base.size() == 2);
}
