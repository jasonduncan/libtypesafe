#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libtypesafe/libtypesafe.hpp>

#include "fakes.hpp"

namespace ts = libtypesafe;
using fakes::Harness;
using fakes::json_response;
using namespace std::chrono_literals;

// Sanitizer runtimes supply their own operator new (TSan's static runtime on Linux clashes with a
// replacement), so allocation counting runs only in unsanitized builds.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LIBTYPESAFE_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LIBTYPESAFE_TEST_SANITIZED 1
#endif
#endif

#ifndef LIBTYPESAFE_TEST_SANITIZED
// Counts allocations while enabled, to check that an idle pump() allocates nothing.
namespace {
std::atomic<bool> counting{false};
std::atomic<int> allocations{0};
}  // namespace

void* operator new(std::size_t size) {
  if (counting) ++allocations;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

ts::Questions one_question(ts::Key<ts::NoulAnswer>* key = nullptr) {
  ts::Questions q;
  auto k = q.add("billing", ts::noul("Is this about billing?"));
  if (key != nullptr) *key = k;
  return q;
}

ts::TransportOutcome success(double p = 0.9) {
  return json_response(200, fakes::ok_body({{"billing", fakes::noul_answer(p)}}), {{"x-typesafe-request-id", "r1"}});
}

ts::TransportOutcome status(int code, ts::Headers headers = {}) {
  return json_response(code, {{"error", "nope"}}, std::move(headers));
}

}  // namespace

TEST_CASE("a request carries the SDK's headers and body") {
  ts::ClientOptions options;
  options.default_headers = {{"X-App", "demo"}, {"Authorization", "Bearer hijack"}};
  Harness h(options);
  ts::CallOptions call;
  call.headers = {{"x-app", "call"}, {"X-TypeSafe-Retry-Count", "9"}};
  call.model = "jev-1.13.0";
  auto pending = h.client.system_one_async("I was charged twice.", one_question(), call);

  REQUIRE(h.transport->attempts.size() == 1);  // sent at submit, not at the next pump
  const ts::HttpRequest& request = h.transport->attempts[0].request;
  CHECK(request.method == "POST");
  CHECK(request.url == "https://api.typesafe.ai/v1/systemone");
  CHECK(request.timeout == 10s);
  CHECK(request.headers.get("Authorization") == "Bearer ts-test-key-123456");
  CHECK(request.headers.get("X-App") == "call");
  CHECK_FALSE(request.headers.contains("X-TypeSafe-Retry-Count"));
  CHECK(request.headers.get("Content-Type") == "application/json");
  CHECK(request.headers.get("Accept") == "application/json");
  CHECK(request.headers.get("User-Agent") == "libtypesafe/0.1.0");
  CHECK(request.headers.get("X-TypeSafe-SDK") == "libtypesafe/0.1.0");
  CHECK_THAT(std::string(*request.headers.get("X-TypeSafe-Runtime")), Catch::Matchers::StartsWith("cpp/"));
  CHECK(request.body ==
        R"({"state":"I was charged twice.","model":"jev-1.13.0","questions":{"billing":{"type":"noul","instructions":"Is this about billing?"}}})");
}

TEST_CASE("results arrive only through pump()") {
  Harness h;
  ts::Key<ts::NoulAnswer> billing;
  auto pending = h.client.system_one_async("s", one_question(&billing));
  h.transport->complete(0, success(0.97));
  CHECK_FALSE(pending.done());  // completed on the transport, not yet delivered
  h.client.pump();
  REQUIRE(pending.done());
  auto result = pending.take();
  REQUIRE(result);
  CHECK((*result)[billing].noul == 0.97);
  CHECK(result->meta().attempts == 1);
  CHECK_FALSE(pending.valid());  // take() empties the handle
  CHECK(h.client.in_flight() == 0);
}

