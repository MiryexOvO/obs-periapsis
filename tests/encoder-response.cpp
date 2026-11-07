#include "../native/encoder-response.hpp"
#include <cassert>
int main(){
    EncoderResponse e;
    assert(!e.observe(0,4000,26000));assert(!e.observe(2000,4000,26000));
    assert(!e.observe(3000,4000,26000));assert(!e.observe(4000,4000,26000));
    assert(e.observe(5000,4000,26000));assert(e.limited);
    for(uint64_t t=6000;t<10000;t+=1000)assert(!e.observe(t,4000,4000));
    assert(e.observe(10000,4000,4000));assert(!e.limited);
    assert(!e.observe(11000,2000,10000));assert(!e.observe(12000,2000,10000));
    assert(!e.limited);
}
