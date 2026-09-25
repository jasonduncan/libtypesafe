#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <set>

namespace libtypesafe {

struct Error;

/// HTTP 408, 429, and 500-599.
[[nodiscard]] inline std::set<int> default_retry_statuses() {
  std::set<int> statuses{408, 429};
  for (int s = 500; s < 600; ++s) statuses.insert(s);
  return statuses;
}

/// When and how to retry a failed attempt. Defaults match the official SDKs.
struct RetryPolicy {
  /// Retries after the first attempt; 0 disables retries.
  int max_retries = 2;
  /// First backoff delay, doubled per retry up to `backoff_max`.
  std::chrono::milliseconds backoff_initial{500};
  std::chrono::milliseconds backoff_max{5'000};
  /// Fraction of each backoff delay randomly subtracted, 0 to 1.
  double backoff_jitter = 0.25;
  std::set<int> http_statuses = default_retry_statuses();
  /// Honor `retry-after-ms` / `Retry-After` up to `max_retry_after`; longer waits fall back to backoff.
  bool respect_retry_after = true;
  std::chrono::milliseconds max_retry_after{60'000};
  bool retry_connection_errors = true;
  bool retry_timeouts = true;
  /// Whether a connection error or timeout that happened *after* the whole request was sent may be
  /// retried. The server may already have evaluated (and billed) that attempt, so a retry can
  /// charge twice. Failures before the request was sent are always safe to retry and are governed
  /// only by the two flags above. `true` matches the official SDKs.
  bool retry_after_send = true;
  /// Total time for one call, including attempts and waits. Stops before a retry whose wait would
  /// reach it and reports the last error. nullopt disables the budget.
  std::optional<std::chrono::milliseconds> total_budget = std::chrono::seconds{30};
  /// Extra rule: return true to retry an error the rules above would not.
  std::function<bool(const Error&)> predicate;
};

}  // namespace libtypesafe
