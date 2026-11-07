#include "../native/key-trace.hpp"
#include <cassert>
#include <thread>
int main(){
    KeyTrace q;KeyEvent e;
    for(uint64_t i=0;i<129;++i)q.push({"key_encoded","",i});
    assert(q.lost==1);
    for(uint64_t i=0;i<128;++i){assert(q.pop(e));assert(e.id==i);}
    assert(!q.pop(e));
    std::atomic<bool> done{false};
    std::thread writer([&]{for(uint64_t i=0;i<100000;++i)q.push({"key_encoded","",i});done=true;});
    uint64_t count=0,last=0;
    while(!done || q.tail!=q.head){
        if(q.pop(e)){if(count)assert(e.id>last);last=e.id;++count;}
    }
    writer.join();assert(count+q.lost-1==100000);
}
