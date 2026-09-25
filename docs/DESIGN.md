# libtypesafe: design

Status: **0.1.0, 2026-09-25 (M1–M4 done).** CI is green on macOS, Linux (GCC 14, Clang 18),
and Windows (MSVC), under ASan/UBSan/TSan, and for the vcpkg and Conan packages. The live suite
passes, and requests are byte-identical to the official Python SDK's (§6.8, §9).

## 1. Goal

An unofficial C++ client for the [TypeSafe AI](https://typesafe.ai) API. It should behave like the
official SDKs, add compile-time checking of answer types, and embed in any C++ application,
including ones that disable exceptions or RTTI, or can't block the thread that calls it.

- Python: [typesafe-sdk-python](https://github.com/typesafe-ai/typesafe-sdk-python) v0.7.1, commit `0ffd094` (2026-09-21)
- JS/TS: [typesafe-sdk-js](https://github.com/typesafe-ai/typesafe-sdk-js) v0.6.0, commit `66880cc` (2026-09-15)
- API schema: OpenAPI 0.2.0, snapshot in [`spec/openapi.json`](../spec/openapi.json), fetched 2026-09-25

### Design constraints

| Constraint | Why | Consequence |
|---|---|---|
| **C++17** floor | Reaches the most toolchains; C++20 is not available everywhere. | No concepts, no `std::stop_token`. The options structs are aggregates, so C++20 callers still get designated initializers. |
| **No exceptions** | Many codebases build with exceptions disabled. | Every failure is an `Error` value inside a `Result<T>`. The library is built with `-fno-exceptions`. |
| **No RTTI** | Some codebases build with RTTI disabled. | No `dynamic_cast` or `typeid`. The error kind is an enum, not a class hierarchy. |
| **Never block the caller** | A thread running an event or UI loop can't stall for a 300 ms model call. | Calls return a `Pending<T>`. `client.pump()` runs from the caller's loop, does no blocking work, and delivers results on the calling thread. |
| **Pluggable networking** | Tests need a fake, and some hosts need their own HTTP stack. | `Transport` is a small asynchronous interface. libcurl is only the default. |
| **No surprise threads or allocations** | The host application should control both. | The default transport is `curl_multi` driven from `pump()`, so no SDK threads are created. An idle `pump()` allocates nothing. |

**Out of scope:** a C ABI, other language bindings, streaming (the API has none), and custom
allocators. libtypesafe is a plain C++ library with a couple of examples.

## 2. What is being ported

| Endpoint | Request | Response |
|---|---|---|
| `POST /v1/systemone` | `state` (text, object, or array), `model`, `questions` (name → question) | `model`, `answers` (name → answer), `usage` |
| `GET /v1/models` | none | `models: [{name, description, release_date}]` |

Authentication is `Authorization: Bearer <key>`. Validation failures return 422 with FastAPI's
`{"detail": [{loc, msg, type}]}` body.

| Question | Criteria on the wire | Answer fields |
|---|---|---|
| `noul` (yes/no) | optional `{"true": desc, "false": desc}` | `noul`: P(yes), 0 to 1 |
| `choice` | `{label: desc or null, ...}` | `choice`, `confidence`, `probabilities{label: p}` |
| `score` | `[desc0, desc1, ...]`, at least 1 | `score` (expected value), `confidence`, `legend{"0": desc}`, `probabilities{"0": p}` |

In both official SDKs, the endpoints are a small part of the code. Most of it (1,300 to 1,700
lines of source each, plus about 130 test cases each) is the reliability layer: configuration from
environment variables, retries with `Retry-After` handling, timeouts, mapping statuses to errors,
and logging with credential redaction. The C++ port is mostly that layer too, restructured as a
non-blocking state machine.

## 3. API tour

**Non-blocking,** from any loop:

```cpp
#include <libtypesafe/libtypesafe.hpp>
namespace ts = libtypesafe;

auto client = ts::Client::create();          // Result<Client>; key from TYPESAFE_API_KEY
if (!client) log(client.error().message);

ts::Questions q;
auto urgent = q.add("urgent", ts::noul("The customer needs a reply today."));
auto topic  = q.add("topic",  ts::choice("What is this about?", "billing", "technical", "other"));
ts::Pending<ts::SystemOneResult> pending = client->system_one_async(message, std::move(q));

// in the caller's loop
client->pump();                               // non-blocking; completes finished calls
if (pending.done()) {
  auto result = pending.take();               // Result<SystemOneResult>; empties the handle
  if (result && (*result)[urgent].yes(0.8)) escalate();
  if (result) route_to((*result)[topic].choice);
}
```

A callback can be passed instead of polling. It runs inside `pump()`, on the calling thread.
Dropping a `Pending` cancels its call and suppresses the callback, so an object that owns the
handle and is destroyed mid-request doesn't get called back and doesn't keep spending tokens.

**Blocking,** for tools, scripts, and tests:

```cpp
ts::ClientOptions options;
options.timeout = std::chrono::seconds(15);    // C++20: Client::create({.timeout = 15s})
auto client = ts::Client::create(options);

auto tone = q.add("sentiment", ts::choice<Tone>("What is the customer's tone?"));  // enum labels
auto urgency = q.add("urgency", ts::score("How urgent?", {"can wait", "this week", "today"}));
auto result = client->system_one(ticket_json, q);
if (!result) { const ts::Error& e = result.error(); /* e.kind, e.status, e.message, ... */ }
(*result)[tone].choice == Tone::angry;       // Tone
(*result)[urgency].legend[(*result)[urgency].level()];
(*result)[urgency].choice;                   // compile error: ScoreAnswer has no member 'choice'
```

Full examples:

- [`examples/async.cpp`](../examples/async.cpp): several non-blocking calls driven by `pump()`
- [`examples/game_loop.cpp`](../examples/game_loop.cpp): the same pattern with one call per object
- [`examples/quickstart.cpp`](../examples/quickstart.cpp): the upstream README quickstart
- [`examples/triage.cpp`](../examples/triage.cpp): a port of the JS `demo.ts`

## 4. Type safety

The TS SDK infers each answer's type from the literal type of its question. The Python SDK groups
answers by type at runtime (`response.choices["tone"]`) and optionally decodes into a pydantic
model. libtypesafe uses **typed keys** instead:

- `Questions::add` returns a `Key<NoulAnswer>`, `Key<ChoiceAnswer>`, `Key<ScoreAnswer>`, or
  `Key<EnumChoiceAnswer<E>>`. Indexing a result with a key returns that type, so accessing a field
  of the wrong answer type is a compile error. The API checks and a set of negative compile
  probes confirm this.
- **Enum choices:** specialize `choice_labels<E>` and the answer's `choice` is an `E`. Without the
  specialization, a `static_assert` says `specialize libtypesafe::choice_labels<E> for this enum`.
- **Decode-time validation:** a response is accepted only if every question asked has an answer of
  the matching type, each choice is one of the question's labels, and score keys are exactly
  `"0".."n-1"`. So `result[key]` cannot fail for a key from the same `Questions`; a key from a
  different set trips `LIBTYPESAFE_ASSERT`. (The JS SDK casts without checking. Python checks the
  response's shape but not that it answers the questions that were asked.)
- **Score maps become vectors:** the wire's string-keyed `legend` and `probabilities` objects
  become `std::vector`s indexed by level.
- **Untyped escape hatches:** `Questions::add_raw(name, json)` sends a question type this SDK
  doesn't model yet, and `result.answers()` / `result.find(name)` return
  `std::variant<NoulAnswer, ChoiceAnswer, ScoreAnswer, UnknownAnswer>`.

Alternatives considered:

- **String-literal template labels** (`choice<"calm", "angry">`): closest to TS, but needs C++20
  and heavy templates, and produces poor error messages. Enums are the idiomatic C++ equivalent.
- **Caller-defined response structs** (like Python's `response_model`): useful, but C++ has no
  reflection. Deferred to `system_one_as<T>()` using nlohmann `from_json`.
- **Variant map only** (Python-style dicts): kept as the untyped layer, not the primary API.

Builder note: `choice("q", {"a", "b"})` is deliberately a compile error, because a braced list of
two C strings is also a valid `std::string` iterator pair. Use `choice("q", "a", "b")` for labels
without descriptions, or `choice("q", {{"a", "desc"}, {"b", nullptr}})` for described labels.

## 5. Architecture

```
Client (move-only; unique_ptr<ClientImpl>)
 │   system_one_async / list_models_async ──► Call (state machine) + Slot<T> shared with Pending<T>
 │   pump():
 │     1. transport->poll()                     (curl_multi_perform, or nothing for transports with their own thread)
 │     2. drain completion queue (mutex; transports may finish on any thread)
 │     3. advance each Call:  Sending → [response|error] → Done | Waiting(retry at t) → Sending
 │     4. handle cancel / abandoned flags, retry timers, total budgets
 │     5. fill Slots, run callbacks
 │   system_one / list_models = submit + loop { pump(); transport->wait(until next timer) }
 │
 ├─ client.cpp       Client, ClientImpl::pump, per-call state machine: headers, attempts,
 │                   status → Error, backoff, budget, cancellation, logs
 ├─ config.cpp       options + env → validated Config
 ├─ wire.cpp         detail::Wire: request JSON, response decoding and validation, error
 │                   messages, ErrorKind mapping, Retry-After parsing
 ├─ questions.cpp    Questions::add validation and wire encoding
 ├─ logging.cpp      level filter, stderr sink, header redaction
 ├─ headers.cpp      case-insensitive Headers
 ├─ result.cpp       to_string(ErrorKind)
 ├─ util.cpp         decimal and HTTP-date parsing, UTF-8-safe JSON output
 ├─ transport.cpp    Transport::wait default (built with RTTI; see §8)
 └─ curl_transport.cpp   default Transport: one curl_multi handle, driven from poll()
```

| Header | Contents |
|---|---|
| `libtypesafe/libtypesafe.hpp` | umbrella + `version`, `api_version` |
| `libtypesafe/client.hpp` | `Client`, `Pending<T>`, `ClientOptions`, `CallOptions`, `LogLevel`, env names |
| `libtypesafe/result.hpp` | `Result<T>`, `Error`, `ErrorKind`, `LIBTYPESAFE_ASSERT` |
| `libtypesafe/questions.hpp` | question structs, builders, `Questions`, `Key<A>`, `choice_labels` |
| `libtypesafe/answers.hpp` | answer structs, `SystemOneResult`, `Usage`, `ResponseMeta`, `ModelInfo` |
| `libtypesafe/retry.hpp` | `RetryPolicy` |
| `libtypesafe/transport.hpp` | `Transport` interface, `HttpRequest`/`HttpResponse`/`TransportError`, `make_curl_transport` |
| `libtypesafe/json.hpp` | `Json` alias, `Headers` |

**Pimpl, and no curl in public headers.** A custom transport implements `Transport::start` and
`cancel`, and optionally `poll` and `wait`. That is the only integration point with a different
network stack.

**nlohmann/json is in the public API.** `libtypesafe::Json` is `nlohmann::ordered_json`, so keys
stay in insertion order, as they do in Python dicts and JS objects. It is header-only, works with
exceptions and RTTI off (it aborts on misuse such as `at()` with a missing key), and a plain
`nlohmann::json` converts implicitly. The SDK uses only its non-throwing parse and accessors.
See open question 2 for applications that bring their own JSON.

## 6. Behavior contract

Ported from the official SDKs. Where the two differ, section 7 records the choice made.

### 6.1 Configuration

`Client::create(options)` returns `Result<Client>`. Values resolve in this order: explicit
option, then environment, then default. Blank environment values are ignored.

| Option | Env | Default | Validation |
|---|---|---|---|
| `api_key` | `TYPESAFE_API_KEY` | required | trimmed; printable ASCII; no internal whitespace |
| `base_url` | `TYPESAFE_BASE_URL` | `https://api.typesafe.ai` | trailing `/` stripped |
| `default_model` | `TYPESAFE_DEFAULT_MODEL` | `jev-latest` | |
| `log_level` | `TYPESAFE_LOG_LEVEL` | `warn` | `debug`, `info`, `warn`/`warning`, `error`, `off` |
| `timeout` | none | 10 s per attempt | > 0 |
| `retry` | none | see 6.4 | retries ≥ 0; delays ≥ 0; jitter 0 to 1; statuses 100 to 999 |

A bad value yields `ErrorKind::invalid_argument`. The key never appears in any message. Shipped
applications should pass `api_key` explicitly (or better, proxy through their own backend; see
open question 4) rather than rely on the environment.

### 6.2 Requests

Headers are merged case-insensitively, and later sources win: `default_headers`, then
`CallOptions::headers`, then the SDK's own headers:

| Header | Value |
|---|---|
| `Authorization` | `Bearer <key>` |
| `Accept` | `application/json` |
| `Content-Type` | `application/json`, only when there is a body |
| `User-Agent`, `X-TypeSafe-SDK` | `libtypesafe/<version>` |
| `X-TypeSafe-Runtime` | e.g. `cpp/201703 (clang 21.0.0; darwin; arm64)` |
| `X-TypeSafe-Retry-Count` | `1`, `2`, ... on retries only; stripped from user headers |

The body is `{"state", "model", "questions"}`, and then `extra_body` is shallow-merged over it
(last write wins). Questions are encoded as follows:

- Null `instructions` are omitted.
- Noul `criteria` are omitted when both descriptions are null.
- Choice criteria are an object in label order. Score criteria are an array.

`Questions::add` never fails on the spot. It records the first problem, and any call with that
set then completes with `invalid_argument` without sending anything. The problems are:

- a duplicate name
- an empty or duplicate choice label
- a score rubric with fewer than 1 entry

An empty question set fails the same way.

### 6.3 Responses and errors

**2xx.** The body is parsed as JSON. Unknown fields are ignored. An answer with an unknown `type`
becomes an `UnknownAnswer` and logs a warning. Anything malformed, or not matching the questions
(§4), yields `response_validation` with `field_path`, e.g. `answers.tone.confidence`.

**Non-2xx.** The body is kept as parsed JSON if it parses, otherwise as text. The message is the
first match of:

0. a plain-text (non-JSON) body, whole
1. `error` (string)
2. `error.message`
3. `message`
4. `detail` (string)
5. `detail.message`
6. `detail[]`, formatted as `"<loc minus 'body'>: <msg>; ..."`
7. the raw body, truncated to 200 chars
8. `"status code (no body)"`

`Error::message` reads `POST https://api.typesafe.ai/v1/systemone: 422 questions.x.criteria: Field required (request_id=…)`.

| `ErrorKind` | Cause | Official SDK class |
|---|---|---|
| `invalid_argument` | bad options or questions; nothing sent | `TypeSafeError` |
| `bad_request` | 400 | `BadRequestError` |
| `authentication` | 401 | `AuthenticationError` |
| `permission_denied` | 403 | `PermissionDeniedError` |
| `not_found` | 404 | `NotFoundError` |
| `unprocessable_entity` | 422 | `UnprocessableEntityError` |
| `rate_limited` | 429 (`retry_after` set when the headers give one) | `RateLimitError` |
| `server_error` | ≥ 500 | `InternalServerError` |
| `http_error` | any other non-2xx | `APIError` |
| `response_validation` | malformed 2xx body | `APIResponseValidationError` (Python) |
| `connection` | no response | `APIConnectionError` |
| `timeout` | attempt exceeded `timeout` | `APITimeoutError` |
| `cancelled` | `cancel()`, handle dropped, or client destroyed | `APIUserAbortError` (JS) |

`Error` also carries `status`, `body`, `headers`, `request_id`, `request_sent`, and `attempts`.

### 6.4 Retries

```
on attempt failure e:
    if e.kind == cancelled or not retryable(e) or attempts > max_retries: complete with e
    delay = e.retry_after if respect_retry_after and ≤ max_retry_after
            else min(backoff_initial · 2^(attempts−1), backoff_max) · (1 − rand() · jitter)
    if total_budget and elapsed + delay ≥ total_budget: complete with e
    state = Waiting(until now + delay)          // pump() resends when due; cancel() ends it

retryable(e) = e.status ∈ http_statuses                         // default 408, 429, 500–599
             ∨ (e.kind == timeout    ∧ retry_timeouts            ∧ (¬e.request_sent ∨ retry_after_send))
             ∨ (e.kind == connection ∧ retry_connection_errors   ∧ (¬e.request_sent ∨ retry_after_send))
             ∨ predicate(e)
```

`retry_after` reads `retry-after-ms` first, then `Retry-After`, which may be in seconds or an
HTTP-date. A negative or invalid value falls back to backoff. Defaults: 2 retries, 500 ms initial,
5 s cap, 0.25 jitter, `Retry-After` honored up to 60 s, 30 s total budget. A `CallOptions::retry`
replaces the client's policy for that call. The clock and RNG are internal seams so the tests are
deterministic.

**Double-billing control (`retry_after_send`).** When a timeout or dropped connection happens
*after* the request was fully sent, the server may already have evaluated, and billed, that
attempt. `retry_after_send = false` retries only failures that certainly happened before sending
(DNS, connect, TLS, a partially uploaded body). The curl transport treats a request as sent once
libcurl reached the send stage (`CURLINFO_PRETRANSFER_TIME_T > 0`) and uploaded the whole body
(`CURLINFO_SIZE_UPLOAD_T`). `CURLINFO_REQUEST_SIZE` is not usable: libcurl 8.7 reports 0 after a
timeout that followed a fully sent request. A custom transport that can't tell leaves
`request_sent = true`, the cautious default. The default for `retry_after_send` is `true`, to
match the official SDKs. It can be set per client or per call:

```cpp
ts::CallOptions careful;
careful.retry = client_policy;               // or a fresh RetryPolicy
careful.retry->retry_after_send = false;
```

The API has no idempotency key. If TypeSafe added one, a retry could be deduplicated on the server
and this trade-off would disappear. See open question 6.

### 6.5 Timeouts and cancellation

- **Per attempt:** `timeout` covers the whole attempt, including the response body
  (`CURLOPT_TIMEOUT_MS`), as in the JS SDK. Connect timeout is `CurlOptions`, 5 s.
- **Per call:** `RetryPolicy::total_budget`.
- **Cancellation** has three triggers:
  - `Pending::cancel()`
  - destroying an unfinished `Pending`
  - destroying the `Client`

  The next `pump()` calls `Transport::cancel` for any in-flight attempt, or clears a retry timer.
  The call then completes as `cancelled`. For abandoned handles and a destroyed client, the
  callback does not run. Cancellation is immediate: `curl_multi_remove_handle` runs in the same
  `pump()`.

### 6.6 Logging

`LogSink` is `std::function<void(LogLevel, std::string_view)>` and is called on the client's
thread, so it can go straight to the application's own log. The default sink writes to stderr
with the prefix `[libtypesafe]`.

- `info` logs one line per attempt, e.g. `#3 POST /v1/systemone <- 200 in 312ms (request abc)`,
  plus retry, timeout, and cancel lines.
- `debug` adds headers and bodies.

Redacted to `***`: `Authorization`, `Proxy-Authorization`, `X-API-Key`, `Api-Key`, `Cookie`,
`Set-Cookie`, and any header whose name contains `token` or `secret`. Bodies are **not** redacted,
as in both official SDKs. State may contain user text, so `debug` is opt-in.

### 6.7 Threading

- `Client` is **thread-compatible**: one thread at a time, typically the main or UI thread. That keeps
  the hot path lock-free. A worker thread that needs its own calls gets its own `Client`. A
  custom `Transport` may be shared across threads if it is thread-safe; the curl transport is
  not, so share one only between clients on the same thread.
- `Transport` implementations may call `done` from any thread. The only lock is on the client's
  completion queue.
- Results and callbacks are delivered only inside `pump()` or a blocking call, on the thread that
  called it.
- A callback may submit new calls. It must not call the blocking methods.
- The default curl transport creates no threads apart from libcurl's threaded DNS resolver. Its
  one `curl_multi` handle reuses connections and caches DNS and TLS sessions across calls.
  `curl_global_init` runs once unless `CurlOptions::global_init = false`.
- A callback must not destroy the `Client` that is running it.

### 6.8 Observed live behavior (2026-09-25, jev-1.13.0)

From `tests/test_live.cpp` and a side-by-side run against the official Python SDK 0.7.1:

- **Wire parity:** for the same questions (all three types, described and undescribed labels,
  non-ASCII state), libtypesafe's request body is byte-identical to the Python SDK's.
- **Answers vary run to run.** Identical requests get slightly different probabilities (one score
  ranged 1.21 to 1.27 across three official-SDK calls). libtypesafe's answers fall inside that
  spread; exact equality between clients is not expected.
- **`GET /v1/models`** returns `jev-latest` and `jev-preview`. `release_date` is a full ISO 8601
  timestamp, not the `YYYY-MM-DD` the OpenAPI schema describes.
- **An unknown model is a 400** (`Unknown model: <name>`), not a 404 or 422.
- **A bad key is a 401**, and is not retried.
- **Server validation (422) paths include the question type**, e.g.
  `questions.broken.score.criteria: List should have at least 1 item after validation, not 0`.
- Every response carries `x-typesafe-request-id` (`req_…`).
- **Latency:** about 290 ms for a five-question call; six concurrent calls over one `curl_multi`
  handle finished in about 380 ms.

## 7. Where the official SDKs disagree

| Topic | Python 0.7.1 | JS 0.6.0 | libtypesafe |
|---|---|---|---|
| Error model | exception classes | exception classes | **`Result<T>` + `ErrorKind`** (§1) |
| Concurrency | sync client + asyncio client | promises | **`Pending<T>` + `pump()`**, blocking helpers |
| Minimum score rubric | 1 | 2 | **1**: matches OpenAPI `min_length: 1` |
| Unknown answer type | dropped with a warning | passed through untyped | **kept** as `UnknownAnswer`, with a warning |
| 2xx body validation | yes, with `field_path` | none (cast) | **yes**, and answers must match the questions |
| Total retry budget | 30 s | none | **30 s** |
| `Retry-After` cap | none | 60 s, then backoff | **60 s** |
| Retry after the request was sent | always | always | **configurable** (`retry_after_send`, default on) |
| API key check | trimmed, printable ASCII, no spaces | present | **Python's** |
| Header redaction | full `***`, plus `token`/`secret` names | keeps last 4 chars | **Python's** |
| Null `instructions` | omitted | sent as `null` | **omitted** |
| Timeout meaning | httpx per-phase (connect/read/write/pool) | whole attempt, including body | **whole attempt** |
| Error text | `endpoint: status msg (request_id=…)` | `status msg` | **Python's** |
| Extra body fields | `extra_body` | extra request properties | `CallOptions::extra_body` |
| Custom retry rule | `predicate`, `exceptions` | none | `predicate` |
| Raw response | `raw_http_response`, `request_id` | `asResponse()`, `withResponse()` | `result.meta()` |

## 8. Build, dependencies, and packaging

- **C++17**, with no exceptions and no RTTI required. On GCC and Clang the library itself is
  compiled with exceptions and RTTI disabled. On MSVC it disables only RTTI (`/GR-`): it still
  never throws, but `_HAS_EXCEPTIONS=0` changes the MSVC STL and would clash with callers built
  normally. The exception is `transport.cpp`: it holds `Transport`'s key function, so
  it is built with RTTI to emit `Transport`'s typeinfo. Without that, a caller built with RTTI
  (the compiler default) could not link a `Transport` subclass. Callers may use either setting.
- **Windows:** `curl_transport.cpp` is built with `NOMINMAX` and `WIN32_LEAN_AND_MEAN`, because
  `curl.h` includes `windows.h`. UBSan's `vptr` check can't be used on the library (it needs RTTI),
  so CI's sanitizer job passes `-fno-sanitize=vptr`.
- **nlohmann in a mixed build:** nlohmann is header-only, so its inline functions are compiled
  both in the library (exceptions off: errors abort) and in callers (exceptions on: errors throw).
  The library only uses nlohmann's non-throwing calls, so neither path is reached from it.
- **nlohmann_json ≥ 3.11** is a public dependency, header-only, found with `find_package` or
  fetched with `FetchContent`.
- **libcurl ≥ 7.84** is private, needed only when the default transport is built. Tests use
  Catch2 v3 and cpp-httplib.
- **CMake** target `libtypesafe::libtypesafe` produces `libtypesafe.a` / `typesafe.lib`. The
  library is always static: a shared build would need an export macro (`LIBTYPESAFE_API`) on the
  public classes.
- **Options**, each defaulting to on only when libtypesafe is the top-level project:
  `LIBTYPESAFE_BUILD_TESTS`, `LIBTYPESAFE_BUILD_EXAMPLES`, `LIBTYPESAFE_BUILD_API_CHECKS`, and
  `LIBTYPESAFE_INSTALL`. `LIBTYPESAFE_WITH_CURL` (on) and `LIBTYPESAFE_FETCH_DEPS` (on) default to
  on everywhere. A configure-time check keeps `libtypesafe::version` equal to the CMake project
  version.
- **Install** produces the headers, the static library, `libtypesafeConfig.cmake` (with
  `find_dependency` on nlohmann_json and, for the curl transport, CURL),
  `libtypesafeTargets.cmake`, a version file (`SameMinorVersion` while 0.x), and the license. A
  fetched nlohmann_json is installed alongside it. Test-only dependencies are not installed.
- **vcpkg:** an overlay port in `ports/libtypesafe` builds this checkout. Publishing to a registry
  means swapping its `SOURCE_PATH` for `vcpkg_from_github` once a release is tagged.
- **Conan 2:** `conanfile.py` plus `test_package/` (`conan create .`). It requires
  `nlohmann_json/3.11.3` and `libcurl/[>=8.6 <9]`.
- **Embedding:** link the static library, call `pump()` from your loop, route `LogSink` to your
  log, and define `LIBTYPESAFE_ASSERT` to your own check macro if you have one.

## 9. Testing

`ctest` runs 55 entries: 49 unit test cases, 4 compile-fail checks, and 2 package checks
(`-L package`). The live suite (`-L live`) is opt-in.

- **Core, with a `FakeTransport` and a fake clock:** request headers and body; results delivered
  only by `pump()`; callbacks, including ones that submit calls or try to block; the backoff
  schedule and retry-count header; `Retry-After` and its cap; non-retryable statuses and
  `predicate`; the total budget; `retry_after_send` with `request_sent` true and false;
  connection errors; `cancel()` in flight and during a retry wait; dropped handles; a destroyed
  client; rejected calls; response validation; completions from another thread; the timeout
  backstop; blocking calls; log redaction; and an idle `pump()` making zero allocations (counted
  with a global `operator new`).
- **Wire:** exact request JSON, decoding of all answer types, every validation `field_path`,
  status mapping, error-message extraction, and `Retry-After` (ms, seconds, HTTP-date).
- **Config:** env fallbacks, precedence, and validation, and that the key never appears in
  messages.
- **libcurl against a local cpp-httplib server:** a round trip, non-2xx responses, a timeout after
  sending (`request_sent = true`), connection refused (`request_sent = false`), cancel
  mid-transfer, and the client end to end with a 503 `retry-after-ms` retry.
- **Compile-fail** (`tests/compile_fail/`): a wrong field on a key, an enum without
  `choice_labels`, a braced label list, and copying a `Pending` must each fail to build *with the
  expected error* (`check.cmake`), in clang, GCC, and MSVC wording.
- **Package** (`tests/consumer/`): a separate project builds and runs against `cmake --install`
  output (`find_package`) and against the source tree (`add_subdirectory`). It subclasses
  `Transport` with RTTI on and links the curl transport. The install check also fails if
  test-only dependencies leak into the package.
- **Live** (`tests/test_live.cpp`, 8 cases): models; every question type in one call; JSON state
  and a raw question; six concurrent async calls; a bad key (401); a server-side 422; an unknown
  model (400); and an unmeetable timeout. All pass against jev-1.13.0 (§6.8).
- **Parity with the official Python SDK:** checked once by hand (§6.8), with byte-identical
  request bodies. This is not an automated test.
- **OpenAPI drift:** `tools/check-openapi.sh` diffs `spec/openapi.json` against the live schema
  (weekly in CI). As of 2026-09-25 they match.

Where it has run (2026-09-25):

| Platform | Result |
|---|---|
| macOS arm64, Apple clang 21 | all 55 + live 8 pass; clean under ASan+UBSan and TSan |
| Linux arm64 (Docker `gcc:14`), GCC 14.4 | all 55 pass, 0 warnings |
| vcpkg overlay port, arm64-osx | installs (with curl from source); the consumer builds and runs against it |
| Conan 2 recipe, macOS | `conan create .` builds against Conan Center's nlohmann_json and libcurl (OpenSSL); `test_package` runs |
| GitHub Actions: Windows MSVC (x64, vcpkg curl), Linux GCC 14 and Clang 18, macOS, ASan+UBSan, TSan, vcpkg + Conan packages | all green (run 36179229027) |

Not done:

- **Upstream coverage:** the upstream suites are covered by behavior, not ported case by case
  (about 270 upstream cases vs 49 here).
- **OpenAPI golden tests** built from the spec's examples.

## 10. Milestones

| | Scope | Status |
|---|---|---|
| **M0** | This doc, public headers, API checks | ✅ headers compile in C++17/20 with exceptions and RTTI on and off |
| **M1** | `Headers`, `Result`/`Error`, question encoding, response decoding, config | ✅ (OpenAPI golden tests still open) |
| **M2** | Call state machine, `pump()`, retries, cancellation, logging, blocking helpers, with `FakeTransport` | ✅ idle `pump()` allocates nothing |
| **M3** | `curl_multi` transport | ✅ local-server tests, live suite, and Python-SDK parity pass; ASan/UBSan/TSan clean |
| **M4** | Install/export, vcpkg/Conan, CI matrix (macOS clang, Linux GCC/Clang, Windows MSVC) | ✅ `find_package` and `add_subdirectory` consumers pass; vcpkg and Conan verified; the CI matrix, Windows included, is green. Released as 0.1.0 |
| Later | `system_one_as<T>`, export macro for shared builds, allocator hooks, `raw_json` state | as needed |

## 11. Open questions

1. **Exceptions opt-in?** Resolved as no for v1: `Result` works in every configuration. A thin
   throwing wrapper (`value_or_throw()`) could be added later for tools code.
2. **JSON interop.** Hosts that already have their own JSON type must convert to `Json`. It may be
   worth also accepting pre-serialized state (`ts::raw_json(std::string)`) so hot paths skip
   nlohmann entirely.
3. **Identity.** Resolved: the project, CMake package, and namespace are `libtypesafe`; the
   headers are `<libtypesafe/...>`; the headers sent are `libtypesafe/<ver>`. This avoids
   colliding with a future official `typesafe` C++ SDK. License: MIT, like upstream.
4. **API keys in distributed apps.** Resolved: the README's "API keys in shipped apps" section
   says to route through your own backend (`base_url` + `default_headers`) for anything beyond
   personal or internal builds.
5. **Idempotency.** `retry_after_send` makes the double-billing risk configurable (resolved), but
   the proper fix is an `Idempotency-Key` header on the server. It is worth requesting from
   TypeSafe.
6. **Strict or lenient decoding.** An unknown choice label or a missing answer is an error here,
   while new answer types and fields are tolerated. That is stricter than upstream.
