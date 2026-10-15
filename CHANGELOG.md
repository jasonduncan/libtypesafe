# Changelog

## 0.2.1 (2026-10-14)

Version fix: the sources now report 0.2.1 (CMake, `libtypesafe::version` and User-Agent, Conan, vcpkg). The v0.2.0 tag (62e4f2c) has the image support but still reports 0.1.0.

## 0.2.0 (2026-10-14)

Images and Box (Jev 1.14), following the Python SDK 0.7.5. TypeSafe labels these
"Dreamer-ready" (not production-ready).

- `CallOptions::assets` (`Assets::add_image(placeholder, bytes)`): images referenced by
  placeholder anywhere text goes (state, instructions, criteria), sent as `multipart/form-data`
  (JSON part `request`, file parts `asset-<i>`), with the same request JSON as the Python SDK
- `TypeSafe-Danger-Zone-Images: true` is added to every request with an asset or a Box question
- `inspect_image` and `Assets` check images before sending: PNG/JPEG still (no animated PNG),
  16 MiB per file, 256-1568 px per side, 78,400-2,458,624 px area, 32 MiB per request; report
  EXIF orientation and progressive JPEGs
- `box()` questions and a typed `BoxAnswer` (`BoxCoordinates` + optional `confidence`), checked
  on decode; no more "unrecognized type" warning for box answers
- `new_placeholder()` (UUID v4); multi-frame state is a JSON array of frames
- base64 assets in `extra_body["assets"]` still work, and replace `CallOptions::assets`

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
