#include "libtypesafe/client.hpp"

#include <algorithm>
#include <cmath>

#include "client_impl.hpp"
#include "libtypesafe/libtypesafe.hpp"
#include "wire.hpp"

namespace libtypesafe {

namespace detail {

Error invalid(std::string message) {
  Error error;
  error.kind = ErrorKind::invalid_argument;
  error.message = std::move(message);
  return error;
}

namespace {

constexpr std::string_view retry_count_header = "X-TypeSafe-Retry-Count";

std::string sdk_identity() { return "libtypesafe/" + std::string(libtypesafe::version); }

}  // namespace

std::string describe_runtime() {
  std::string compiler = "unknown";
#if defined(__clang__)
  compiler = "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
             std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  compiler = "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
             std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
  compiler = "msvc " + std::to_string(_MSC_VER);
#endif
#if defined(__APPLE__)
  const char* os = "darwin";
#elif defined(__linux__)
  const char* os = "linux";
#elif defined(_WIN32)
  const char* os = "win32";
#else
  const char* os = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  const char* arch = "x86_64";
#else
  const char* arch = "unknown";
#endif
#if defined(_MSVC_LANG)
  const long standard = _MSVC_LANG;  // MSVC reports __cplusplus as 199711L without /Zc:__cplusplus
#else
  const long standard = __cplusplus;
#endif
  return "cpp/" + std::to_string(standard) + " (" + compiler + "; " + os + "; " + arch + ")";
}

bool retryable(const Error& error, const RetryPolicy& policy) {
  bool builtin = false;
  switch (error.kind) {
    case ErrorKind::cancelled:
    case ErrorKind::invalid_argument:
      return false;
    case ErrorKind::timeout:
      builtin = policy.retry_timeouts && (!error.request_sent || policy.retry_after_send);
      break;
    case ErrorKind::connection:
      builtin = policy.retry_connection_errors && (!error.request_sent || policy.retry_after_send);
      break;
    default:
      builtin = error.has_response() && policy.http_statuses.count(error.status) != 0;
      break;
  }
  return builtin || (policy.predicate && policy.predicate(error));
}

milliseconds retry_delay(int retry_index, const Error& error, const RetryPolicy& policy, double random) {
  if (policy.respect_retry_after && error.retry_after && *error.retry_after <= policy.max_retry_after) {
    return *error.retry_after;
  }
  if (policy.backoff_initial.count() == 0 || policy.backoff_max.count() == 0) return milliseconds(0);
  const double initial = static_cast<double>(policy.backoff_initial.count());
  const double maximum = static_cast<double>(policy.backoff_max.count());
  const double exponential = retry_index >= 62 ? maximum : std::min(initial * std::ldexp(1.0, retry_index), maximum);
  return milliseconds(static_cast<std::int64_t>(std::llround(exponential * (1 - random * policy.backoff_jitter))));
}

// ---------------------------------------------------------------------------
// ClientImpl
// ---------------------------------------------------------------------------

ClientImpl::ClientImpl(Config cfg)
    : config(std::move(cfg)),
      logger(config.log_level, config.logger),
      runtime(describe_runtime()),
      rng_(static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()) ^
           reinterpret_cast<std::uintptr_t>(this)) {
  random = [this] { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); };
}

ClientImpl::~ClientImpl() {
  // Complete everything as cancelled without running callbacks; handles still see the result.
  Deferred discarded;
  for (auto& call : calls) {
    if (call->phase == CallBase::Phase::finished) continue;
    if (call->phase == CallBase::Phase::in_flight) config.transport->cancel(call->transfer);
    Error error = invalid("Client destroyed before the call finished.");
    error.kind = ErrorKind::cancelled;
    error.attempts = call->attempts;
    call->fail(std::move(error), false, discarded);
  }
}

