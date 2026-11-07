// Local-overload adaptation, not a bandwidth estimator. / 本地过载退让，不是带宽估计器。
#pragma once
#include <cstdint>
struct UplinkBudget {
    uint32_t ceiling, floor, current;
    uint64_t last_pressure = 0, last_change = 0;
    bool pending = false, seen = false;
    explicit UplinkBudget(uint32_t bitrate) : ceiling(bitrate),
        floor(bitrate < 1000 ? bitrate : 1000), current(bitrate) {}
    void pressure(uint64_t now) { last_pressure = now; pending = seen = true; }
    bool pressured(uint64_t now) const { return seen && now - last_pressure < 1000; }
    bool can_raise(uint64_t now) const {
        return current < ceiling && now - last_pressure >= 10000 && now - last_change >= 10000;
    }
    uint32_t next(uint64_t now) {
        if (pending && now - last_change >= 1000) {
            const auto reduced = current * 3 / 4;
            current = reduced < floor ? floor : reduced;
            pending = false;
            last_change = now;
        } else if (can_raise(now)) {
            const auto step = current / 20 ? current / 20 : 1;
            current = ceiling - current < step ? ceiling : current + step;
            last_change = now;
        }
        return current;
    }
};
