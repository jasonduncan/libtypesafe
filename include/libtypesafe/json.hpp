#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace libtypesafe {

/// JSON value for state, instructions, and criteria.
///
/// Ordered so object keys keep the order the caller wrote, as Python dicts and JS objects do.
/// A `nlohmann::json` converts implicitly (its keys arrive already sorted). With exceptions
/// disabled, nlohmann aborts on misuse such as `at()` with a missing key; the SDK itself only uses
/// its non-throwing calls.
using Json = nlohmann::ordered_json;

/// HTTP headers: insertion-ordered, case-insensitive lookup, last write wins.
class Headers {
 public:
  Headers() = default;
  Headers(std::initializer_list<std::pair<std::string, std::string>> entries);

  /// Replace any existing header with the same name, ignoring case.
  void set(std::string name, std::string value);
  void erase(std::string_view name);
  [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const;
  [[nodiscard]] bool contains(std::string_view name) const { return get(name).has_value(); }

  /// Apply `other` over this set; later values win.
  void merge(const Headers& other);

  [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] auto end() const noexcept { return entries_.end(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

}  // namespace libtypesafe
