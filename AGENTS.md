# libtypesafe — agent instructions

An unofficial C++17 client library for the TypeSafe AI API (`https://api.typesafe.ai`), ported from
the official Python and JS SDKs. Started 2026-09-25.

## Status

M1–M4 done (2026-09-25): the library, the curl transport, and its tests, which are clean under
ASan/UBSan/TSan and pass live against jev-1.13.0, sending requests byte-identical to the Python SDK. It also has
install/export, a vcpkg overlay port, a Conan recipe, and a GitHub Actions matrix. Read `docs/DESIGN.md`
first. It holds the design constraints (§1), the behavior contract (§6), the decisions where the
official SDKs disagree (§7), test status (§9), milestones, and open questions.

## Hard rules for the public API

- C++17. Must compile with `-fno-exceptions -fno-rtti`. The SDK never throws; failures are
  `Result<T>` / `Error`.
- Never block in `pump()`, and allocate nothing there when idle.
- No curl types in public headers; networking goes through `Transport`.
- Change headers together with `docs/DESIGN.md`.

## Layout

- `include/libtypesafe/`: public API, namespace `libtypesafe`.
- `src/`: implementation, built without exceptions or RTTI (except `transport.cpp`; see DESIGN §8).
- `examples/`: compiled by the `libtypesafe_api_check_*` targets on every build.
- `spec/openapi.json`: API schema snapshot (v0.2.0, fetched 2026-09-25). Refresh deliberately and
  record what changed.
- `tests/`: Catch2 v3 with a fake transport and fake clock (`fakes.hpp`), plus libcurl tests against a
  local cpp-httplib server. `compile_fail/` holds misuse that must not compile. `consumer/` is a
  separate project that must build against the installed and the vendored library.
  `test_live.cpp` is opt-in.
- `cmake/`: the package config template. `ports/libtypesafe/`: vcpkg overlay port.
  `conanfile.py` + `test_package/`: Conan 2 recipe.
- `.github/workflows/ci.yml`: Linux GCC/Clang, macOS, Windows MSVC, sanitizers, packages, a live
  job (manual), and a weekly OpenAPI drift check (`tools/check-openapi.sh`).

## Build / check

```sh
cmake -S . -B build && cmake --build build   # fetches nlohmann_json if not installed
```

```sh
ctest --test-dir build -LE live         # unit, compile-fail (label-less), and package tests
ctest --test-dir build -L package       # just the install/add_subdirectory consumer tests
LIBTYPESAFE_LIVE=1 ctest --test-dir build -L live   # real API calls; needs TYPESAFE_API_KEY
tools/check-openapi.sh                  # spec/openapi.json vs the live schema
```

The build also compiles the examples in four modes: C++17 and C++20, each with and without
exceptions and RTTI.

## Reference implementations

- https://github.com/typesafe-ai/typesafe-sdk-python (v0.7.1 @ 0ffd094)
- https://github.com/typesafe-ai/typesafe-sdk-js (v0.6.0 @ 66880cc)

Port behavior and test cases from these rather than inventing new behavior. Where they differ,
follow DESIGN §7.

## Boundaries

- Live tests call the real API with `TYPESAFE_API_KEY` from the environment and spend a few
  tokens per run. Never send private data in them.
- Never print or commit API keys.
