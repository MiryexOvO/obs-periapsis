#include "../native/uplink-budget.hpp"
#include <cassert>
int main() {
    UplinkBudget b(20000);
    assert(b.next(1000) == 20000);
    b.pressure(1000); assert(b.next(1000) == 15000);
    b.pressure(1100); assert(b.next(1999) == 15000);
    assert(b.next(2000) == 11250);
    for (uint64_t now=3000; now<=20000; now+=1000) { b.pressure(now); b.next(now); }
    assert(b.current == 1000);
    assert(!b.can_raise(29999)); assert(b.can_raise(30000));
    assert(b.next(30000) == 1050); assert(b.next(30001) == 1050);
    for (uint64_t now=40000; now<1000000; now+=10000) b.next(now);
    assert(b.current == 20000);
    UplinkBudget small(500); small.pressure(1000); assert(small.next(1000) == 500);
}
