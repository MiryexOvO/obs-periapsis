// Compare measured output with requested bitrate after a settling interval.
// 等待收敛后比较实际编码输出与请求值，防止把设置成功当作能力保证。
#pragma once
#include <cstdint>
struct EncoderResponse {
    uint32_t target=0, bad=0, good=0;
    uint64_t changed=0;
    bool limited=false;
    bool observe(uint64_t now,uint32_t requested,uint64_t actual) {
        const bool previous=limited;
        if(requested!=target){target=requested;changed=now;bad=good=0;}
        if(!target || now-changed<3000)return false;
        if(actual>uint64_t(target)*3/2+250){good=0;if(bad<3)++bad;if(bad==3)limited=true;}
        else {bad=0;if(good<5)++good;if(good==5)limited=false;}
        return previous!=limited;
    }
};
