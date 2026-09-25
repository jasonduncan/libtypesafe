#include "libtypesafe/transport.hpp"

#include <algorithm>
#include <thread>

namespace libtypesafe {

// Transport's key function: this file is built with RTTI so Transport's typeinfo exists for
// subclasses compiled with RTTI (see CMakeLists.txt).
void Transport::wait(std::chrono::milliseconds max_wait) {
  const auto nap = std::clamp(max_wait, std::chrono::milliseconds(0), std::chrono::milliseconds(10));
  if (nap.count() > 0) std::this_thread::sleep_for(nap);
}

#ifndef LIBTYPESAFE_WITH_CURL
std::shared_ptr<Transport> make_curl_transport(CurlOptions) { return nullptr; }
#endif

}  // namespace libtypesafe
