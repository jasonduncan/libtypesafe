// Mirrors the quickstart in the official Python and JS READMEs. Needs TYPESAFE_API_KEY.
#include <iostream>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;

int main() {
  auto client = ts::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }

  ts::Questions questions;
  auto category = questions.add(
      "category", ts::choice("What is this ticket about?", "billing", "technical", "other"));

  auto result = client->system_one({{"document", "I was charged twice. Please fix this ASAP."}},
                                   questions);
  if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
  std::cout << (*result)[category].choice << '\n';
}
