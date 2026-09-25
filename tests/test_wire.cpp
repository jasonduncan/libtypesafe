#include <array>
#include <chrono>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libtypesafe/libtypesafe.hpp>

#include "fakes.hpp"
#include "wire.hpp"

namespace ts = libtypesafe;
using ts::detail::Wire;
using namespace std::chrono_literals;

namespace {

enum class Tone { calm, angry };

ts::detail::DecodeContext context(std::vector<std::string>* warnings = nullptr) {
  return {"POST https://api.typesafe.ai/v1/systemone", 1, [warnings](std::string_view message) {
            if (warnings != nullptr) warnings->emplace_back(message);
          }};
}

ts::HttpResponse ok(const ts::Json& body) {
  return fakes::json_response(200, body, {{"x-typesafe-request-id", "req-1"}});
}

}  // namespace

template <>
struct libtypesafe::choice_labels<Tone> {
  static constexpr std::array values{
      std::pair{Tone::calm, std::string_view{"calm"}},
      std::pair{Tone::angry, std::string_view{"angry"}},
  };
};

TEST_CASE("decode a full System One response into typed answers") {
  ts::Questions q;
  auto billing = q.add("billing", ts::noul("?"));
  auto tone = q.add("tone", ts::choice<Tone>("?"));
  auto topic = q.add("topic", ts::choice("?", "billing", "technical", "other"));
  auto urgency = q.add("urgency", ts::score("?", {"low", "mid", "high"}));

  const ts::Json body = fakes::ok_body({
      {"billing", fakes::noul_answer(0.98)},
      {"tone", {{"type", "choice"}, {"choice", "angry"}, {"confidence", 0.8},
                {"probabilities", {{"angry", 0.8}, {"calm", 0.2}}}}},
      {"topic", {{"type", "choice"}, {"choice", "billing"}, {"confidence", 0.9},
                 {"probabilities", {{"other", 0.05}, {"technical", 0.05}, {"billing", 0.9}}}}},
      {"urgency", {{"type", "score"}, {"score", 1.7}, {"confidence", 0.7},
                   {"legend", {{"2", "high"}, {"0", "low"}, {"1", "mid"}}},
                   {"probabilities", {{"0", 0.1}, {"1", 0.1}, {"2", 0.8}}}}},
  });

  auto result = Wire::decode_system_one(ok(body), q, context());
  REQUIRE(result);
  const ts::SystemOneResult& r = *result;
  CHECK(r.model() == "jev-1.13.0");
  CHECK(r.usage().input_tokens == 12);
  CHECK(r.meta().request_id == "req-1");
  CHECK(r[billing].noul == 0.98);
  CHECK(r[billing].yes());

  const auto typed = r[tone];
  CHECK(typed.choice == Tone::angry);
  CHECK(typed.probability(Tone::calm) == 0.2);

  // Probabilities follow the question's label order, not the response's.
  REQUIRE(r[topic].probabilities.size() == 3);
  CHECK(r[topic].probabilities[0].first == "billing");
  CHECK(r[topic].probabilities[2].first == "other");

  // Score maps become vectors indexed by level.
  CHECK(r[urgency].legend == std::vector<ts::Json>{"low", "mid", "high"});
  CHECK(r[urgency].probabilities == std::vector<double>{0.1, 0.1, 0.8});
  CHECK(r[urgency].level() == 2);
}

TEST_CASE("unknown answer types are kept and warned about") {
  ts::Questions q;
  q.add_raw("ranking", {{"type", "rank"}, {"candidates", {"a", "b"}}});
  std::vector<std::string> warnings;
  auto result = Wire::decode_system_one(
      ok(fakes::ok_body({{"ranking", {{"type", "rank"}, {"order", {"b", "a"}}}}})), q, context(&warnings));
  REQUIRE(result);
  const auto* answer = result->find("ranking");
  REQUIRE(answer != nullptr);
  const auto* unknown = std::get_if<ts::UnknownAnswer>(answer);
  REQUIRE(unknown != nullptr);
  CHECK(unknown->type == "rank");
  CHECK(unknown->raw["order"][0] == "b");
  REQUIRE(warnings.size() == 1);
  CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("unrecognized type \"rank\""));
}

