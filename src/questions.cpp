#include "libtypesafe/questions.hpp"

#include <set>

namespace libtypesafe {

bool Questions::accept_name(const std::string& name) {
  if (name.empty()) {
    fail("Question names must be non-empty.");
    return false;
  }
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      fail("Duplicate question name \"" + name + "\".");
      return false;
    }
  }
  return true;
}

void Questions::fail(std::string message) {
  if (error_) return;  // keep the first problem
  Error error;
  error.kind = ErrorKind::invalid_argument;
  error.message = std::move(message);
  error_ = std::move(error);
}

Key<NoulAnswer> Questions::add(std::string name, NoulQuestion question) {
  if (accept_name(name)) {
    Json wire = {{"type", "noul"}};
    if (!question.instructions.is_null()) wire["instructions"] = std::move(question.instructions);
    if (!question.criteria.yes.is_null() || !question.criteria.no.is_null()) {
      Json criteria = Json::object();
      if (!question.criteria.yes.is_null()) criteria["true"] = std::move(question.criteria.yes);
      if (!question.criteria.no.is_null()) criteria["false"] = std::move(question.criteria.no);
      wire["criteria"] = std::move(criteria);
    }
    entries_.push_back({name, "noul", std::move(wire), {}, 0});
  }
  return Key<NoulAnswer>(std::move(name));
}

Key<ChoiceAnswer> Questions::add(std::string name, ChoiceQuestion question) {
  if (accept_name(name)) {
    std::vector<std::string> labels;
    std::set<std::string> seen;
    Json criteria = Json::object();
    bool valid = true;
    if (question.criteria.empty()) {
      fail("Choice question \"" + name + "\" has no labels.");
      valid = false;
    }
    for (auto& [label, description] : question.criteria) {
      if (!valid) break;
      if (label.empty()) {
        fail("Choice question \"" + name + "\" has an empty label.");
        valid = false;
      } else if (!seen.insert(label).second) {
        fail("Choice question \"" + name + "\" repeats the label \"" + label + "\".");
        valid = false;
      } else {
        labels.push_back(label);
        criteria[label] = std::move(description);
      }
    }
    if (valid) {
      Json wire = {{"type", "choice"}};
      if (!question.instructions.is_null()) wire["instructions"] = std::move(question.instructions);
      wire["criteria"] = std::move(criteria);
      entries_.push_back({name, "choice", std::move(wire), std::move(labels), 0});
    }
  }
  return Key<ChoiceAnswer>(std::move(name));
}

Key<ScoreAnswer> Questions::add(std::string name, ScoreQuestion question) {
  if (accept_name(name)) {
    if (question.criteria.empty()) {
      fail("Score question \"" + name + "\" has no criteria; at least one score is required.");
    } else {
      const std::size_t levels = question.criteria.size();
      Json wire = {{"type", "score"}};
      if (!question.instructions.is_null()) wire["instructions"] = std::move(question.instructions);
      wire["criteria"] = Json(std::move(question.criteria));
      entries_.push_back({name, "score", std::move(wire), {}, levels});
    }
  }
  return Key<ScoreAnswer>(std::move(name));
}

void Questions::add_raw(std::string name, Json question) {
  if (!accept_name(name)) return;
  const auto type = question.is_object() ? question.find("type") : question.end();
  if (!question.is_object() || type == question.end() || !type->is_string() ||
      type->get_ref<const std::string&>().empty()) {
    fail("Question \"" + name + "\" must be an object with a nonempty string \"type\".");
    return;
  }
  const std::string type_name = type->get<std::string>();
  const auto criteria = question.find("criteria");
  if ((type_name == "choice" || type_name == "score") && criteria == question.end()) {
    fail("Question \"" + name + "\" requires \"criteria\".");
    return;
  }
  Entry entry{name, type_name, {}, {}, 0};
  if (type_name == "choice" && criteria->is_object()) {
    for (auto it = criteria->begin(); it != criteria->end(); ++it) entry.labels.push_back(it.key());
  } else if (type_name == "score" && criteria->is_array()) {
    entry.levels = criteria->size();
  }
  entry.wire = std::move(question);
  entries_.push_back(std::move(entry));
}

Json Questions::to_wire() const {
  Json wire = Json::object();
  for (const auto& entry : entries_) wire[entry.name] = entry.wire;
  return wire;
}

}  // namespace libtypesafe