TEST_CASE("callbacks run inside pump() and may submit new calls") {
  Harness h;
  int callbacks = 0;
  ts::Pending<ts::SystemOneResult> second;
  auto first = h.client.system_one_async("s", one_question(), {}, [&](ts::Result<ts::SystemOneResult> result) {
    ++callbacks;
    CHECK(result);
    second = h.client.system_one_async("again", one_question());
    // Blocking from inside a callback is refused instead of deadlocking.
    auto blocked = h.client.system_one("x", one_question());
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().kind == ts::ErrorKind::invalid_argument);
  });
  first.detach();
  h.transport->complete(0, success());
  CHECK(callbacks == 0);
  h.client.pump();
  CHECK(callbacks == 1);
  CHECK(second.valid());
  CHECK(h.transport->attempts.size() == 2);
}

TEST_CASE("retries follow the backoff schedule and add a retry count") {
  Harness h;  // random() == 0: no jitter
  auto pending = h.client.system_one_async("s", one_question());
  h.transport->complete(0, status(503));
  h.client.pump();
  CHECK(h.transport->attempts.size() == 1);  // waiting 500ms
  h.advance(499ms);
  h.client.pump();
  CHECK(h.transport->attempts.size() == 1);
  h.advance(1ms);
  h.client.pump();
  REQUIRE(h.transport->attempts.size() == 2);
  CHECK(h.transport->attempts[1].request.headers.get("X-TypeSafe-Retry-Count") == "1");

  h.transport->complete(1, status(500));
  h.client.pump();
  h.advance(999ms);
  h.client.pump();
  CHECK(h.transport->attempts.size() == 2);  // second retry waits 1000ms
  h.advance(1ms);
  h.client.pump();
  REQUIRE(h.transport->attempts.size() == 3);
  CHECK(h.transport->attempts[2].request.headers.get("X-TypeSafe-Retry-Count") == "2");

  h.transport->complete(2, status(502));
  h.client.pump();
  REQUIRE(pending.done());  // max_retries = 2 exhausted
  auto result = pending.take();
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == ts::ErrorKind::server_error);
  CHECK(result.error().status == 502);
  CHECK(result.error().attempts == 3);
  CHECK(h.logged("#1 POST /v1/systemone retrying in 500ms (retry 1/2) after 503"));
}

TEST_CASE("Retry-After is honored up to its cap") {
  Harness h;
  auto pending = h.client.system_one_async("s", one_question());
  h.transport->complete(0, status(429, {{"retry-after-ms", "1234"}}));
  h.client.pump();
  h.advance(1233ms);
  h.client.pump();
  CHECK(h.transport->attempts.size() == 1);
  h.advance(1ms);
  h.client.pump();
  REQUIRE(h.transport->attempts.size() == 2);

  // Longer than max_retry_after (60 s): falls back to backoff, the second retry's 1000ms.
  h.transport->complete(1, status(429, {{"Retry-After", "120"}}));
  h.client.pump();
  h.advance(1000ms);
  h.client.pump();
  CHECK(h.transport->attempts.size() == 3);
}

TEST_CASE("non-retryable statuses fail at once; a predicate can widen the rules") {
  Harness h;
  auto plain = h.client.system_one_async("s", one_question());
  h.transport->complete(0, status(400));
  h.client.pump();
  REQUIRE(plain.done());
  CHECK(plain.take().error().kind == ts::ErrorKind::bad_request);

  ts::CallOptions options;
  options.retry = ts::RetryPolicy{};
  options.retry->predicate = [](const ts::Error& error) { return error.status == 400; };
  auto widened = h.client.system_one_async("s", one_question(), options);
  h.transport->complete(1, status(400));
  h.client.pump();
  h.advance(500ms);
  h.client.pump();
  CHECK(h.transport->attempts.size() == 3);
}

