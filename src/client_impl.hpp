#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "libtypesafe/client.hpp"

#include "config.hpp"
#include "logging.hpp"
#include "util.hpp"
#include "wire.hpp"

namespace libtypesafe::detail {

/// Completions from the transport, possibly from other threads. Shared with every `done`
/// callback so a late completion after the client is gone lands here harmlessly.
struct Inbox {
  std::mutex mutex;
  std::vector<std::pair<std::uint64_t, TransportOutcome>> items;

  void push(std::uint64_t attempt, TransportOutcome outcome) {
    std::lock_guard<std::mutex> lock(mutex);
    items.emplace_back(attempt, std::move(outcome));
  }
  std::vector<std::pair<std::uint64_t, TransportOutcome>> drain() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<std::uint64_t, TransportOutcome>> out;
    out.swap(items);
    return out;
  }
};

using Deferred = std::vector<std::function<void()>>;

/// One API call and its attempts. `Call<T>` adds the result type.
class CallBase {
 public:
  enum class Phase { rejected, in_flight, waiting, finished };

  virtual ~CallBase() = default;

  std::uint64_t number = 0;
  std::string method;
  std::string path;
  std::string url;
  std::string endpoint;  // for messages: method + URL without credentials
  std::string body;
  Headers headers;  // final headers, without the retry count
  milliseconds timeout{0};
  RetryPolicy retry;
  Clock::time_point started;

  Phase phase = Phase::in_flight;
  int attempts = 0;
  TransferId transfer = 0;
  std::uint64_t attempt_key = 0;
  Clock::time_point attempt_started;
  Clock::time_point resend_at;
  std::optional<Error> rejection;  // invalid_argument found at submit; delivered by pump()
  std::function<void(std::string_view)> warn;

  [[nodiscard]] DecodeContext context() const { return {endpoint, attempts, warn}; }

  [[nodiscard]] std::string tag() const { return "#" + std::to_string(number) + " " + method + " " + path; }

  [[nodiscard]] virtual bool cancel_requested() const = 0;
  [[nodiscard]] virtual bool abandoned() const = 0;
  /// Decode a 2xx response. Completes the call and returns nullopt, or returns the decode error.
  virtual std::optional<Error> try_succeed(const HttpResponse& response, Deferred& deferred) = 0;
  /// Complete the call with `error`.
  virtual void fail(Error error, bool run_callback, Deferred& deferred) = 0;
};

template <class T>
class Call final : public CallBase {
 public:
  using Decode = std::function<Result<T>(const HttpResponse&, const DecodeContext&)>;

  Call(std::shared_ptr<Slot<T>> slot, Decode decode) : slot_(std::move(slot)), decode_(std::move(decode)) {}

  bool cancel_requested() const override { return slot_->cancel_requested; }
  bool abandoned() const override { return slot_->abandoned; }

  std::optional<Error> try_succeed(const HttpResponse& response, Deferred& deferred) override {
    Result<T> result = decode_(response, context());
    if (!result) return result.error();
    finish(std::move(result), true, deferred);
    return std::nullopt;
  }

  void fail(Error error, bool run_callback, Deferred& deferred) override {
    finish(Result<T>(std::move(error)), run_callback, deferred);
  }

 private:
  void finish(Result<T> result, bool run_callback, Deferred& deferred) {
    phase = Phase::finished;
    slot_->completed = true;
    if (slot_->on_done) {
      auto callback = std::move(slot_->on_done);
      slot_->on_done = nullptr;
      if (run_callback) {
        auto shared = std::make_shared<Result<T>>(std::move(result));
        deferred.push_back([callback = std::move(callback), shared]() { callback(std::move(*shared)); });
      }
    } else {
      slot_->result.emplace(std::move(result));
    }
  }

  std::shared_ptr<Slot<T>> slot_;
  Decode decode_;
};

class ClientImpl {
 public:
  explicit ClientImpl(Config config);
  ~ClientImpl();

  /// Start the call's first attempt, or queue its rejection for the next pump().
  void submit(std::unique_ptr<CallBase> call);
  void pump();
  [[nodiscard]] std::size_t in_flight() const;
  /// How long a blocking call may wait before the next timer is due.
  [[nodiscard]] milliseconds time_to_next_deadline() const;

  /// Common call setup; returns an error message for invalid per-call options.
  std::optional<std::string> prepare(CallBase& call, std::string method, std::string path,
                                     const CallOptions& options);

  Config config;
  Logger logger;
  std::string runtime;
  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  std::vector<std::unique_ptr<CallBase>> calls;
  bool in_pump = false;

  // Seams for deterministic tests.
  std::function<Clock::time_point()> now = [] { return Clock::now(); };
  std::function<double()> random;  // uniform in [0, 1)

  /// Grace past an attempt's timeout before the client stops waiting for the transport.
  static constexpr milliseconds backstop_grace{500};

 private:
  void send(CallBase& call);
  void handle(CallBase& call, TransportOutcome outcome, Deferred& deferred);
  void on_failure(CallBase& call, Error error, Deferred& deferred);
  Error transport_error(const CallBase& call, const TransportError& failure) const;

  std::uint64_t next_call_ = 0;
  std::uint64_t next_attempt_ = 0;
  std::mt19937_64 rng_;
};

Error invalid(std::string message);

template <class T>
Pending<T> make_pending(std::shared_ptr<Slot<T>> slot) {
  return Pending<T>(std::move(slot));
}

/// Whether `policy` retries `error` (not counting attempts or budget).
bool retryable(const Error& error, const RetryPolicy& policy);

/// Delay before retry number `retry_index` (0 for the first retry). `random` is in [0, 1).
milliseconds retry_delay(int retry_index, const Error& error, const RetryPolicy& policy, double random);

/// `cpp/201703 (clang 21.0.0; darwin; arm64)`.
std::string describe_runtime();

/// Test-only access to a client's internals.
struct TestAccess {
  static ClientImpl& impl(Client& client) { return *client.impl_; }
};

}  // namespace libtypesafe::detail
