#include "config.hpp"

#include <cmath>
#include <cstdlib>

#include "logging.hpp"
#include "util.hpp"

namespace libtypesafe::detail {

namespace {

Error invalid(std::string message) {
  Error error;
  error.kind = ErrorKind::invalid_argument;
  error.message = std::move(message);
  return error;
}

/// Explicit value, else a non-blank environment value, else nullopt.
std::optional<std::string> from_code_or_env(std::optional<std::string> explicit_value, const char* name,
                                            const EnvReader& env) {
  if (explicit_value) return explicit_value;
  if (auto value = env(name)) {
    const auto trimmed = trim(*value);
    if (!trimmed.empty()) return std::string(trimmed);
  }
  return std::nullopt;
}

}  // namespace

EnvReader process_env() {
  return [](const char* name) -> std::optional<std::string> {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
  };
}

std::optional<std::string> validate_retry(const RetryPolicy& policy) {
  if (policy.max_retries < 0) return "retry.max_retries must be a non-negative integer.";
  if (policy.backoff_initial.count() < 0) return "retry.backoff_initial must not be negative.";
  if (policy.backoff_max.count() < 0) return "retry.backoff_max must not be negative.";
  if (!std::isfinite(policy.backoff_jitter) || policy.backoff_jitter < 0 || policy.backoff_jitter > 1) {
    return "retry.backoff_jitter must be between 0 and 1.";
  }
  for (int status : policy.http_statuses) {
    if (status < 100 || status > 999) {
      return "retry.http_statuses must contain HTTP status codes, got " + std::to_string(status) + ".";
    }
  }
  if (policy.max_retry_after.count() < 0) return "retry.max_retry_after must not be negative.";
  if (policy.total_budget && policy.total_budget->count() <= 0) return "retry.total_budget must be positive.";
  return std::nullopt;
}

Result<Config> resolve_config(ClientOptions options, const EnvReader& env) {
  Config config;

  // The key is never included in a message.
  auto key = from_code_or_env(std::move(options.api_key), env::api_key, env);
  const std::string_view trimmed_key = key ? trim(*key) : std::string_view();
  if (trimmed_key.empty()) {
    return invalid(std::string("No API key was provided. Set ClientOptions::api_key or the ") + env::api_key +
                   " environment variable.");
  }
  for (char c : trimmed_key) {
    if (c < 0x21 || c > 0x7E) {
      return invalid("API key must contain only printable ASCII characters without whitespace.");
    }
  }
  config.api_key = std::string(trimmed_key);

  auto base_url = from_code_or_env(std::move(options.base_url), env::base_url, env)
                      .value_or(std::string(default_base_url));
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  const std::string lower_url = to_lower(base_url);
  if (lower_url.rfind("https://", 0) != 0 && lower_url.rfind("http://", 0) != 0) {
    return invalid("base_url must start with http:// or https://.");
  }
  config.base_url = std::move(base_url);

  config.default_model = from_code_or_env(std::move(options.default_model), env::default_model, env)
                             .value_or(std::string(libtypesafe::default_model));

  if (options.log_level) {
    config.log_level = *options.log_level;
  } else if (auto text = from_code_or_env(std::nullopt, env::log_level, env)) {
    auto level = parse_log_level(*text);
    if (!level) {
      return invalid(std::string("Invalid log level \"") + *text + "\" from " + env::log_level +
                     ". Expected one of: debug, info, warn, error, off.");
    }
    config.log_level = *level;
  }
  config.logger = options.logger ? std::move(options.logger) : stderr_sink();

  if (auto problem = validate_retry(options.retry)) return invalid(std::move(*problem));
  config.retry = std::move(options.retry);

  if (options.timeout.count() <= 0) return invalid("timeout must be positive.");
  config.timeout = options.timeout;
  config.default_headers = std::move(options.default_headers);
  config.transport = std::move(options.transport);
  return config;
}

}  // namespace libtypesafe::detail
