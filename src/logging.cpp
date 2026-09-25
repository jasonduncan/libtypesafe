#include "logging.hpp"

#include <array>
#include <cstdio>

#include "util.hpp"

namespace libtypesafe::detail {

Logger::Logger(LogLevel level, LogSink sink) : level_(level), sink_(std::move(sink)) {}

LogSink stderr_sink() {
  return [](LogLevel level, std::string_view message) {
    const std::string_view name = level_name(level);
    std::fprintf(stderr, "[libtypesafe] %.*s: %.*s\n", static_cast<int>(name.size()), name.data(),
                 static_cast<int>(message.size()), message.data());
  };
}

std::optional<LogLevel> parse_log_level(std::string_view text) {
  const std::string lower = to_lower(trim(text));
  if (lower == "debug") return LogLevel::debug;
  if (lower == "info") return LogLevel::info;
  if (lower == "warn" || lower == "warning") return LogLevel::warn;
  if (lower == "error") return LogLevel::error;
  if (lower == "off") return LogLevel::off;
  return std::nullopt;
}

std::string_view level_name(LogLevel level) {
  switch (level) {
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warn: return "warn";
    case LogLevel::error: return "error";
    case LogLevel::off: return "off";
  }
  return "?";
}

bool is_secret_header(std::string_view name) {
  static constexpr std::array<std::string_view, 6> secret{
      "authorization", "proxy-authorization", "x-api-key", "api-key", "cookie", "set-cookie"};
  const std::string lower = to_lower(name);
  for (std::string_view candidate : secret) {
    if (lower == candidate) return true;
  }
  return lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos;
}

std::string describe_headers(const Headers& headers) {
  std::string out = "{";
  for (const auto& [name, value] : headers) {
    if (out.size() > 1) out += ", ";
    out += name;
    out += ": ";
    out += is_secret_header(name) ? std::string("***") : value;
  }
  out += "}";
  return out;
}

}  // namespace libtypesafe::detail
