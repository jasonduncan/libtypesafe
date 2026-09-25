#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "libtypesafe/json.hpp"

namespace libtypesafe::detail {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

inline std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = ascii_lower(c);
  return out;
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

inline std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

/// `[+-]?digits[.digits]` (either side of the point may be empty, not both), surrounding
/// whitespace allowed. Locale-independent. Returns nullopt for anything else.
std::optional<double> parse_decimal(std::string_view s);

/// IMF-fixdate (`Sun, 06 Nov 1994 08:49:37 GMT`) as seconds since the Unix epoch.
std::optional<std::int64_t> parse_http_date(std::string_view s);

/// At most `max_bytes` of `s`, cut on a UTF-8 character boundary, with "…" appended when cut.
std::string truncate_utf8(std::string_view s, std::size_t max_bytes);

/// Compact JSON. Invalid UTF-8 in strings is replaced rather than aborting (nlohmann would throw,
/// which is an abort with exceptions disabled).
std::string dump(const Json& value);

/// Parse JSON without throwing; nullopt when `text` is not JSON.
std::optional<Json> parse_json(std::string_view text);

/// A response body as the SDK reports it: parsed JSON, the raw text as a JSON string, or null.
Json body_value(std::string_view text);

std::string format_ms(milliseconds ms);

inline milliseconds to_ms(Clock::duration d) { return std::chrono::duration_cast<milliseconds>(d); }

}  // namespace libtypesafe::detail
