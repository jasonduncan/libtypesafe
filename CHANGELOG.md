# Changelog

## 0.1.0 (2026-09-25)

First release. An unofficial C++17 client for the TypeSafe AI API, ported from the official
Python SDK 0.7.1 and JS SDK 0.6.0 against API schema 0.2.0.

- `POST /v1/systemone` and `GET /v1/models`, blocking and non-blocking (`Pending<T>` +
  `Client::pump()`)
- Typed answer keys: noul, choice (string or enum labels), and score; using the wrong answer
  type is a compile error
- Responses are checked against the questions asked; unknown answer types are kept as
  `UnknownAnswer`
- `Result<T>` errors with `ErrorKind`; builds and links with exceptions and RTTI disabled
- Retries matching the official SDKs (backoff, `Retry-After`, total budget), plus
  `retry_after_send` to avoid retrying requests the server may already have billed
- Pluggable `Transport`; the default is libcurl `curl_multi`, driven from `pump()`
- Logging with credential redaction
- CMake package (`find_package(libtypesafe)`), vcpkg overlay port, Conan 2 recipe
- Tested on macOS (Apple clang), Linux (GCC 14, Clang 18), and Windows (MSVC), under ASan,
  UBSan, and TSan; the live suite passes against jev-1.13.0
