// {"a", "b"} is also a std::string iterator pair, so it is rejected; write choice("?", "a", "b").
#include <libtypesafe/libtypesafe.hpp>
namespace ts = libtypesafe;
void f() {
  ts::Questions q;
  q.add("x", ts::choice("?", {"a", "b"}));
}
