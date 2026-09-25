// Port of examples/demo.ts from the official JS SDK, using the blocking calls. Needs TYPESAFE_API_KEY.
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <utility>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;

enum class Tone { calm, frustrated, angry };

template <>
struct libtypesafe::choice_labels<Tone> {
  static constexpr std::array values{
      std::pair{Tone::calm, std::string_view{"calm"}},
      std::pair{Tone::frustrated, std::string_view{"frustrated"}},
      std::pair{Tone::angry, std::string_view{"angry"}},
  };
};

int main() {
  ts::ClientOptions options;
  options.log_level = ts::LogLevel::info;
  options.timeout = std::chrono::seconds(15);
  auto client = ts::Client::create(options);
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }

  if (auto models = client->list_models()) {
    std::cout << "Available models:";
    for (const auto& model : *models) std::cout << ' ' << model.name;
    std::cout << '\n';
  }

  const ts::Json ticket = {
      {"subject", "Charged twice this month"},
      {"body",
       "Hi, I see two charges of $49 on my card for August. I only have one account. "
       "Please fix this ASAP, I'm pretty frustrated."},
  };

  ts::Questions q;
  auto is_billing = q.add("isBilling", ts::noul("Is this ticket about billing?"));
  auto tone = q.add("sentiment", ts::choice<Tone>("What is the customer's tone?"));
  auto urgency = q.add("urgency", ts::score("How urgent is this ticket?",
                                            {"can wait", "this week", "today", "right now"}));
  auto refund = q.add("refundRisk",
                      ts::noul("The customer will demand a refund.",
                               {"Asks for money back or threatens a chargeback", nullptr}));

  const auto result = client->system_one(ticket, q);
  if (!result) {
    const ts::Error& e = result.error();
    std::cerr << ts::to_string(e.kind) << " (request " << e.request_id.value_or("unknown")
              << "): " << e.message << '\n';
    return 1;
  }

  // Each answer's type follows from the key that indexes it.
  const ts::SystemOneResult& r = *result;
  std::cout << "billing?     " << r[is_billing].noul << '\n';
  const auto sentiment = r[tone];  // EnumChoiceAnswer<Tone>
  std::cout << "angry?       " << (sentiment.choice == Tone::angry) << " ("
            << sentiment.probability(Tone::angry) << ")\n";
  std::cout << "urgency      " << r[urgency].score << " -> "
            << r[urgency].legend[r[urgency].level()] << '\n';
  std::cout << "refund risk  " << r[refund].yes(0.7) << '\n';
  std::cout << "tokens       " << r.usage().input_tokens.value_or(0) << " in\n";
}
