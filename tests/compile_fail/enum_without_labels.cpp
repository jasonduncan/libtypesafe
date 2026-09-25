// An enum used as choice labels needs a choice_labels specialization.
#include <libtypesafe/libtypesafe.hpp>
namespace ts = libtypesafe;
enum class Unlabeled { a, b };
void f() {
  ts::Questions q;
  (void)q.add("x", ts::choice<Unlabeled>("?"));
}