TEST_CASE("the total budget stops a retry that would overrun it") {
  ts::ClientOptions options;
  options.retry.total_budget = 2s;
  Harness h(options);
  auto pending = h.client.system_one_async("s", one_question());
  h.advance(1600ms);  // a slow first attempt
  h.transport->complete(0, status(503));
  h.client.pump();  // 1600 + 500 >= 2000: give up
  REQUIRE(pending.done());
  CHECK(pending.take().error().status == 503);
  CHECK(h.logged("total retry budget reached"));
}

TEST_CASE("retry_after_send controls retries that might double-bill") {
  const auto attempts_after_timeout = [](bool retry_after_send, bool request_sent) {
    ts::ClientOptions options;
    options.retry.retry_after_send = retry_after_send;
    Harness h(options);
    auto pending = h.client.system_one_async("s", one_question());
    h.transport->complete(0, ts::TransportError{ts::TransportError::Kind::timeout, "slow", request_sent});
    h.client.pump();
    h.advance(500ms);
    h.client.pump();
    if (pending.done()) CHECK(pending.take().error().kind == ts::ErrorKind::timeout);
    return h.transport->attempts.size();
  };
  CHECK(attempts_after_timeout(true, true) == 2);    // default: retry like the official SDKs
  CHECK(attempts_after_timeout(false, true) == 1);   // might have been processed: stop
  CHECK(attempts_after_timeout(false, false) == 2);  // never reached the server: safe
}

TEST_CASE("connection errors retry by default and report request_sent") {
  Harness h;
  auto pending = h.client.system_one_async("s", one_question());
  h.transport->complete(0, ts::TransportError{ts::TransportError::Kind::connection, "refused", false});
  h.client.pump();
  h.advance(500ms);
  h.client.pump();
  h.transport->complete(1, ts::TransportError{ts::TransportError::Kind::connection, "reset", true});
  h.client.pump();
  h.advance(1s);
  h.client.pump();
  h.transport->complete(2, ts::TransportError{ts::TransportError::Kind::connection, "reset", true});
  h.client.pump();
  REQUIRE(pending.done());
  auto error = pending.take().error();
  CHECK(error.kind == ts::ErrorKind::connection);
  CHECK(error.message == "POST https://api.typesafe.ai/v1/systemone: Connection error: reset");
  CHECK(error.attempts == 3);
}

TEST_CASE("cancel() stops the transfer and reports cancelled") {
  Harness h;
  bool called = false;
  auto pending = h.client.system_one_async("s", one_question(), {}, [&](ts::Result<ts::SystemOneResult> result) {
    called = true;
    CHECK(result.error().kind == ts::ErrorKind::cancelled);
  });
  pending.cancel();
  h.client.pump();
  CHECK(h.transport->attempts[0].cancelled);
  CHECK(called);
  CHECK(pending.done());
}

TEST_CASE("cancel() during a retry wait ends the wait") {
  Harness h;
  auto pending = h.client.system_one_async("s", one_question());
  h.transport->complete(0, status(503));
  h.client.pump();
  pending.cancel();
  h.client.pump();
  REQUIRE(pending.done());
  CHECK(pending.take().error().kind == ts::ErrorKind::cancelled);
  CHECK(h.transport->attempts.size() == 1);
}

TEST_CASE("dropping a handle cancels the call and suppresses its callback") {
  Harness h;
  bool called = false;
  {
    auto pending = h.client.system_one_async("s", one_question(), {}, [&](auto) { called = true; });
  }
  h.client.pump();
  CHECK(h.transport->attempts[0].cancelled);
  CHECK_FALSE(called);
  CHECK(h.client.in_flight() == 0);
}

TEST_CASE("destroying the client completes handles as cancelled") {
  ts::Pending<ts::SystemOneResult> pending;
  std::shared_ptr<fakes::FakeTransport> transport;
  {
    Harness h;
    transport = h.transport;
    pending = h.client.system_one_async("s", one_question());
  }
  REQUIRE(pending.done());
  CHECK(pending.take().error().kind == ts::ErrorKind::cancelled);
  CHECK(transport->attempts[0].cancelled);
  // A late completion from the transport after the client is gone is harmless.
  transport->attempts[0].done(success());
}

