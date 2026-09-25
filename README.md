# libtypesafe

An unofficial C++ client for [TypeSafe AI](https://typesafe.ai), modeled on the official
[Python](https://github.com/typesafe-ai/typesafe-sdk-python) and
[JS](https://github.com/typesafe-ai/typesafe-sdk-js) SDKs.

- C++17, and works with exceptions and RTTI disabled: failures are `Result<T>` values
- Non-blocking: calls return a `Pending<T>`, and `client.pump()` runs from your loop (blocking calls too)
- Answer types are checked at compile time
- Pluggable HTTP transport (libcurl by default, or your own)
- Sends byte-identical requests to the official Python SDK (checked against the live API)

```cpp
#include <libtypesafe/libtypesafe.hpp>
namespace ts = libtypesafe;

auto client = ts::Client::create();  // reads TYPESAFE_API_KEY

ts::Questions q;
auto topic = q.add("topic", ts::choice("What is this about?", "billing", "technical", "other"));
auto pending = client->system_one_async("I was charged twice.", std::move(q));

// in your loop
client->pump();
if (pending.done()) {
  if (auto result = pending.take()) route_to((*result)[topic].choice);
}
```

Blocking calls (`client->system_one(...)`, `client->list_models()`) return the same `Result`.
See [`examples/`](examples) and [docs/DESIGN.md](docs/DESIGN.md).

## Use it in a CMake project

Requires CMake ≥ 3.24, a C++17 compiler, nlohmann_json ≥ 3.11 (fetched when missing), and libcurl ≥
7.84 for the default transport. It builds as a static library.

**Vendored** (a submodule, a copy, or `FetchContent`):

```cmake
add_subdirectory(third_party/libtypesafe)   # tests and examples stay off when not top-level
target_link_libraries(app PRIVATE libtypesafe::libtypesafe)
```

**Installed:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /opt/libtypesafe
```

```cmake
find_package(libtypesafe 0.1 REQUIRED)       # CMAKE_PREFIX_PATH=/opt/libtypesafe
target_link_libraries(app PRIVATE libtypesafe::libtypesafe)
```

**vcpkg** (overlay port in this repo; builds curl too):

```sh
vcpkg install libtypesafe --overlay-ports=path/to/libtypesafe/ports
```

**Conan 2** (recipe in this repo):

```sh
conan create path/to/libtypesafe --build=missing -s compiler.cppstd=17
```

Then `find_package(libtypesafe CONFIG REQUIRED)` as above.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build -LE live     # offline: unit, compile-fail, and packaging tests
TYPESAFE_API_KEY=... LIBTYPESAFE_LIVE=1 ctest --test-dir build -L live   # real API calls
```

## API keys in shipped apps

A key compiled into a program you give to other people can be extracted. For anything beyond
personal or internal builds, point `ClientOptions::base_url` at your own backend, which holds the
key and forwards requests, and authenticate your users there (`default_headers` can carry your own
token).

## Releases

See [CHANGELOG.md](CHANGELOG.md). Versions follow semver; while 0.x, minor versions may change the
API, and the CMake package version check (`SameMinorVersion`) enforces that.

## License

MIT. Not affiliated with TypeSafe AI.
