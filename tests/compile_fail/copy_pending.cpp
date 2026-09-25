// A Pending owns its call; it can be moved, not copied.
#include <libtypesafe/libtypesafe.hpp>
namespace ts = libtypesafe;
void f(ts::Pending<ts::SystemOneResult>& pending) {
  ts::Pending<ts::SystemOneResult> copy = pending;
  (void)copy;
}