TEST_CASE("invalid calls send nothing and are delivered by the next pump") {
  Harness h;
  ts::Questions bad;
  bad.add("x", ts::noul());
  bad.add("x", ts::noul());
  auto duplicate = h.client.system_one_async("s", bad);
  auto empty = h.client.system_one_async("s", ts::Questions());
  ts::CallOptions zero;
  zero.timeout = 0ms;
  auto bad_timeout = h.client.system_one_async("s", one_question(), zero);
  CHECK(h.transport->attempts.empty());
  CHECK_FALSE(duplicate.done());
  h.client.pump();
  CHECK(duplicate.take().error().message == "Duplicate question name \"x\".");
  CHECK(empty.take().error().message == "At least one question is required.");
  CHECK(bad_timeout.take().error().message == "timeout must be positive.");
}

TEST_CASE("a response that doesn't answer the questions fails validation") {
  Harness h;
  auto pending = h.client.system_one_async("s", one_question());
  h.transport->complete(0, json_response(200, fakes::ok_body({{"other", fakes::noul_answer(1)}})));
  h.client.pump();
  auto result = pending.take();
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == ts::ErrorKind::response_validation);
  CHECK(result.error().field_path == "answers.billing");
  CHECK(h.transport->attempts.size() == 1);  // not retried
}

TEST_CASE("completions from another thread are delivered on the pumping thread") {
  Harness h;
  auto pending = h.client.system_one_async("s", one_question());
  std::thread worker([&] { h.transport->complete(0, success()); });
  worker.join();
  h.client.pump();
  REQUIRE(pending.done());
  CHECK(pending.take());
}

TEST_CASE("the client stops waiting when a transport ignores its timeout") {
  ts::ClientOptions options;
  options.retry.max_retries = 0;
  Harness h(options);
  auto pending = h.client.system_one_async("s", one_question());
  h.advance(10s + ts::detail::ClientImpl::backstop_grace);
  h.client.pump();
  REQUIRE(pending.done());
  CHECK(pending.take().error().kind == ts::ErrorKind::timeout);
  CHECK(h.transport->attempts[0].cancelled);
}

TEST_CASE("blocking calls pump until done") {
  Harness h;
  h.transport->script.push_back(success(0.6));
  auto result = h.client.system_one("s", one_question());
  REQUIRE(result);
  CHECK(result->model() == "jev-1.13.0");

  h.transport->script.push_back(json_response(
      200, {{"models", {{{"name", "jev-latest"}, {"description", "d"}, {"release_date", "2026-09-15"}}}}}));
  auto models = h.client.list_models();
  REQUIRE(models);
  CHECK(models->at(0).name == "jev-latest");
  const auto& request = h.transport->attempts.back().request;
  CHECK(request.method == "GET");
  CHECK(request.url == "https://api.typesafe.ai/v1/models");
  CHECK_FALSE(request.headers.contains("Content-Type"));
}

TEST_CASE("logs redact credentials; debug shows bodies") {
  Harness h;
  auto pending = h.client.system_one_async("s", one_question());
  h.transport->complete(0, success());
  h.client.pump();
  CHECK(h.logged("#1 POST /v1/systemone <- 200 in 0ms (request r1)"));
  CHECK(h.logged("Authorization: ***"));
  CHECK(h.logged("<- body {"));
  for (const auto& [level, line] : h.logs) CHECK(line.find("ts-test-key") == std::string::npos);
}

TEST_CASE("an idle pump() allocates nothing") {
#ifdef LIBTYPESAFE_TEST_SANITIZED
  SKIP("allocation counting is off under sanitizers");
#else
  Harness h;
  h.client.pump();
  allocations = 0;
  counting = true;
  for (int i = 0; i < 100; ++i) h.client.pump();
  counting = false;
  CHECK(allocations == 0);
#endif
}
