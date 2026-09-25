#include "wire.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "util.hpp"

namespace libtypesafe::detail {

namespace {

constexpr std::size_t max_error_body_length = 200;
constexpr std::string_view request_id_header = "x-typesafe-request-id";

std::optional<std::string> request_id_of(const Headers& headers) {
  if (auto id = headers.get(request_id_header)) return std::string(*id);
  return std::nullopt;
}

std::string with_context(const std::string& endpoint, std::string text,
                         const std::optional<std::string>& request_id) {
  std::string message = endpoint.empty() ? std::move(text) : endpoint + ": " + text;
  if (request_id) message += " (request_id=" + *request_id + ")";
  return message;
}

Error invalid_response(const HttpResponse& response, std::string path, const DecodeContext& context) {
  Error error;
  error.kind = ErrorKind::response_validation;
  error.status = response.status;
  error.body = body_value(response.body);
  error.headers = response.headers;
  error.request_id = request_id_of(response.headers);
  error.attempts = context.attempts;
  error.message = with_context(context.endpoint,
                               std::to_string(response.status) + " Invalid response data at '" + path + "'.",
                               error.request_id);
  error.field_path = std::move(path);
  return error;
}

const Json* member(const Json& object, const char* key) {
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

bool read_number(const Json& object, const char* key, double& out) {
  const Json* value = member(object, key);
  if (value == nullptr || !value->is_number()) return false;
  out = value->get<double>();
  return std::isfinite(out);
}

/// "0", "1", ... with no sign, spaces, or leading zeros.
std::optional<std::size_t> level_index(const std::string& key) {
  if (key.empty() || key.size() > 9 || (key.size() > 1 && key[0] == '0')) return std::nullopt;
  std::size_t value = 0;
  for (char c : key) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  return value;
}

/// Convert an object keyed "0".."n-1" into a vector indexed by level.
template <class T, class Read>
bool read_levels(const Json& object, std::vector<T>& out, Read read) {
  if (!object.is_object()) return false;
  std::vector<std::optional<T>> slots(object.size());
  for (auto it = object.begin(); it != object.end(); ++it) {
    const auto index = level_index(it.key());
    if (!index || *index >= slots.size() || slots[*index]) return false;
    std::optional<T> value = read(it.value());
    if (!value) return false;
    slots[*index] = std::move(value);
  }
  out.clear();
  for (auto& slot : slots) out.push_back(std::move(*slot));
  return true;
}

std::optional<double> finite_number(const Json& value) {
  if (!value.is_number()) return std::nullopt;
  const double number = value.get<double>();
  if (!std::isfinite(number)) return std::nullopt;
  return number;
}

std::string format_validation_errors(const Json& detail) {
  std::string joined;
  for (const Json& entry : detail) {
    if (!entry.is_object()) continue;
    const Json* msg = member(entry, "msg");
    if (msg == nullptr || !msg->is_string()) continue;
    std::string location;
    if (const Json* loc = member(entry, "loc"); loc != nullptr && loc->is_array()) {
      for (const Json& part : *loc) {
        std::string text;
        if (part.is_string()) {
          text = part.get<std::string>();
          if (text == "body") continue;
        } else if (part.is_number_integer()) {
          text = std::to_string(part.get<std::int64_t>());
        } else {
          continue;
        }
        if (!location.empty()) location += '.';
        location += text;
      }
    }
    if (!joined.empty()) joined += "; ";
    joined += location.empty() ? msg->get<std::string>() : location + ": " + msg->get<std::string>();
  }
  return joined;
}

}  // namespace

std::string without_userinfo(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) return std::string(url);
  const std::size_t host_start = scheme + 3;
  const std::size_t host_end = url.find('/', host_start);
  const std::size_t at = url.substr(0, host_end).find('@', host_start);
  if (at == std::string_view::npos) return std::string(url);
  return std::string(url.substr(0, host_start)) + std::string(url.substr(at + 1));
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

Result<std::string> Wire::encode_system_one(const Json& state, const Questions& questions,
                                            const std::string& model, const Json& extra_body) {
  if (!extra_body.is_null() && !extra_body.is_object()) {
    Error error;
    error.message = "extra_body must be a JSON object.";
    return error;
  }
  Json body = Json::object();
  body["state"] = state;
  body["model"] = model;
  body["questions"] = questions.to_wire();
  if (extra_body.is_object()) {
    for (auto it = extra_body.begin(); it != extra_body.end(); ++it) body[it.key()] = it.value();
  }
  return dump(body);
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

Result<SystemOneResult> Wire::decode_system_one(const HttpResponse& response, const Questions& questions,
                                                const DecodeContext& context) {
  const auto fail = [&](std::string path) -> Result<SystemOneResult> {
    return invalid_response(response, std::move(path), context);
  };
  const auto document = parse_json(response.body);
  if (!document || !document->is_object()) return fail("");
  const Json& doc = *document;

  SystemOneResult result;
  const Json* model = member(doc, "model");
  if (model == nullptr || !model->is_string()) return fail("model");
  result.model_ = model->get<std::string>();

  const Json* usage = member(doc, "usage");
  if (usage == nullptr || !usage->is_object()) return fail("usage");
  for (const auto& [key, target] : {std::pair{"input_tokens", &result.usage_.input_tokens},
                                    std::pair{"output_tokens", &result.usage_.output_tokens}}) {
    const Json* count = member(*usage, key);
    if (count == nullptr || count->is_null()) continue;
    if (!count->is_number_integer()) return fail(std::string("usage.") + key);
    *target = count->get<std::int64_t>();
  }

  static const Json no_answers = Json::object();
  const Json* answers = member(doc, "answers");
  if (answers == nullptr || answers->is_null()) answers = &no_answers;
  if (!answers->is_object()) return fail("answers");

  const auto entry_named = [&](const std::string& name) -> const Questions::Entry* {
    for (const auto& entry : questions.entries_) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  };

  for (auto it = answers->begin(); it != answers->end(); ++it) {
    const std::string& name = it.key();
    const Json& raw = it.value();
    const std::string at = "answers." + name;
    const Json* type = raw.is_object() ? member(raw, "type") : nullptr;
    if (type == nullptr || !type->is_string()) return fail(at + ".type");
    const std::string& type_name = type->get_ref<const std::string&>();

    if (type_name == "noul") {
      NoulAnswer answer;
      if (!read_number(raw, "noul", answer.noul)) return fail(at + ".noul");
      result.answers_.emplace_back(name, answer);
    } else if (type_name == "choice") {
      ChoiceAnswer answer;
      const Json* choice = member(raw, "choice");
      if (choice == nullptr || !choice->is_string()) return fail(at + ".choice");
      answer.choice = choice->get<std::string>();
      if (!read_number(raw, "confidence", answer.confidence)) return fail(at + ".confidence");
      const Json* probabilities = member(raw, "probabilities");
      if (probabilities == nullptr || !probabilities->is_object()) return fail(at + ".probabilities");
      for (auto p = probabilities->begin(); p != probabilities->end(); ++p) {
        const auto value = finite_number(p.value());
        if (!value) return fail(at + ".probabilities." + p.key());
      }
      // Question order first, then any labels the question did not list.
      const Questions::Entry* entry = entry_named(name);
      if (entry != nullptr) {
        for (const auto& label : entry->labels) {
          const auto p = probabilities->find(label);
          if (p != probabilities->end()) answer.probabilities.emplace_back(label, p->get<double>());
        }
      }
      for (auto p = probabilities->begin(); p != probabilities->end(); ++p) {
        const bool listed = entry != nullptr && std::find(entry->labels.begin(), entry->labels.end(),
                                                          p.key()) != entry->labels.end();
        if (!listed) answer.probabilities.emplace_back(p.key(), p.value().get<double>());
      }
      result.answers_.emplace_back(name, std::move(answer));
    } else if (type_name == "score") {
      ScoreAnswer answer;
      if (!read_number(raw, "score", answer.score)) return fail(at + ".score");
      if (!read_number(raw, "confidence", answer.confidence)) return fail(at + ".confidence");
      const Json* legend = member(raw, "legend");
      if (legend == nullptr ||
          !read_levels(*legend, answer.legend, [](const Json& v) { return std::optional<Json>(v); })) {
        return fail(at + ".legend");
      }
      const Json* probabilities = member(raw, "probabilities");
      if (probabilities == nullptr || !read_levels(*probabilities, answer.probabilities, finite_number)) {
        return fail(at + ".probabilities");
      }
      result.answers_.emplace_back(name, std::move(answer));
    } else {
      if (context.warn) {
        context.warn("answer \"" + name + "\" has unrecognized type \"" + type_name +
                     "\"; kept as UnknownAnswer");
      }
      result.answers_.emplace_back(name, UnknownAnswer{type_name, raw});
    }
  }

  // Every question must be answered, with the type (and label or levels) it asked for.
  for (const auto& entry : questions.entries_) {
    const std::string at = "answers." + entry.name;
    const Answer* answer = result.find(entry.name);
    if (answer == nullptr) return fail(at);
    if (entry.type == "noul") {
      if (!std::holds_alternative<NoulAnswer>(*answer)) return fail(at + ".type");
    } else if (entry.type == "choice") {
      const auto* choice = std::get_if<ChoiceAnswer>(answer);
      if (choice == nullptr) return fail(at + ".type");
      if (!entry.labels.empty() &&
          std::find(entry.labels.begin(), entry.labels.end(), choice->choice) == entry.labels.end()) {
        return fail(at + ".choice");
      }
    } else if (entry.type == "score") {
      const auto* score = std::get_if<ScoreAnswer>(answer);
      if (score == nullptr) return fail(at + ".type");
      if (entry.levels != 0) {
        if (score->legend.size() != entry.levels) return fail(at + ".legend");
        if (score->probabilities.size() != entry.levels) return fail(at + ".probabilities");
      }
    }
  }

  result.meta_.status = response.status;
  result.meta_.headers = response.headers;
  result.meta_.body = response.body;
  result.meta_.request_id = request_id_of(response.headers);
  result.meta_.attempts = context.attempts;
  return result;
}

Result<std::vector<ModelInfo>> Wire::decode_models(const HttpResponse& response,
                                                   const DecodeContext& context) {
  const auto fail = [&](std::string path) -> Result<std::vector<ModelInfo>> {
    return invalid_response(response, std::move(path), context);
  };
  const auto document = parse_json(response.body);
  if (!document || !document->is_object()) return fail("");
  const Json* models = member(*document, "models");
  if (models == nullptr || !models->is_array()) return fail("models");
  std::vector<ModelInfo> out;
  for (std::size_t i = 0; i < models->size(); ++i) {
    const Json& raw = (*models)[i];
    const std::string at = "models[" + std::to_string(i) + "]";
    if (!raw.is_object()) return fail(at);
    ModelInfo info;
    for (const auto& [key, target] : {std::pair{"name", &info.name}, std::pair{"description", &info.description},
                                      std::pair{"release_date", &info.release_date}}) {
      const Json* value = member(raw, key);
      if (value == nullptr || !value->is_string()) return fail(at + "." + key);
      *target = value->get<std::string>();
    }
    out.push_back(std::move(info));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

ErrorKind Wire::kind_for_status(int status) {
  switch (status) {
    case 400: return ErrorKind::bad_request;
    case 401: return ErrorKind::authentication;
    case 403: return ErrorKind::permission_denied;
    case 404: return ErrorKind::not_found;
    case 422: return ErrorKind::unprocessable_entity;
    case 429: return ErrorKind::rate_limited;
    default: return status >= 500 ? ErrorKind::server_error : ErrorKind::http_error;
  }
}

std::optional<std::string> Wire::extract_message(const Json& body) {
  const auto nonempty = [](std::string text) -> std::optional<std::string> {
    if (text.empty()) return std::nullopt;
    return text;
  };
  if (body.is_string()) return nonempty(body.get<std::string>());
  if (!body.is_object()) return std::nullopt;
  const Json* error = member(body, "error");
  const Json* message = member(body, "message");
  const Json* detail = member(body, "detail");
  if (error != nullptr && error->is_string()) return nonempty(error->get<std::string>());
  if (error != nullptr && error->is_object()) {
    const Json* inner = member(*error, "message");
    if (inner != nullptr && inner->is_string()) return nonempty(inner->get<std::string>());
  }
  if (message != nullptr && message->is_string()) return nonempty(message->get<std::string>());
  if (detail != nullptr && detail->is_string()) return nonempty(detail->get<std::string>());
  if (detail != nullptr && detail->is_object()) {
    const Json* inner = member(*detail, "message");
    if (inner != nullptr && inner->is_string()) return nonempty(inner->get<std::string>());
  }
  if (detail != nullptr && detail->is_array()) return nonempty(format_validation_errors(*detail));
  return std::nullopt;
}

Error Wire::api_error(const HttpResponse& response, const DecodeContext& context) {
  Error error;
  error.kind = kind_for_status(response.status);
  error.status = response.status;
  error.body = body_value(response.body);
  error.headers = response.headers;
  error.request_id = request_id_of(response.headers);
  error.retry_after = parse_retry_after(response.headers, std::chrono::system_clock::now());
  error.attempts = context.attempts;

  std::string detail;
  if (auto extracted = extract_message(error.body)) {
    detail = std::move(*extracted);
  } else if (error.body.is_null()) {
    detail = "status code (no body)";
  } else {
    detail = truncate_utf8(error.body.is_string() ? error.body.get<std::string>() : dump(error.body),
                           max_error_body_length);
  }
  error.message =
      with_context(context.endpoint, std::to_string(response.status) + " " + detail, error.request_id);
  return error;
}

std::optional<std::chrono::milliseconds> Wire::parse_retry_after(const Headers& headers,
                                                                 std::chrono::system_clock::time_point now) {
  using std::chrono::milliseconds;
  const auto to_ms = [](double value) { return milliseconds(static_cast<std::int64_t>(std::llround(value))); };
  if (auto raw = headers.get("retry-after-ms")) {
    const auto text = trim(*raw);
    const auto value = text.empty() ? std::optional<double>(0.0) : parse_decimal(text);
    if (value && *value >= 0 && *value < 1e15) return to_ms(*value);
  }
  if (auto raw = headers.get("retry-after")) {
    const auto text = trim(*raw);
    if (text.empty()) return milliseconds(0);
    if (auto seconds = parse_decimal(text)) {
      if (*seconds < 0 || *seconds >= 1e12) return std::nullopt;
      return to_ms(*seconds * 1000);
    }
    if (auto date = parse_http_date(text)) {
      const auto at = std::chrono::system_clock::time_point(std::chrono::seconds(*date));
      const auto delay = std::chrono::duration_cast<milliseconds>(at - now);
      return delay.count() > 0 ? delay : milliseconds(0);
    }
  }
  return std::nullopt;
}

}  // namespace libtypesafe::detail
