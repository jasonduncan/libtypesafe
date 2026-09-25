#include "libtypesafe/result.hpp"

namespace libtypesafe {

std::string_view to_string(ErrorKind kind) noexcept {
  switch (kind) {
    case ErrorKind::invalid_argument: return "invalid_argument";
    case ErrorKind::bad_request: return "bad_request";
    case ErrorKind::authentication: return "authentication";
    case ErrorKind::permission_denied: return "permission_denied";
    case ErrorKind::not_found: return "not_found";
    case ErrorKind::unprocessable_entity: return "unprocessable_entity";
    case ErrorKind::rate_limited: return "rate_limited";
    case ErrorKind::server_error: return "server_error";
    case ErrorKind::http_error: return "http_error";
    case ErrorKind::response_validation: return "response_validation";
    case ErrorKind::connection: return "connection";
    case ErrorKind::timeout: return "timeout";
    case ErrorKind::cancelled: return "cancelled";
  }
  return "unknown";
}

}  // namespace libtypesafe
