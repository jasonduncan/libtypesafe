#include "libtypesafe/json.hpp"

#include "util.hpp"

namespace libtypesafe {

Headers::Headers(std::initializer_list<std::pair<std::string, std::string>> entries) {
  for (const auto& [name, value] : entries) set(name, value);
}

void Headers::set(std::string name, std::string value) {
  for (auto& entry : entries_) {
    if (detail::iequals(entry.first, name)) {
      entry.first = std::move(name);
      entry.second = std::move(value);
      return;
    }
  }
  entries_.emplace_back(std::move(name), std::move(value));
}

void Headers::erase(std::string_view name) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (detail::iequals(it->first, name)) {
      entries_.erase(it);
      return;
    }
  }
}

std::optional<std::string_view> Headers::get(std::string_view name) const {
  for (const auto& [key, value] : entries_) {
    if (detail::iequals(key, name)) return std::string_view(value);
  }
  return std::nullopt;
}

void Headers::merge(const Headers& other) {
  for (const auto& [name, value] : other) set(name, value);
}

}  // namespace libtypesafe
