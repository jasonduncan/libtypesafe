#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtypesafe/json.hpp"
#include "libtypesafe/result.hpp"

namespace libtypesafe {

// ---------------------------------------------------------------------------
// Enum-typed choices
// ---------------------------------------------------------------------------

/// Specialize to use an enum as a choice question's labels:
///
///     template <> struct libtypesafe::choice_labels<Tone> {
///       static constexpr std::array values{
///           std::pair{Tone::calm, std::string_view{"calm"}},
///           std::pair{Tone::angry, std::string_view{"angry"}},
///       };
///     };
template <class E>
struct choice_labels;

template <class E, class = void>
struct has_choice_labels : std::false_type {};
template <class E>
struct has_choice_labels<E, std::void_t<decltype(choice_labels<E>::values)>> : std::true_type {};

template <class E>
inline constexpr bool is_choice_enum_v = std::is_enum_v<E> && has_choice_labels<E>::value;

namespace detail {
struct Wire;
}  // namespace detail

// Answer types, so a Key can name the answer its question produces. Defined in answers.hpp.
struct NoulAnswer;
struct ChoiceAnswer;
struct ScoreAnswer;
template <class E>
struct EnumChoiceAnswer;

// ---------------------------------------------------------------------------
// Questions
// ---------------------------------------------------------------------------

/// Descriptions of the yes and no outcomes. Serialized as `{"true": ..., "false": ...}`;
/// omitted from the request when both are null.
struct NoulCriteria {
  Json yes = nullptr;
  Json no = nullptr;
};

/// A yes/no question or statement.
struct NoulQuestion {
  Json instructions = nullptr;
  NoulCriteria criteria;
};

/// Select one of several named labels. Each description is text, JSON, or null.
struct ChoiceQuestion {
  Json instructions = nullptr;
  std::vector<std::pair<std::string, Json>> criteria;
};

/// Rate against an ordered rubric; a description's index is its score level.
struct ScoreQuestion {
  Json instructions = nullptr;
  std::vector<Json> criteria;
};

/// A choice question whose labels come from `choice_labels<E>`.
template <class E>
struct EnumChoiceQuestion {
  static_assert(is_choice_enum_v<E>, "specialize libtypesafe::choice_labels<E> for this enum");
  Json instructions = nullptr;
  /// Optional descriptions; labels without one are sent as null.
  std::vector<std::pair<E, Json>> descriptions;
};

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

[[nodiscard]] inline NoulQuestion noul(Json instructions = nullptr, NoulCriteria criteria = {}) {
  return {std::move(instructions), std::move(criteria)};
}

/// `choice("What is the tone?", "calm", "angry")`: undescribed labels.
template <class... Labels,
          std::enable_if_t<(sizeof...(Labels) > 0) &&
                               (std::is_convertible_v<Labels, std::string_view> && ...),
                           int> = 0>
[[nodiscard]] ChoiceQuestion choice(Json instructions, Labels&&... labels) {
  ChoiceQuestion q{std::move(instructions), {}};
  (q.criteria.emplace_back(std::string(std::string_view(labels)), nullptr), ...);
  return q;
}

/// Deleted: a braced pair of C strings is also a std::string iterator pair, which compiles into
/// nonsense or an unreadable error. Write `choice("q", "a", "b")` for labels, or
/// `choice("q", {{"a", "description"}, {"b", nullptr}})` for described labels.
ChoiceQuestion choice(Json instructions, std::initializer_list<const char*> labels) = delete;

/// `choice("What is the tone?", {{"calm", "Neutral or polite"}, {"angry", nullptr}})`.
[[nodiscard]] inline ChoiceQuestion choice(Json instructions,
                                           std::vector<std::pair<std::string, Json>> criteria) {
  return {std::move(instructions), std::move(criteria)};
}

/// `choice<Tone>("What is the tone?")`: labels from `choice_labels<Tone>`.
template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
[[nodiscard]] EnumChoiceQuestion<E> choice(Json instructions = nullptr,
                                           std::vector<std::pair<E, Json>> descriptions = {}) {
  return {std::move(instructions), std::move(descriptions)};
}

/// `score("How urgent?", {"can wait", "this week", "today"})`: level i is described by rubric[i].
[[nodiscard]] inline ScoreQuestion score(Json instructions, std::vector<Json> rubric) {
  return {std::move(instructions), std::move(rubric)};
}

// ---------------------------------------------------------------------------
// Question sets and typed keys
// ---------------------------------------------------------------------------

/// A typed handle to one answer. Index a SystemOneResult with it to get `Answer` directly.
/// Default-constructed keys are empty placeholders and must not be used for indexing.
template <class Answer>
class Key {
 public:
  Key() = default;
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  friend class Questions;
  explicit Key(std::string name) : name_(std::move(name)) {}
  std::string name_;
};

/// Named questions for one System One request.
///
/// `add` never fails on the spot. A duplicate name, an empty choice, a duplicate label, or an
/// empty score rubric is recorded, and a call with these questions returns that
/// `invalid_argument` error without sending anything.
class Questions {
 public:
  Key<NoulAnswer> add(std::string name, NoulQuestion question);
  Key<ChoiceAnswer> add(std::string name, ChoiceQuestion question);
  Key<ScoreAnswer> add(std::string name, ScoreQuestion question);

  template <class E>
  Key<EnumChoiceAnswer<E>> add(std::string name, EnumChoiceQuestion<E> question) {
    ChoiceQuestion plain{std::move(question.instructions), {}};
    for (const auto& [value, label] : choice_labels<E>::values) {
      Json description = nullptr;
      for (auto& [described, text] : question.descriptions) {
        if (described == value) description = std::move(text);
      }
      plain.criteria.emplace_back(std::string(label), std::move(description));
    }
    Key<ChoiceAnswer> key = add(std::move(name), std::move(plain));
    return Key<EnumChoiceAnswer<E>>(key.name());
  }

  /// Escape hatch for question types this SDK version does not model. The request sends `question`
  /// as is; read its answer through `SystemOneResult::find`.
  void add_raw(std::string name, Json question);

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  /// The first problem recorded by `add`, if any.
  [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

  /// The `questions` object sent on the wire, in insertion order.
  [[nodiscard]] Json to_wire() const;

 private:
  friend struct detail::Wire;  // request encoding and answer validation

  bool accept_name(const std::string& name);
  void fail(std::string message);

  struct Entry {
    std::string name;
    std::string type;                 // "noul", "choice", "score", or a raw question's type
    Json wire;                        // serialized question
    std::vector<std::string> labels;  // choice labels, used to validate the answer
    std::size_t levels = 0;           // score rubric length, used to validate the answer
  };
  std::vector<Entry> entries_;
  std::optional<Error> error_;
};

}  // namespace libtypesafe
