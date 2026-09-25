#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "libtypesafe/client.hpp"
#include "libtypesafe/json.hpp"

namespace libtypesafe::detail {

class Logger {
 public:
  Logger(LogLevel level, LogSink sink);

  [[nodiscard]] bool enabled(LogLevel level) const noexcept {
    return level_ != LogLevel::off && level != LogLevel::off && level >= level_ && sink_;
  }
  void write(LogLevel level, std::string_view message) const {
    if (enabled(level)) sink_(level, message);
  }

 private:
  LogLevel level_;
  LogSink sink_;
};

/// Lines prefixed `[libtypesafe]` on stderr.
LogSink stderr_sink();

std::optional<LogLevel> parse_log_level(std::string_view text);
std::string_view level_name(LogLevel level);

/// Whether a header's value must never be logged.
bool is_secret_header(std::string_view name);

/// `{Name: value, ...}` with secret values replaced by `***`.
std::string describe_headers(const Headers& headers);

}  // namespace libtypesafe::detail
