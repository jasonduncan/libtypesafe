#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "libtypesafe/json.hpp"

/// Checks preconditions such as `Result::value()` on an error. Define before including to route
/// failures to a host application's own check macro.
#ifndef LIBTYPESAFE_ASSERT
#include <cassert>
#define LIBTYPESAFE_ASSERT(condition, message) assert((condition) && (message))
#endif

namespace libtypesafe {

enum class ErrorKind {
  invalid_argument,      ///< Bad options or questions; nothing was sent.
  bad_request,           ///< HTTP 400.
  authentication,        ///< HTTP 401.
  permission_denied,     ///< HTTP 403.
  not_found,             ///< HTTP 404.
  unprocessable_entity,  ///< HTTP 422: the server rejected the request body.
  rate_limited,          ///< HTTP 429.
  server_error,          ///< HTTP 5xx.
  http_error,            ///< Any other non-2xx status.
  response_validation,   ///< 2xx body missing data or not answering the questions asked.
  connection,            ///< No response: DNS, TLS, connection reset, etc.
  timeout,               ///< An attempt exceeded its timeout.
  cancelled,             ///< `Pending::cancel()`, the handle was dropped, or the client was destroyed.
};

[[nodiscard]] std::string_view to_string(ErrorKind kind) noexcept;

/// Every failure the SDK reports. The SDK never throws.
struct Error {
  ErrorKind kind = ErrorKind::invalid_argument;
  /// Full description, e.g.
  /// `POST https://api.typesafe.ai/v1/systemone: 422 questions.x.criteria: Field required (request_id=abc)`.
  /// Never contains the API key.
  std::string message;
  /// HTTP status of the last attempt; 0 when no response arrived.
  int status = 0;
  /// Parsed JSON body, the raw text as a JSON string, or null.
  Json body;
  Headers headers;
  std::optional<std::string> request_id;
  /// `response_validation` only: dotted path to the first bad field, e.g. `answers.tone.confidence`.
  std::string field_path;
  /// From `retry-after-ms` or `Retry-After`, when present and valid.
  std::optional<std::chrono::milliseconds> retry_after;
  /// `connection` / `timeout`: false only when the whole request certainly never reached the server.
  bool request_sent = true;
  /// Attempts made, including the first.
  int attempts = 0;

  [[nodiscard]] bool has_response() const noexcept { return status != 0; }
};

/// A value or an Error. Accessing the wrong side is a precondition failure (LIBTYPESAFE_ASSERT).
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return *checked_value(); }
  [[nodiscard]] const T& value() const& { return *checked_value(); }
  [[nodiscard]] T&& value() && { return std::move(*checked_value()); }
  T* operator->() { return checked_value(); }
  const T* operator->() const { return checked_value(); }
  T& operator*() & { return *checked_value(); }
  const T& operator*() const& { return *checked_value(); }

  [[nodiscard]] const Error& error() const& {
    LIBTYPESAFE_ASSERT(!ok(), "Result::error() called on a value");
    return *std::get_if<1>(&v_);
  }

  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return ok() ? *std::get_if<0>(&v_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  T* checked_value() {
    LIBTYPESAFE_ASSERT(ok(), "Result::value() called on an error");
    return std::get_if<0>(&v_);
  }
  const T* checked_value() const {
    LIBTYPESAFE_ASSERT(ok(), "Result::value() called on an error");
    return std::get_if<0>(&v_);
  }

  std::variant<T, Error> v_;
};

}  // namespace libtypesafe
