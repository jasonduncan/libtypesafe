// Calls the real TypeSafe API. Skipped unless both TYPESAFE_API_KEY and LIBTYPESAFE_LIVE=1 are set.
// Each test prints what the server actually returned, prefixed "live:".
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;
using namespace std::chrono_literals;

namespace {

enum class Tone { calm, frustrated, angry };

bool live_enabled() {
  const char* live = std::getenv("LIBTYPESAFE_LIVE");
  return std::getenv("TYPESAFE_API_KEY") != nullptr && live != nullptr && std::string(live) == "1";
}

#define REQUIRE_LIVE()                                                  \
  do {                                                                  \
    if (!live_enabled()) SKIP("set TYPESAFE_API_KEY and LIBTYPESAFE_LIVE=1 to run"); \
  } while (0)

ts::Client make_client(ts::ClientOptions options = {}) {
  if (!options.log_level) options.log_level = ts::LogLevel::warn;
  auto client = ts::Client::create(std::move(options));
  if (!client) FAIL(client.error().message);
  return std::move(client).value();
}

double sum(const std::vector<double>& values) { return std::accumulate(values.begin(), values.end(), 0.0); }

double sum(const std::vector<std::pair<std::string, double>>& values) {
  double total = 0;
  for (const auto& [label, p] : values) total += p;
  return total;
}

void report(const ts::Error& error) {
  std::cout << "live:   kind=" << ts::to_string(error.kind) << " status=" << error.status
            << " attempts=" << error.attempts << "\nlive:   message=" << error.message << '\n';
}

}  // namespace

template <>
struct libtypesafe::choice_labels<Tone> {
  static constexpr std::array values{
      std::pair{Tone::calm, std::string_view{"calm"}},
      std::pair{Tone::frustrated, std::string_view{"frustrated"}},
      std::pair{Tone::angry, std::string_view{"angry"}},
  };
};

TEST_CASE("live: list models") {
  REQUIRE_LIVE();
  auto client = make_client();
  auto models = client.list_models();
  if (!models) FAIL(models.error().message);
  REQUIRE_FALSE(models->empty());
  for (const auto& model : *models) {
    std::cout << "live: model " << model.name << " (" << model.release_date << "): " << model.description << '\n';
    CHECK_FALSE(model.name.empty());
  }
}

TEST_CASE("live: every question type in one call") {
  REQUIRE_LIVE();
  auto client = make_client();
  ts::Questions q;
  auto billing = q.add("billing", ts::noul("Is this message about billing?",
                                           {"Charges, invoices, payments, or refunds", "Anything else"}));
  auto topic = q.add("topic", ts::choice("What is this about?", "billing", "technical", "other"));
  auto team = q.add("team", ts::choice("Which team should handle this?",
                                       {{"payments", "Charges and refunds"}, {"support", "Product problems"}}));
  auto tone = q.add("tone", ts::choice<Tone>("What is the customer's tone?"));
  auto urgency = q.add("urgency", ts::score("How urgent is this?", {"can wait", "this week", "today", "right now"}));

  const auto started = std::chrono::steady_clock::now();
  auto result = client.system_one(
      "I was charged twice for my subscription this month. Please refund one of them, this is really annoying.", q);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  if (!result) FAIL(result.error().message);
  const ts::SystemOneResult& r = *result;

  std::cout << "live: model=" << r.model() << " input_tokens=" << r.usage().input_tokens.value_or(-1)
            << " output_tokens=" << r.usage().output_tokens.value_or(-1)
            << " request_id=" << r.meta().request_id.value_or("(none)") << " elapsed=" << elapsed.count() << "ms\n";
  std::cout << "live: billing noul=" << r[billing].noul << '\n';
  std::cout << "live: topic=" << r[topic].choice << " confidence=" << r[topic].confidence << '\n';
  std::cout << "live: team=" << r[team].choice << " tone=" << ts::Json(r.find("tone") ? std::get<ts::ChoiceAnswer>(*r.find("tone")).choice : "")
            << " urgency=" << r[urgency].score << " (level " << r[urgency].level() << ")\n";

  CHECK_FALSE(r.model().empty());
  CHECK(r.usage().input_tokens.value_or(0) > 0);
  CHECK(r[billing].noul >= 0.0);
  CHECK(r[billing].noul <= 1.0);
  CHECK(r[billing].yes(0.8));  // an unambiguous billing message
  CHECK(r[topic].choice == "billing");
  CHECK(r[team].choice == "payments");
  CHECK(std::abs(sum(r[topic].probabilities) - 1.0) < 0.03);
  CHECK(r[topic].probabilities.size() == 3);
  CHECK(r[topic].probabilities[0].first == "billing");  // question order
  const auto typed_tone = r[tone];
  CHECK(typed_tone.choice != Tone::calm);
  CHECK(r[urgency].probabilities.size() == 4);
  CHECK(std::abs(sum(r[urgency].probabilities) - 1.0) < 0.05);
  CHECK(r[urgency].legend == std::vector<ts::Json>{"can wait", "this week", "today", "right now"});
  CHECK(r[urgency].score >= 0.0);
  CHECK(r[urgency].score <= 3.0);
}

