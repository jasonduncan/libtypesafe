#include <array>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <libtypesafe/libtypesafe.hpp>

#include "util.hpp"
#include "wire.hpp"

namespace ts = libtypesafe;

enum class Tone { calm, angry };

template <>
struct libtypesafe::choice_labels<Tone> {
  static constexpr std::array values{
      std::pair{Tone::calm, std::string_view{"calm"}},
      std::pair{Tone::angry, std::string_view{"angry"}},
  };
};

TEST_CASE("questions encode in insertion order with null fields omitted") {
  ts::Questions q;
  q.add("billing", ts::noul("Is this about billing?"));
  q.add("bare", ts::noul());
  q.add("spam", ts::noul("Spam?", {"Unsolicited advertising", nullptr}));
  q.add("tone", ts::choice("What is the tone?", {{"calm", "Neutral"}, {"angry", nullptr}}));
  q.add("urgency", ts::score("How urgent?", {"can wait", "today"}));

  CHECK(ts::detail::dump(q.to_wire()) ==
        R"({"billing":{"type":"noul","instructions":"Is this about billing?"},)"
        R"("bare":{"type":"noul"},)"
        R"("spam":{"type":"noul","instructions":"Spam?","criteria":{"true":"Unsolicited advertising"}},)"
        R"("tone":{"type":"choice","instructions":"What is the tone?","criteria":{"calm":"Neutral","angry":null}},)"
        R"("urgency":{"type":"score","instructions":"How urgent?","criteria":["can wait","today"]}})");
  CHECK_FALSE(q.error());
}

TEST_CASE("labels-only choice and enum choice") {
  ts::Questions q;
  q.add("topic", ts::choice("Topic?", "billing", "technical"));
  q.add("tone", ts::choice<Tone>("Tone?", {{Tone::angry, "Hostile"}}));
  CHECK(ts::detail::dump(q.to_wire()) ==
        R"({"topic":{"type":"choice","instructions":"Topic?","criteria":{"billing":null,"technical":null}},)"
        R"("tone":{"type":"choice","instructions":"Tone?","criteria":{"calm":null,"angry":"Hostile"}}})");
}

TEST_CASE("add records the first problem and skips the bad question") {
  SECTION("duplicate name") {
    ts::Questions q;
    q.add("x", ts::noul("a"));
    q.add("x", ts::noul("b"));
    REQUIRE(q.error());
    CHECK(q.error()->kind == ts::ErrorKind::invalid_argument);
    CHECK(q.error()->message == "Duplicate question name \"x\".");
    CHECK(q.size() == 1);
  }
  SECTION("empty choice, duplicate label, empty rubric, empty name") {
    ts::Questions q;
    q.add("c", ts::choice("?", std::vector<std::pair<std::string, ts::Json>>{}));
    q.add("d", ts::choice("?", "a", "a"));
    q.add("s", ts::score("?", {}));
    q.add("", ts::noul());
    REQUIRE(q.error());
    CHECK(q.error()->message == "Choice question \"c\" has no labels.");  // first one wins
    CHECK(q.empty());
  }
  SECTION("each problem on its own") {
    ts::Questions a, b, c;
    a.add("d", ts::choice("?", "a", "a"));
    b.add("s", ts::score("?", {}));
    c.add("", ts::noul());
    CHECK(a.error()->message == "Choice question \"d\" repeats the label \"a\".");
    CHECK(b.error()->message == "Score question \"s\" has no criteria; at least one score is required.");
    CHECK(c.error()->message == "Question names must be non-empty.");
  }
}

TEST_CASE("add_raw validates the shape the Python SDK validates") {
  ts::Questions ok;
  ok.add_raw("future", {{"type", "rank"}, {"candidates", {1, 2}}});
  CHECK_FALSE(ok.error());

  ts::Questions no_type, no_criteria;
  no_type.add_raw("x", {{"instructions", "?"}});
  no_criteria.add_raw("y", {{"type", "choice"}});
  CHECK(no_type.error()->message == "Question \"x\" must be an object with a nonempty string \"type\".");
  CHECK(no_criteria.error()->message == "Question \"y\" requires \"criteria\".");
}

TEST_CASE("request body: state, model, questions, then extra_body") {
  ts::Questions q;
  q.add("b", ts::noul("?"));
  auto body = ts::detail::Wire::encode_system_one({{"z", 1}, {"a", 2}}, q, "jev-latest",
                                                  {{"model", "override"}, {"trace", true}});
  REQUIRE(body);
  CHECK(*body == R"({"state":{"z":1,"a":2},"model":"override","questions":{"b":{"type":"noul","instructions":"?"}},"trace":true})");

  auto bad = ts::detail::Wire::encode_system_one("s", q, "m", ts::Json::array());
  REQUIRE_FALSE(bad);
  CHECK(bad.error().kind == ts::ErrorKind::invalid_argument);
}

TEST_CASE("invalid UTF-8 in state is replaced, not fatal") {
  ts::Questions q;
  q.add("b", ts::noul("?"));
  auto body = ts::detail::Wire::encode_system_one(std::string("bad \xFF byte"), q, "m", nullptr);
  REQUIRE(body);
  CHECK(body->find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD
}
