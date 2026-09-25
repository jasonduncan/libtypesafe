#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "libtypesafe/json.hpp"

namespace libtypesafe {

struct HttpRequest {
  std::string method;  // "GET" or "POST"
  std::string url;
  Headers headers;
  std::string body;  // empty for GET
  /// Limit for the whole attempt, including reading the response body.
  std::chrono::milliseconds timeout{10'000};
};

struct HttpResponse {
  int status = 0;
  Headers headers;
  std::string body;
};

struct TransportError {
  enum class Kind { connection, timeout, cancelled };
  Kind kind = Kind::connection;
  std::string message;
  /// Set to false only when certain the server never received the whole request (e.g. DNS or
  /// connect failure). Unknown means true. Drives `RetryPolicy::retry_after_send`.
  bool request_sent = true;
};

using TransportOutcome = std::variant<HttpResponse, TransportError>;
using TransferId = std::uint64_t;

/// Carries HTTP attempts for a Client. Implement this to use a different HTTP stack, or a fake in
/// tests. The client owns retries, error mapping, and logging.
class Transport {
 public:
  virtual ~Transport() = default;

  /// Start one attempt without blocking. Call `done` exactly once, from any thread, with every
  /// HTTP response (including non-2xx) or a TransportError. The client queues it and handles it
  /// in its next `pump()`.
  virtual TransferId start(HttpRequest request, std::function<void(TransportOutcome)> done) = 0;

  /// Stop a transfer early. `done` must still be called, with `Kind::cancelled`.
  virtual void cancel(TransferId id) = 0;

  /// Called at the start of every `Client::pump()`, on the client's thread. Must not block.
  /// Transports without their own thread do their I/O here.
  virtual void poll() {}

  /// Used only by the blocking calls: wait up to `max_wait` for transfer activity.
  /// The default sleeps for `max_wait`, capped at 10 ms.
  virtual void wait(std::chrono::milliseconds max_wait);
};

struct CurlOptions {
  std::chrono::milliseconds connect_timeout{5'000};
  /// Otherwise libcurl reads the usual proxy environment variables.
  std::optional<std::string> proxy;
  /// Otherwise the system trust store.
  std::optional<std::string> ca_bundle;
  bool http2 = true;
  /// Call `curl_global_init` once. Set false if the host application already does.
  bool global_init = true;
};

/// The default transport: one `curl_multi` handle driven from `poll()`. It creates no threads
/// (libcurl's threaded DNS resolver aside).
[[nodiscard]] std::shared_ptr<Transport> make_curl_transport(CurlOptions options = {});

}  // namespace libtypesafe