TEST_CASE("live: JSON state and a raw question") {
  REQUIRE_LIVE();
  auto client = make_client();
  ts::Questions q;
  q.add_raw("spam", {{"type", "noul"}, {"instructions", "Is this message unsolicited advertising?"}});
  auto result = client.system_one(
      {{"subject", "Team lunch Friday"}, {"body", "Pizza at noon in the big conference room. Reply if vegetarian."}}, q);
  if (!result) FAIL(result.error().message);
  const auto* answer = result->find("spam");
  REQUIRE(answer != nullptr);
  const auto* spam = std::get_if<ts::NoulAnswer>(answer);
  REQUIRE(spam != nullptr);
  std::cout << "live: raw noul spam=" << spam->noul << '\n';
  CHECK(spam->noul < 0.5);
}

TEST_CASE("live: concurrent async calls over curl_multi") {
  REQUIRE_LIVE();
  auto client = make_client();
  const std::vector<std::string> messages = {
      "I was charged twice this month.",
      "The export button does nothing on Safari.",
      "Do you have a student discount?",
      "My invoice shows the wrong company name.",
      "The app crashes when I open settings.",
  };
  std::vector<ts::Key<ts::ChoiceAnswer>> keys;
  std::vector<ts::Pending<ts::SystemOneResult>> pending;
  int callbacks = 0;
  const auto started = std::chrono::steady_clock::now();
  for (const auto& message : messages) {
    ts::Questions q;
    keys.push_back(q.add("topic", ts::choice("What is this about?", "billing", "technical", "other")));
    pending.push_back(client.system_one_async(message, std::move(q)));
  }
  // One more through the callback path.
  ts::Questions extra;
  extra.add("billing", ts::noul("Is this about billing?"));
  client.system_one_async("Refund please", extra, {}, [&](ts::Result<ts::SystemOneResult> r) {
    ++callbacks;
    CHECK(r);
  }).detach();

  CHECK(client.in_flight() == messages.size() + 1);
  while (client.in_flight() > 0 && std::chrono::steady_clock::now() - started < 30s) {
    client.pump();
    std::this_thread::sleep_for(5ms);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  std::cout << "live: " << messages.size() + 1 << " concurrent calls finished in " << elapsed.count() << "ms\n";
  CHECK(callbacks == 1);
  // Only the unambiguous cases are asserted; the student-discount question is a real judgment call
  // (jev-1.13.0 says "billing").
  const std::vector<std::string> expected = {"billing", "technical", "", "billing", "technical"};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    REQUIRE(pending[i].done());
    auto result = pending[i].take();
    if (!result) FAIL(result.error().message);
    std::cout << "live:   \"" << messages[i] << "\" -> " << (*result)[keys[i]].choice << '\n';
    if (!expected[i].empty()) CHECK((*result)[keys[i]].choice == expected[i]);
  }
}

TEST_CASE("live: a bad key is an authentication error, not retried") {
  REQUIRE_LIVE();
  ts::ClientOptions options;
  options.api_key = "ts-libtypesafe-invalid-test-key";
  auto client = make_client(options);
  auto models = client.list_models();
  REQUIRE_FALSE(models);
  report(models.error());
  CHECK((models.error().kind == ts::ErrorKind::authentication || models.error().kind == ts::ErrorKind::permission_denied));
  CHECK(models.error().attempts == 1);
  CHECK_THAT(models.error().message, !Catch::Matchers::ContainsSubstring("ts-libtypesafe-invalid"));
}

TEST_CASE("live: the server's own validation errors are readable") {
  REQUIRE_LIVE();
  auto client = make_client();
  ts::Questions q;
  q.add_raw("broken", {{"type", "score"}, {"instructions", "Rate it"}, {"criteria", ts::Json::array()}});
  auto result = client.system_one("anything", q);
  REQUIRE_FALSE(result);
  report(result.error());
  CHECK(result.error().kind == ts::ErrorKind::unprocessable_entity);
  CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring("questions.broken"));
  CHECK(result.error().attempts == 1);
}

TEST_CASE("live: an unknown model fails cleanly") {
  REQUIRE_LIVE();
  auto client = make_client();
  ts::Questions q;
  q.add("b", ts::noul("Is this about billing?"));
  ts::CallOptions options;
  options.model = "libtypesafe-no-such-model";
  auto result = client.system_one("Refund please", q, options);
  REQUIRE_FALSE(result);
  report(result.error());
  CHECK(result.error().has_response());
}

TEST_CASE("live: a timeout that can't be met") {
  REQUIRE_LIVE();
  auto client = make_client();
  ts::Questions q;
  q.add("b", ts::noul("Is this about billing?"));
  ts::CallOptions options;
  options.timeout = 1ms;
  options.retry = ts::RetryPolicy{};
  options.retry->max_retries = 0;
  auto result = client.system_one("Refund please", q, options);
  REQUIRE_FALSE(result);
  report(result.error());
  std::cout << "live:   request_sent=" << result.error().request_sent << '\n';
  CHECK(result.error().kind == ts::ErrorKind::timeout);
}