TEST_CASE("malformed or mismatched responses name the offending field") {
  ts::Questions q;
  q.add("b", ts::noul("?"));
  q.add("t", ts::choice("?", "x", "y"));
  q.add("s", ts::score("?", {"lo", "hi"}));
  const ts::Json good_t = {{"type", "choice"}, {"choice", "x"}, {"confidence", 0.9},
                           {"probabilities", {{"x", 0.9}, {"y", 0.1}}}};
  const ts::Json good_s = {{"type", "score"}, {"score", 0.5}, {"confidence", 0.5},
                           {"legend", {{"0", "lo"}, {"1", "hi"}}}, {"probabilities", {{"0", 0.5}, {"1", 0.5}}}};

  const auto path_of = [&](const ts::HttpResponse& response) {
    auto result = Wire::decode_system_one(response, q, context());
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ts::ErrorKind::response_validation);
    return result.error().field_path;
  };
  const auto with = [&](const char* name, ts::Json answer) {
    ts::Json answers = {{"b", fakes::noul_answer(0.5)}, {"t", good_t}, {"s", good_s}};
    if (answer.is_null()) {
      answers.erase(name);
    } else {
      answers[name] = std::move(answer);
    }
    return ok(fakes::ok_body(answers));
  };

  CHECK(path_of({200, {}, "not json"}) == "");
  CHECK(path_of(ok({{"answers", ts::Json::object()}, {"usage", ts::Json::object()}})) == "model");
  CHECK(path_of(ok({{"model", "m"}, {"answers", ts::Json::object()}})) == "usage");
  CHECK(path_of(ok({{"model", "m"}, {"usage", {{"input_tokens", "12"}}}})) == "usage.input_tokens");
  CHECK(path_of(with("b", nullptr)) == "answers.b");
  CHECK(path_of(with("b", good_t)) == "answers.b.type");
  CHECK(path_of(with("b", {{"type", "noul"}})) == "answers.b.noul");
  CHECK(path_of(with("t", {{"type", "choice"}, {"choice", "z"}, {"confidence", 1}, {"probabilities", {{"z", 1}}}})) ==
        "answers.t.choice");
  CHECK(path_of(with("t", {{"type", "choice"}, {"choice", "x"}, {"confidence", 1}, {"probabilities", {{"x", "high"}}}})) ==
        "answers.t.probabilities.x");
  CHECK(path_of(with("s", {{"type", "score"}, {"score", 0}, {"confidence", 1},
                           {"legend", {{"0", "lo"}, {"2", "hi"}}}, {"probabilities", {{"0", 1}, {"1", 0}}}})) ==
        "answers.s.legend");
  CHECK(path_of(with("s", {{"type", "score"}, {"score", 0}, {"confidence", 1}, {"legend", {{"0", "lo"}}},
                           {"probabilities", {{"0", 1}}}})) == "answers.s.legend");
  CHECK(path_of(with("s", {{"type", 7}})) == "answers.s.type");

  auto error = Wire::decode_system_one(with("b", nullptr), q, context());
  CHECK(error.error().message ==
        "POST https://api.typesafe.ai/v1/systemone: 200 Invalid response data at 'answers.b'. (request_id=req-1)");
  CHECK(error.error().status == 200);
}

TEST_CASE("null token counts are unknown, not zero") {
  ts::Questions q;
  q.add("b", ts::noul());
  auto result = Wire::decode_system_one(
      ok({{"model", "m"}, {"answers", {{"b", fakes::noul_answer(1)}}}, {"usage", {{"input_tokens", nullptr}}}}), q,
      context());
  REQUIRE(result);
  CHECK_FALSE(result->usage().input_tokens);
  CHECK_FALSE(result->usage().output_tokens);
}

TEST_CASE("decode models") {
  auto models = Wire::decode_models(
      ok({{"models", {{{"name", "jev-latest"}, {"description", "General."}, {"release_date", "2026-09-15"}}}}}),
      context());
  REQUIRE(models);
  REQUIRE(models->size() == 1);
  CHECK(models->front().name == "jev-latest");

  auto bad = Wire::decode_models(ok({{"models", {{{"name", "a"}, {"description", "d"}, {"release_date", "x"}},
                                                 {{"name", 1}}}}}),
                                 context());
  REQUIRE_FALSE(bad);
  CHECK(bad.error().field_path == "models[1].name");
}

TEST_CASE("status codes map to error kinds") {
  CHECK(Wire::kind_for_status(400) == ts::ErrorKind::bad_request);
  CHECK(Wire::kind_for_status(401) == ts::ErrorKind::authentication);
  CHECK(Wire::kind_for_status(403) == ts::ErrorKind::permission_denied);
  CHECK(Wire::kind_for_status(404) == ts::ErrorKind::not_found);
  CHECK(Wire::kind_for_status(422) == ts::ErrorKind::unprocessable_entity);
  CHECK(Wire::kind_for_status(429) == ts::ErrorKind::rate_limited);
  CHECK(Wire::kind_for_status(503) == ts::ErrorKind::server_error);
  CHECK(Wire::kind_for_status(409) == ts::ErrorKind::http_error);
}

