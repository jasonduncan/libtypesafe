// Non-blocking use from a game loop: submit, keep rendering, pump once per frame.
// Needs TYPESAFE_API_KEY.
#include <iostream>
#include <string>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;

/// An NPC that classifies what the player said, without stalling the frame.
class Npc {
 public:
  void on_player_said(ts::Client& client, const std::string& line) {
    ts::Questions q;
    hostile_ = q.add("hostile", ts::noul("The player is threatening this character."));
    intent_ = q.add("intent", ts::choice("What does the player want?", "trade", "quest", "chat"));
    // Replacing the handle cancels any earlier, still-running classification.
    pending_ = client.system_one_async(line, std::move(q));
  }

  void update() {
    if (!pending_.done()) return;
    auto result = pending_.take();  // empties the handle, so this runs once per request
    if (!result) {
      std::cerr << "classification failed: " << result.error().message << '\n';
      return;
    }
    if ((*result)[hostile_].yes(0.8)) std::cout << "NPC draws sword\n";
    std::cout << "NPC thinks you want to " << (*result)[intent_].choice << '\n';
  }

 private:
  ts::Pending<ts::SystemOneResult> pending_;
  ts::Key<ts::NoulAnswer> hostile_;
  ts::Key<ts::ChoiceAnswer> intent_;
};

int main() {
  auto client = ts::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }

  Npc npc;
  npc.on_player_said(*client, "Hand over the map or else.");

  for (int frame = 0; frame < 600; ++frame) {
    client->pump();  // non-blocking: transfers, retries, completions
    npc.update();
    // ... render ...
  }
}
