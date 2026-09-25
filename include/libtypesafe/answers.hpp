#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "libtypesafe/json.hpp"
#include "libtypesafe/questions.hpp"
#include "libtypesafe/result.hpp"

namespace libtypesafe {

/// A yes/no answer.
struct NoulAnswer {
  /// Probability of yes, from 0 to 1.
  double noul = 0;
  [[nodiscard]] bool yes(double threshold = 0.5) const noexcept { return noul >= threshold; }
};

/// The selected label and the probability of each label.
struct ChoiceAnswer {
  std::string choice;
  double confidence = 0;
  /// In the order the question listed its labels.
  std::vector<std::pair<std::string, double>> probabilities;

  /// Probability of `label`, or 0 when the server did not report it.
  [[nodiscard]] double probability(std::string_view label) const noexcept {
    for (const auto& [name, p] : probabilities) {
      if (name == label) return p;
    }
    return 0;
  }
};

/// An expected score with its rubric and per-level probabilities.
struct ScoreAnswer {
  /// Probability-weighted average of the levels; may fall between integers.
  double score = 0;
  double confidence = 0;
  /// Rubric descriptions indexed by level (the wire sends an object keyed "0", "1", ...).
  std::vector<Json> legend;
  /// Probabilities indexed by level.
  std::vector<double> probabilities;

  /// The most probable level.
  [[nodiscard]] std::size_t level() const noexcept {
    std::size_t best = 0;
    for (std::size_t i = 1; i < probabilities.size(); ++i) {
      if (probabilities[i] > probabilities[best]) best = i;
    }
    return best;
  }
};

/// A choice answer mapped onto the enum its question was built from.
template <class E>
struct EnumChoiceAnswer {
  static_assert(is_choice_enum_v<E>, "specialize libtypesafe::choice_labels<E> for this enum");
  E choice{};
  double confidence = 0;
  std::vector<std::pair<E, double>> probabilities;

  [[nodiscard]] double probability(E label) const noexcept {
    for (const auto& [value, p] : probabilities) {
      if (value == label) return p;
    }
    return 0;
  }
};

/// An answer whose type this SDK version does not model. Kept rather than dropped.
struct UnknownAnswer {
  std::string type;
  Json raw;
};

using Answer = std::variant<NoulAnswer, ChoiceAnswer, ScoreAnswer, UnknownAnswer>;

/// Token counts, when the server reports them.
struct Usage {
  std::optional<std::int64_t> input_tokens;
  std::optional<std::int64_t> output_tokens;
};

/// The HTTP response behind a result.
struct ResponseMeta {
  int status = 0;
  Headers headers;
  std::string body;
  /// `x-typesafe-request-id`, when present.
  std::optional<std::string> request_id;
  /// Attempts made, including the first.
  int attempts = 0;
};

/// Metadata for an available model.
struct ModelInfo {
  std::string name;
  std::string description;
  /// As reported. The OpenAPI schema says YYYY-MM-DD, but the live API returns a full ISO 8601
  /// timestamp, e.g. `2026-09-10T18:38:01.391457+00:00`.
  std::string release_date;
};

/// Answers to a System One request.
///
/// Decoding checks that every question has an answer of the matching type (and, for choices and
/// scores, a label or level the question defined), so indexing with a Key from the same Questions
/// always succeeds.
class SystemOneResult {
 public:
  [[nodiscard]] const std::string& model() const noexcept { return model_; }
  [[nodiscard]] const Usage& usage() const noexcept { return usage_; }
  [[nodiscard]] const ResponseMeta& meta() const noexcept { return meta_; }

  [[nodiscard]] const NoulAnswer& operator[](const Key<NoulAnswer>& key) const {
    return get<NoulAnswer>(key.name());
  }
  [[nodiscard]] const ChoiceAnswer& operator[](const Key<ChoiceAnswer>& key) const {
    return get<ChoiceAnswer>(key.name());
  }
  [[nodiscard]] const ScoreAnswer& operator[](const Key<ScoreAnswer>& key) const {
    return get<ScoreAnswer>(key.name());
  }

  template <class E>
  [[nodiscard]] EnumChoiceAnswer<E> operator[](const Key<EnumChoiceAnswer<E>>& key) const {
    const ChoiceAnswer& plain = get<ChoiceAnswer>(key.name());
    EnumChoiceAnswer<E> typed;
    typed.confidence = plain.confidence;
    for (const auto& [value, label] : choice_labels<E>::values) {
      if (label == plain.choice) typed.choice = value;
      typed.probabilities.emplace_back(value, plain.probability(label));
    }
    return typed;
  }

  /// All answers in response order, including ones from `Questions::add_raw`.
  [[nodiscard]] const std::vector<std::pair<std::string, Answer>>& answers() const noexcept {
    return answers_;
  }
  /// The answer named `name`, or null.
  [[nodiscard]] const Answer* find(std::string_view name) const noexcept {
    for (const auto& [n, a] : answers_) {
      if (n == name) return &a;
    }
    return nullptr;
  }

 private:
  friend struct detail::Wire;  // response decoding

  template <class A>
  const A& get(const std::string& name) const {
    const Answer* answer = find(name);
    const A* typed = answer ? std::get_if<A>(answer) : nullptr;
    // Only reachable with a Key from a different Questions object.
    LIBTYPESAFE_ASSERT(typed != nullptr, "Key does not belong to the Questions of this result");
    return *typed;
  }

  std::string model_;
  Usage usage_;
  std::vector<std::pair<std::string, Answer>> answers_;
  ResponseMeta meta_;
};

}  // namespace libtypesafe
