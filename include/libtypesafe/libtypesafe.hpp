#pragma once

#include <string_view>

#include "libtypesafe/answers.hpp"
#include "libtypesafe/client.hpp"
#include "libtypesafe/json.hpp"
#include "libtypesafe/questions.hpp"
#include "libtypesafe/result.hpp"
#include "libtypesafe/retry.hpp"
#include "libtypesafe/transport.hpp"

namespace libtypesafe {

inline constexpr std::string_view version = "0.1.0";

/// The API schema this version was written against (spec/openapi.json).
inline constexpr std::string_view api_version = "0.2.0";

}  // namespace libtypesafe
