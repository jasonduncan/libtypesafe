#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtypesafe/answers.hpp"
#include "libtypesafe/json.hpp"
#include "libtypesafe/questions.hpp"
#include "libtypesafe/result.hpp"
#include "libtypesafe/transport.hpp"

namespace libtypesafe::detail {

/// Where a response came from, for error messages and warnings.
struct DecodeContext {
  /// `POST https://api.typesafe.ai/v1/systemone`, without credentials.
  std::string endpoint;
  int attempts = 0;
  /// Receives warnings such as an answer type this version does not model.
  std::function<void(std::string_view)> warn;
};

/// The TypeSafe wire format. A friend of Questions and SystemOneResult.
struct Wire {
  /// `{"state", "model", "questions"}` with `extra_body` shallow-merged over it.
  static Result<std::string> encode_system_one(const Json& state, const Questions& questions,
                                               const std::string& model, const Json& extra_body);

  /// Decode a 2xx System One response and check it answers `questions`.
  static Result<SystemOneResult> decode_system_one(const HttpResponse& response,
                                                   const Questions& questions,
                                                   const DecodeContext& context);

  static Result<std::vector<ModelInfo>> decode_models(const HttpResponse& response,
                                                      const DecodeContext& context);

  /// The Error for a non-2xx response.
  static Error api_error(const HttpResponse& response, const DecodeContext& context);

  static ErrorKind kind_for_status(int status);

  /// The message the official SDKs extract from an error body, or nullopt.
  static std::optional<std::string> extract_message(const Json& body);

  /// `retry-after-ms`, then `Retry-After` (seconds or HTTP-date), relative to `now`.
  static std::optional<std::chrono::milliseconds> parse_retry_after(
      const Headers& headers, std::chrono::system_clock::time_point now);
};

/// Strip `user:password@` from a URL, for messages.
std::string without_userinfo(std::string_view url);

}  // namespace libtypesafe::detail
