// One encoder-callback producer, one feedback consumer. No callback allocation or I/O.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
struct KeyEvent {
    const char *event = "", *reason = "";
    uint64_t id=0, pts=0, wall=0, bytes=0, queued=0, occupied=0;
};
struct KeyTrace {
    static constexpr uint32_t capacity=128;
    std::array<KeyEvent,capacity> events{};
    std::atomic<uint32_t> head{0},tail{0};
    std::atomic<uint64_t> lost{0};
    void push(const KeyEvent &e) {
        const auto h=head.load(std::memory_order_relaxed);
        if(h-tail.load(std::memory_order_acquire)>=capacity){++lost;return;}
        events[h%capacity]=e;head.store(h+1,std::memory_order_release);
    }
    bool pop(KeyEvent &e) {
        const auto t=tail.load(std::memory_order_relaxed);
        if(t==head.load(std::memory_order_acquire))return false;
        e=events[t%capacity];tail.store(t+1,std::memory_order_release);return true;
    }
};
