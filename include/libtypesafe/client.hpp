#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtypesafe/answers.hpp"
#include "libtypesafe/json.hpp"
#include "libtypesafe/questions.hpp"
#include "libtypesafe/result.hpp"
#include "libtypesafe/retry.hpp"
#include "libtypesafe/transport.hpp"

namespace libtypesafe {

inline constexpr std::string_view default_base_url = "https://api.typesafe.ai";
inline constexpr std::string_view default_model = "jev-latest";
inline constexpr std::chrono::milliseconds default_timeout{10'000};

namespace env {
inline constexpr const char* api_key = "TYPESAFE_API_KEY";
inline constexpr const char* base_url = "TYPESAFE_BASE_URL";
inline constexpr const char* default_model = "TYPESAFE_DEFAULT_MODEL";
inline constexpr const char* log_level = "TYPESAFE_LOG_LEVEL";
}  // namespace env

enum class LogLevel { debug, info, warn, error, off };

/// Receives log lines at or above the client's level, on the client's thread. Credential headers
/// are redacted; request and response bodies are not (they are logged only at `debug`).
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

/// Client settings. Explicit values win over environment variables, which win over defaults.
/// Blank environment values are ignored. An aggregate, so C++20 callers can use designated
/// initializers.
struct ClientOptions {
  /// Falls back to TYPESAFE_API_KEY. Whitespace is trimmed; must be printable ASCII without spaces.
  std::optional<std::string> api_key;
  /// Falls back to TYPESAFE_BASE_URL, then `default_base_url`. Trailing slashes are removed.
  std::optional<std::string> base_url;
  /// Falls back to TYPESAFE_DEFAULT_MODEL, then `default_model`.
  std::optional<std::string> default_model;
  /// Falls back to TYPESAFE_LOG_LEVEL, then `warn`.
  std::optional<LogLevel> log_level;
  /// Default: lines prefixed `[libtypesafe]` on stderr.
  LogSink logger;
  RetryPolicy retry;
  /// Per attempt, covering the full response body.
  std::chrono::milliseconds timeout = default_timeout;
  /// Sent with every request; cannot override Authorization, Content-Type, or SDK headers.
  Headers default_headers;
  /// Default: `make_curl_transport()`.
  std::shared_ptr<Transport> transport;
};

/// Per-call overrides.
struct CallOptions {
  /// System One only; overrides the client's default model.
  std::optional<std::string> model;
  std::optional<std::chrono::milliseconds> timeout;
  /// Replaces the client's policy for this call.
  std::optional<RetryPolicy> retry;
  /// Merged over `ClientOptions::default_headers`.
  Headers headers;
  /// System One only: a JSON object shallow-merged over the request body, last write wins.
  Json extra_body = nullptr;
};

namespace detail {
class ClientImpl;
struct TestAccess;

template <class T>
struct Slot {
  std::optional<Result<T>> result;
  std::function<void(Result<T>)> on_done;
  bool completed = false;
  bool cancel_requested = false;
  bool abandoned = false;
};
}  // namespace detail

template <class T>
class Pending;

namespace detail {
template <class T>
Pending<T> make_pending(std::shared_ptr<Slot<T>> slot);
}  // namespace detail

/// A call in flight. Completes during `Client::pump()`, on the client's thread.
///
/// Move-only. Destroying an unfinished handle cancels the call and suppresses its callback, so
/// a handle owned by an object can't call back into it after it is gone. Call `detach()` to let it finish.
template <class T>
class Pending {
 public:
  Pending() = default;
  Pending(Pending&&) noexcept = default;
  Pending& operator=(Pending&& other) noexcept {
    release();
    slot_ = std::move(other.slot_);
    return *this;
  }
  Pending(const Pending&) = delete;
  Pending& operator=(const Pending&) = delete;
  ~Pending() { release(); }

  [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr; }
  /// True once the call has finished, including by cancellation.
  [[nodiscard]] bool done() const noexcept { return slot_ && slot_->completed; }

  /// Move the result out, leaving the handle empty (`valid()` and `done()` become false).
  /// Requires `done()` and no callback.
  [[nodiscard]] Result<T> take() {
    LIBTYPESAFE_ASSERT(done() && slot_->result.has_value(), "Pending::take() before done() or with a callback");
    Result<T> result = std::move(*slot_->result);
    slot_.reset();
    return result;
  }

  /// Request cancellation. The call completes as `ErrorKind::cancelled` in the next `pump()`.
  void cancel() noexcept {
    if (slot_) slot_->cancel_requested = true;
  }

  /// Let the call run to completion without this handle; a callback still runs.
  void detach() noexcept { slot_.reset(); }

 private:
  template <class U>
  friend Pending<U> detail::make_pending(std::shared_ptr<detail::Slot<U>> slot);
  explicit Pending(std::shared_ptr<detail::Slot<T>> slot) : slot_(std::move(slot)) {}

  void release() noexcept {
    if (slot_ && !slot_->completed) slot_->abandoned = true;
    slot_.reset();
  }

  std::shared_ptr<detail::Slot<T>> slot_;
};

/// Client for the TypeSafe AI API.
///
/// Thread-compatible: use each Client from one thread at a time (typically the main or UI thread).
/// Transports may complete from any thread; results and callbacks are always delivered inside
/// `pump()`. The SDK never throws; every failure is an `Error` in a `Result`.
class Client {
 public:
  /// Fails with `invalid_argument` when the API key is missing or invalid or an option is out of range.
  [[nodiscard]] static Result<Client> create(ClientOptions options = {});

  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  /// Cancels unfinished calls; their handles report `cancelled`, and callbacks do not run.
  ~Client();

  // --- Non-blocking -------------------------------------------------------------------------

  /// POST /v1/systemone: answer named questions about `state` (text, a JSON object, or an array).
  /// `on_done`, if given, runs inside `pump()` and receives the result instead of `take()`.
  [[nodiscard]] Pending<SystemOneResult> system_one_async(
      Json state, Questions questions, CallOptions options = {},
      std::function<void(Result<SystemOneResult>)> on_done = {});

  /// GET /v1/models.
  [[nodiscard]] Pending<std::vector<ModelInfo>> list_models_async(
      CallOptions options = {}, std::function<void(Result<std::vector<ModelInfo>>)> on_done = {});

  /// Advance transfers, retry timers, and cancellations; complete finished calls and run their
  /// callbacks. Never blocks. Call it from your loop. Allocates nothing when no call is in flight.
  void pump();

  [[nodiscard]] std::size_t in_flight() const noexcept;

  // --- Blocking (tools, tests, loading screens) ---------------------------------------------

  /// Submit and `pump()` until this call finishes. Other calls' callbacks may run meanwhile.
  /// Must not be called from inside a callback.
  [[nodiscard]] Result<SystemOneResult> system_one(const Json& state, const Questions& questions,
                                                   const CallOptions& options = {});
  [[nodiscard]] Result<std::vector<ModelInfo>> list_models(const CallOptions& options = {});

  [[nodiscard]] const std::string& base_url() const noexcept;
  [[nodiscard]] const std::string& default_model() const noexcept;
  [[nodiscard]] LogLevel log_level() const noexcept;

 private:
  friend struct detail::TestAccess;
  explicit Client(std::unique_ptr<detail::ClientImpl> impl);
  std::unique_ptr<detail::ClientImpl> impl_;
};

}  // namespace libtypesafe