TEST_CASE("error messages follow the official SDKs' extraction order") {
  using ts::Json;
  CHECK(Wire::extract_message("plain text") == "plain text");
  CHECK(Wire::extract_message({{"error", "e"}, {"message", "m"}}) == "e");
  CHECK(Wire::extract_message({{"error", {{"message", "nested"}}}}) == "nested");
  CHECK(Wire::extract_message({{"message", "m"}, {"detail", "d"}}) == "m");
  CHECK(Wire::extract_message({{"detail", "d"}}) == "d");
  CHECK(Wire::extract_message({{"detail", {{"message", "dm"}}}}) == "dm");
  CHECK(Wire::extract_message({{"detail",
                                {{{"loc", {"body", "questions", "x", "criteria"}}, {"msg", "Field required"}},
                                 {{"loc", {"body", "state"}}, {"msg", "bad"}},
                                 {{"msg", "no location"}}}}}) ==
        "questions.x.criteria: Field required; state: bad; no location");
  CHECK_FALSE(Wire::extract_message({{"other", 1}}));
  CHECK_FALSE(Wire::extract_message({{"error", ""}}));
}

TEST_CASE("api_error builds the full message and metadata") {
  auto context_ = context();
  const auto error = Wire::api_error(
      fakes::json_response(422, {{"detail", {{{"loc", {"body", "state"}}, {"msg", "Field required"}}}}},
                           {{"x-typesafe-request-id", "abc"}}),
      context_);
  CHECK(error.kind == ts::ErrorKind::unprocessable_entity);
  CHECK(error.status == 422);
  CHECK(error.request_id == "abc");
  CHECK(error.message == "POST https://api.typesafe.ai/v1/systemone: 422 state: Field required (request_id=abc)");

  const auto empty = Wire::api_error({500, {}, ""}, context_);
  CHECK(empty.message == "POST https://api.typesafe.ai/v1/systemone: 500 status code (no body)");
  CHECK(empty.body.is_null());

  // Plain text is the message, whole, as in both official SDKs.
  const auto text = Wire::api_error({502, {}, std::string(300, 'x')}, context_);
  CHECK(text.message == "POST https://api.typesafe.ai/v1/systemone: 502 " + std::string(300, 'x'));

  // JSON with no recognized message field is shown raw, cut to 200 bytes.
  const auto odd = Wire::api_error({502, {}, ts::Json{{"unexpected", std::string(300, 'x')}}.dump()}, context_);
  const std::string raw = ts::Json{{"unexpected", std::string(300, 'x')}}.dump();
  CHECK(odd.message == "POST https://api.typesafe.ai/v1/systemone: 502 " + raw.substr(0, 200) + "\xE2\x80\xA6");
}

TEST_CASE("Retry-After parsing") {
  const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(784111777));  // 1994-11-06 08:49:37 UTC
  const auto parse = [&](ts::Headers headers) { return Wire::parse_retry_after(headers, now); };
  CHECK(parse({{"retry-after-ms", "1234"}, {"Retry-After", "9"}}) == 1234ms);
  CHECK(parse({{"retry-after-ms", "12.6"}}) == 13ms);
  CHECK(parse({{"retry-after-ms", ""}}) == 0ms);
  CHECK(parse({{"retry-after-ms", "-5"}, {"Retry-After", "2"}}) == 2000ms);
  CHECK(parse({{"Retry-After", "1.5"}}) == 1500ms);
  CHECK(parse({{"Retry-After", "Sun, 06 Nov 1994 08:49:47 GMT"}}) == 10000ms);
  CHECK(parse({{"Retry-After", "Sun, 06 Nov 1994 08:49:00 GMT"}}) == 0ms);  // past dates mean now
  CHECK_FALSE(parse({{"Retry-After", "-1"}}));
  CHECK_FALSE(parse({{"Retry-After", "soon"}}));
  CHECK_FALSE(parse({}));
}

TEST_CASE("endpoints never show credentials") {
  CHECK(ts::detail::without_userinfo("https://user:pw@example.com/v1/systemone") ==
        "https://example.com/v1/systemone");
  CHECK(ts::detail::without_userinfo("https://example.com/a@b") == "https://example.com/a@b");
}