std::optional<std::string> ClientImpl::prepare(CallBase& call, std::string method, std::string path,
                                               const CallOptions& options) {
  call.number = ++next_call_;
  call.method = std::move(method);
  call.path = std::move(path);
  call.url = config.base_url + call.path;
  call.endpoint = call.method + " " + without_userinfo(call.url);
  call.timeout = options.timeout.value_or(config.timeout);
  call.retry = options.retry.value_or(config.retry);
  call.started = now();
  call.warn = [this](std::string_view message) { logger.write(LogLevel::warn, message); };

  // User headers first; the SDK's own headers win, and the retry count is ours alone.
  call.headers = config.default_headers;
  call.headers.merge(options.headers);
  call.headers.erase(retry_count_header);
  call.headers.set("Authorization", "Bearer " + config.api_key);
  call.headers.set("Accept", "application/json");
  call.headers.set("User-Agent", sdk_identity());
  call.headers.set("X-TypeSafe-SDK", sdk_identity());
  call.headers.set("X-TypeSafe-Runtime", runtime);
  if (call.method == "POST") {
    call.headers.set("Content-Type", "application/json");
  } else {
    call.headers.erase("Content-Type");
  }

  if (call.timeout.count() <= 0) return "timeout must be positive.";
  if (auto problem = validate_retry(call.retry)) return problem;
  return std::nullopt;
}

void ClientImpl::submit(std::unique_ptr<CallBase> call) {
  if (call->rejection) {
    call->phase = CallBase::Phase::rejected;
  } else {
    send(*call);
  }
  calls.push_back(std::move(call));
}

void ClientImpl::send(CallBase& call) {
  call.attempts += 1;
  call.attempt_key = ++next_attempt_;
  call.phase = CallBase::Phase::in_flight;
  call.attempt_started = now();

  HttpRequest request;
  request.method = call.method;
  request.url = call.url;
  request.headers = call.headers;
  if (call.attempts > 1) request.headers.set(std::string(retry_count_header), std::to_string(call.attempts - 1));
  request.body = call.body;
  request.timeout = call.timeout;

  if (logger.enabled(LogLevel::debug)) {
    logger.write(LogLevel::debug, call.tag() + " -> " + without_userinfo(call.url) +
                                      " headers=" + describe_headers(request.headers) + " body=" + call.body);
  }
  const std::uint64_t key = call.attempt_key;
  call.transfer = config.transport->start(
      std::move(request), [inbox = inbox, key](TransportOutcome outcome) { inbox->push(key, std::move(outcome)); });
}

Error ClientImpl::transport_error(const CallBase& call, const TransportError& failure) const {
  Error error;
  error.request_sent = failure.request_sent;
  error.attempts = call.attempts;
  switch (failure.kind) {
    case TransportError::Kind::timeout:
      error.kind = ErrorKind::timeout;
      error.message = call.endpoint + ": Request timed out after " + format_ms(call.timeout) + ".";
      break;
    case TransportError::Kind::cancelled:
      error.kind = ErrorKind::cancelled;
      error.message = call.endpoint + ": Request was cancelled.";
      break;
    case TransportError::Kind::connection:
      error.kind = ErrorKind::connection;
      error.message = call.endpoint + ": Connection error" +
                      (failure.message.empty() ? std::string(".") : ": " + failure.message);
      break;
  }
  return error;
}

void ClientImpl::handle(CallBase& call, TransportOutcome outcome, Deferred& deferred) {
  const std::string elapsed = format_ms(to_ms(now() - call.attempt_started));
  if (auto* response = std::get_if<HttpResponse>(&outcome)) {
    if (logger.enabled(LogLevel::info)) {
      const auto request_id = response->headers.get("x-typesafe-request-id");
      logger.write(LogLevel::info, call.tag() + " <- " + std::to_string(response->status) + " in " + elapsed +
                                       (request_id ? " (request " + std::string(*request_id) + ")" : ""));
    }
    if (logger.enabled(LogLevel::debug)) logger.write(LogLevel::debug, call.tag() + " <- body " + response->body);

    DecodeContext context{call.endpoint, call.attempts, {}};
    if (response->status >= 200 && response->status < 300) {
      auto error = call.try_succeed(*response, deferred);
      if (!error) return;
      logger.write(LogLevel::warn, call.tag() + " " + error->message);
      on_failure(call, std::move(*error), deferred);
      return;
    }
    on_failure(call, Wire::api_error(*response, context), deferred);
    return;
  }

  const auto& failure = std::get<TransportError>(outcome);
  Error error = transport_error(call, failure);
  if (failure.kind == TransportError::Kind::timeout) {
    logger.write(LogLevel::info, call.tag() + " timed out after " + elapsed);
  } else if (failure.kind == TransportError::Kind::connection) {
    logger.write(LogLevel::info, call.tag() + " connection error after " + elapsed + ": " + failure.message);
  } else {
    logger.write(LogLevel::info, call.tag() + " cancelled by the transport");
  }
  on_failure(call, std::move(error), deferred);
}

