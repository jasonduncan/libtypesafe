#include <map>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libtypesafe/libtypesafe.hpp>

#include "config.hpp"

namespace ts = libtypesafe;
using namespace std::chrono_literals;

namespace {

ts::detail::EnvReader env_of(std::map<std::string, std::string> values) {
  return [values](const char* name) -> std::optional<std::string> {
    const auto it = values.find(name);
    if (it == values.end()) return std::nullopt;
    return it->second;
  };
}

ts::Result<ts::detail::Config> resolve(ts::ClientOptions options, std::map<std::string, std::string> env = {}) {
  return ts::detail::resolve_config(std::move(options), env_of(std::move(env)));
}

}  // namespace

TEST_CASE("defaults and environment fallbacks") {
  auto config = resolve({}, {{"TYPESAFE_API_KEY", "  env-key  "}, {"TYPESAFE_BASE_URL", "https://proxy.local//"},
                            {"TYPESAFE_DEFAULT_MODEL", "jev-1"}, {"TYPESAFE_LOG_LEVEL", "WARNING"}});
  REQUIRE(config);
  CHECK(config->api_key == "env-key");
  CHECK(config->base_url == "https://proxy.local");
  CHECK(config->default_model == "jev-1");
  CHECK(config->log_level == ts::LogLevel::warn);
  CHECK(config->timeout == 10s);

  auto defaults = resolve({}, {{"TYPESAFE_API_KEY", "k"}});
  REQUIRE(defaults);
  CHECK(defaults->base_url == "https://api.typesafe.ai");
  CHECK(defaults->default_model == "jev-latest");
  CHECK(defaults->log_level == ts::LogLevel::warn);
}

TEST_CASE("explicit options win; blank environment values are ignored") {
  ts::ClientOptions options;
  options.api_key = "explicit";
  options.default_model = "mine";
  auto config = resolve(options, {{"TYPESAFE_API_KEY", "env"}, {"TYPESAFE_DEFAULT_MODEL", "theirs"},
                                  {"TYPESAFE_BASE_URL", "   "}});
  REQUIRE(config);
  CHECK(config->api_key == "explicit");
  CHECK(config->default_model == "mine");
  CHECK(config->base_url == "https://api.typesafe.ai");
}

TEST_CASE("API key validation never echoes the key") {
  auto missing = resolve({});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().kind == ts::ErrorKind::invalid_argument);
  CHECK_THAT(missing.error().message, Catch::Matchers::ContainsSubstring("TYPESAFE_API_KEY"));

  ts::ClientOptions spaced;
  spaced.api_key = "secret part2";
  auto bad = resolve(spaced);
  REQUIRE_FALSE(bad);
  CHECK_THAT(bad.error().message, !Catch::Matchers::ContainsSubstring("secret"));

  ts::ClientOptions empty;
  empty.api_key = "";
  CHECK_FALSE(resolve(empty, {{"TYPESAFE_API_KEY", "env"}}));  // explicit empty is not a fallback
}

TEST_CASE("invalid settings are rejected") {
  const auto rejected = [](ts::ClientOptions options, std::map<std::string, std::string> env = {}) {
    if (!options.api_key) options.api_key = "k";
    auto config = resolve(std::move(options), std::move(env));
    return config ? std::string() : config.error().message;
  };
  CHECK_THAT(rejected({}, {{"TYPESAFE_LOG_LEVEL", "loud"}}), Catch::Matchers::ContainsSubstring("Invalid log level"));
  ts::ClientOptions bad_url;
  bad_url.base_url = "api.typesafe.ai";
  CHECK_THAT(rejected(bad_url), Catch::Matchers::ContainsSubstring("base_url"));
  ts::ClientOptions bad_timeout;
  bad_timeout.timeout = 0ms;
  CHECK(rejected(bad_timeout) == "timeout must be positive.");
  ts::ClientOptions bad_retry;
  bad_retry.retry.backoff_jitter = 1.5;
  CHECK(rejected(bad_retry) == "retry.backoff_jitter must be between 0 and 1.");
  ts::ClientOptions bad_status;
  bad_status.retry.http_statuses = {42};
  CHECK_THAT(rejected(bad_status), Catch::Matchers::ContainsSubstring("42"));
  ts::ClientOptions bad_budget;
  bad_budget.retry.total_budget = 0ms;
  CHECK(rejected(bad_budget) == "retry.total_budget must be positive.");
}
