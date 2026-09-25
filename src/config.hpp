#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "libtypesafe/client.hpp"

namespace libtypesafe::detail {

/// Validated client settings.
struct Config {
  std::string api_key;
  std::string base_url;
  std::string default_model;
  LogLevel log_level = LogLevel::warn;
  LogSink logger;
  RetryPolicy retry;
  std::chrono::milliseconds timeout = default_timeout;
  Headers default_headers;
  std::shared_ptr<Transport> transport;
};

/// Reads an environment variable; nullopt when unset.
using EnvReader = std::function<std::optional<std::string>(const char* name)>;

EnvReader process_env();

/// Resolve explicit options, then the environment, then defaults. Blank environment values are
/// ignored. Does not create a transport.
Result<Config> resolve_config(ClientOptions options, const EnvReader& env);

/// Why `policy` is invalid, or nullopt.
std::optional<std::string> validate_retry(const RetryPolicy& policy);

}  // namespace libtypesafe::detail