void ClientImpl::on_failure(CallBase& call, Error error, Deferred& deferred) {
  error.attempts = call.attempts;
  const int retries_done = call.attempts - 1;
  if (retries_done < call.retry.max_retries && retryable(error, call.retry)) {
    const milliseconds delay = retry_delay(retries_done, error, call.retry, random());
    const Clock::time_point t = now();
    if (!call.retry.total_budget || to_ms(t - call.started) + delay < *call.retry.total_budget) {
      call.phase = CallBase::Phase::waiting;
      call.resend_at = t + delay;
      const std::string reason = error.has_response() ? std::to_string(error.status) : std::string(to_string(error.kind));
      logger.write(LogLevel::info, call.tag() + " retrying in " + format_ms(delay) + " (retry " +
                                       std::to_string(retries_done + 1) + "/" +
                                       std::to_string(call.retry.max_retries) + ") after " + reason);
      return;
    }
    logger.write(LogLevel::info, call.tag() + " not retrying: total retry budget reached");
  }
  call.fail(std::move(error), true, deferred);
}

void ClientImpl::pump() {
  if (in_pump) return;
  in_pump = true;
  config.transport->poll();

  Deferred& deferred = deferred_;
  inbox->drain(drained_);
  for (auto& [key, outcome] : drained_) {
    for (auto& call : calls) {
      if (call->phase == CallBase::Phase::in_flight && call->attempt_key == key) {
        handle(*call, std::move(outcome), deferred);
        break;
      }
    }
    // No match: a completion for an attempt that was cancelled or timed out. Ignore it.
  }

  const Clock::time_point t = now();
  for (auto& call : calls) {
    switch (call->phase) {
      case CallBase::Phase::finished:
        continue;
      case CallBase::Phase::rejected:
        call->fail(std::move(*call->rejection), true, deferred);
        continue;
      default:
        break;
    }
    if (call->abandoned() || call->cancel_requested()) {
      if (call->phase == CallBase::Phase::in_flight) config.transport->cancel(call->transfer);
      logger.write(LogLevel::info, call->tag() + " cancelled");
      Error error = invalid(call->endpoint + ": Request was cancelled.");
      error.kind = ErrorKind::cancelled;
      error.attempts = call->attempts;
      call->fail(std::move(error), !call->abandoned(), deferred);
    } else if (call->phase == CallBase::Phase::waiting && t >= call->resend_at) {
      send(*call);
    } else if (call->phase == CallBase::Phase::in_flight &&
               t >= call->attempt_started + call->timeout + backstop_grace) {
      // The transport did not enforce the timeout; stop waiting for it.
      config.transport->cancel(call->transfer);
      logger.write(LogLevel::info, call->tag() + " timed out after " + format_ms(to_ms(t - call->attempt_started)));
      on_failure(*call, transport_error(*call, TransportError{TransportError::Kind::timeout, {}, true}), deferred);
    }
  }
  calls.erase(std::remove_if(calls.begin(), calls.end(),
                             [](const auto& call) { return call->phase == CallBase::Phase::finished; }),
              calls.end());

  drained_.clear();
  for (auto& callback : deferred) callback();
  deferred.clear();
  in_pump = false;
}

std::size_t ClientImpl::in_flight() const {
  return static_cast<std::size_t>(std::count_if(calls.begin(), calls.end(), [](const auto& call) {
    return call->phase != CallBase::Phase::finished;
  }));
}

