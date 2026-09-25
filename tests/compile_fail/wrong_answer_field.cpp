// A score key indexes a ScoreAnswer, which has no `choice`.
#include <libtypesafe/libtypesafe.hpp>
namespace ts = libtypesafe;
void f(const ts::SystemOneResult& r) {
  ts::Questions q;
  auto urgency = q.add("urgency", ts::score("How urgent?", {"low", "high"}));
  (void)r[urgency].choice;
}
