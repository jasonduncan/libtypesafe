#include "util.hpp"

#include <array>

namespace libtypesafe::detail {

std::optional<double> parse_decimal(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  bool negative = false;
  if (s[i] == '+' || s[i] == '-') {
    negative = s[i] == '-';
    ++i;
  }
  double value = 0;
  bool digits = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    value = value * 10 + (s[i] - '0');
    digits = true;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    double scale = 0.1;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      value += (s[i] - '0') * scale;
      scale /= 10;
      digits = true;
    }
  }
  if (!digits || i != s.size()) return std::nullopt;
  return negative ? -value : value;
}

namespace {

// Howard Hinnant's days_from_civil: days since 1970-01-01 in the proleptic Gregorian calendar.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool read_int(std::string_view s, std::size_t pos, std::size_t len, int& out) {
  if (pos + len > s.size()) return false;
  out = 0;
  for (std::size_t i = pos; i < pos + len; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + (s[i] - '0');
  }
  return true;
}

}  // namespace

std::optional<std::int64_t> parse_http_date(std::string_view s) {
  // "Sun, 06 Nov 1994 08:49:37 GMT"
  s = trim(s);
  const std::size_t comma = s.find(", ");
  if (comma == std::string_view::npos) return std::nullopt;
  s.remove_prefix(comma + 2);
  // "06 Nov 1994 08:49:37 GMT": day 0-1, month 3-5, year 7-10, time 12-19, zone 21-23.
  if (s.size() != 24 || s[2] != ' ' || s[6] != ' ' || s[11] != ' ' || s[14] != ':' ||
      s[17] != ':' || s[20] != ' ' || s.substr(21) != "GMT") {
    return std::nullopt;
  }
  static constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  unsigned month = 0;
  for (unsigned i = 0; i < months.size(); ++i) {
    if (s.substr(3, 3) == months[i]) month = i + 1;
  }
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (month == 0 || !read_int(s, 0, 2, day) || !read_int(s, 7, 4, year) || !read_int(s, 12, 2, hour) ||
      !read_int(s, 15, 2, minute) || !read_int(s, 18, 2, second)) {
    return std::nullopt;
  }
  if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return std::nullopt;
  return days_from_civil(year, month, static_cast<unsigned>(day)) * 86400 + hour * 3600 + minute * 60 +
         second;
}

std::string truncate_utf8(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return std::string(s);
  std::size_t cut = max_bytes;
  // Back off continuation bytes (10xxxxxx) so a multi-byte character is not split.
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return std::string(s.substr(0, cut)) + "\xE2\x80\xA6";  // …
}

std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<Json> parse_json(std::string_view text) {
  Json parsed = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) return std::nullopt;
  return parsed;
}

Json body_value(std::string_view text) {
  if (text.empty()) return nullptr;
  if (auto parsed = parse_json(text)) return std::move(*parsed);
  return Json(std::string(text));
}

std::string format_ms(milliseconds ms) { return std::to_string(ms.count()) + "ms"; }

}  // namespace libtypesafe::detail