milliseconds ClientImpl::time_to_next_deadline() const {
  constexpr milliseconds longest{100};
  const Clock::time_point t = now();
  milliseconds wait = longest;
  for (const auto& call : calls) {
    Clock::time_point due;
    if (call->phase == CallBase::Phase::waiting) {
      due = call->resend_at;
    } else if (call->phase == CallBase::Phase::in_flight) {
      due = call->attempt_started + call->timeout + backstop_grace;
    } else {
      return milliseconds(0);  // rejected: deliver on the next pump
    }
    wait = std::min(wait, std::max(milliseconds(0), to_ms(due - t)));
  }
  return wait;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Result<Client> Client::create(ClientOptions options) {
  auto config = detail::resolve_config(std::move(options), detail::process_env());
  if (!config) return config.error();
  if (!config->transport) {
    config->transport = make_curl_transport();
    if (!config->transport) {
      Error error;
      error.message = "No transport: libtypesafe was built without libcurl; set ClientOptions::transport.";
      return error;
    }
  }
  return Client(std::make_unique<detail::ClientImpl>(std::move(*config)));
}

Client::Client(std::unique_ptr<detail::ClientImpl> impl) : impl_(std::move(impl)) {}
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client() = default;

namespace {

template <class T>
Pending<T> submit_call(detail::ClientImpl& impl, std::shared_ptr<detail::Slot<T>> slot,
                       std::unique_ptr<detail::Call<T>> call, std::optional<Error> rejection) {
  if (rejection) call->rejection = std::move(rejection);
  impl.submit(std::move(call));
  return detail::make_pending(std::move(slot));
}

template <class T>
Result<T> wait_for(detail::ClientImpl& impl, Pending<T> pending) {
  while (!pending.done()) {
    impl.pump();
    if (pending.done()) break;
    impl.config.transport->wait(impl.time_to_next_deadline());
  }
  return pending.take();
}

Error blocking_in_callback() {
  return detail::invalid("Blocking calls cannot be made from inside a callback; use the async form.");
}

}  // namespace

Pending<SystemOneResult> Client::system_one_async(Json state, Questions questions, CallOptions options,
                                                  std::function<void(Result<SystemOneResult>)> on_done) {
  detail::ClientImpl& impl = *impl_;
  auto slot = std::make_shared<detail::Slot<SystemOneResult>>();
  slot->on_done = std::move(on_done);
  auto shared = std::make_shared<const Questions>(std::move(questions));
  auto call = std::make_unique<detail::Call<SystemOneResult>>(
      slot, [shared](const HttpResponse& response, const detail::DecodeContext& context) {
        return detail::Wire::decode_system_one(response, *shared, context);
      });

  if (auto problem = impl.prepare(*call, "POST", "/v1/systemone", options)) {
    return submit_call(impl, slot, std::move(call), detail::invalid(std::move(*problem)));
  }
  if (shared->error()) return submit_call(impl, slot, std::move(call), shared->error());
  if (shared->empty()) {
    return submit_call(impl, slot, std::move(call), detail::invalid("At least one question is required."));
  }
  auto body = detail::Wire::encode_system_one(state, *shared, options.model.value_or(impl.config.default_model),
                                              options.extra_body);
  if (!body) return submit_call(impl, slot, std::move(call), body.error());
  call->body = std::move(*body);
  return submit_call(impl, slot, std::move(call), std::nullopt);
}

Pending<std::vector<ModelInfo>> Client::list_models_async(
    CallOptions options, std::function<void(Result<std::vector<ModelInfo>>)> on_done) {
  detail::ClientImpl& impl = *impl_;
  auto slot = std::make_shared<detail::Slot<std::vector<ModelInfo>>>();
  slot->on_done = std::move(on_done);
  auto call = std::make_unique<detail::Call<std::vector<ModelInfo>>>(
      slot, [](const HttpResponse& response, const detail::DecodeContext& context) {
        return detail::Wire::decode_models(response, context);
      });
  std::optional<Error> rejection;
  if (auto problem = impl.prepare(*call, "GET", "/v1/models", options)) rejection = detail::invalid(std::move(*problem));
  return submit_call(impl, slot, std::move(call), std::move(rejection));
}

void Client::pump() { impl_->pump(); }

std::size_t Client::in_flight() const noexcept { return impl_->in_flight(); }

Result<SystemOneResult> Client::system_one(const Json& state, const Questions& questions,
                                           const CallOptions& options) {
  if (impl_->in_pump) return blocking_in_callback();
  return wait_for(*impl_, system_one_async(state, questions, options));
}

Result<std::vector<ModelInfo>> Client::list_models(const CallOptions& options) {
  if (impl_->in_pump) return blocking_in_callback();
  return wait_for(*impl_, list_models_async(options));
}

const std::string& Client::base_url() const noexcept { return impl_->config.base_url; }
const std::string& Client::default_model() const noexcept { return impl_->config.default_model; }
LogLevel Client::log_level() const noexcept { return impl_->config.log_level; }

}  // namespace libtypesafe
