// Bounded recovery pacing, independent of the media writer.
// 有界恢复节流，与媒体发送线程独立。
#pragma once
#include <cstdint>

struct RecoveryGate {
    static constexpr uint64_t cooldown_ms = 333;
    static constexpr uint64_t response_ms = 1000;
    bool pending = false, waiting = false, issued = false;
    uint64_t last = 0, baseline = 0;
    uint64_t requested_keys = 0;

    void request(uint64_t keys = 0) { pending = true; requested_keys = keys; }
    bool due(uint64_t now) const
    {
        return pending && !waiting && (!issued || now - last >= cooldown_ms);
    }
    void submitted(uint64_t now, uint64_t keys)
    {
        pending = false;
        waiting = issued = true;
        last = now;
        baseline = keys;
    }
    bool observed(uint64_t keys)
    {
        if (!waiting || keys <= baseline) return false;
        waiting = false;
        // A newly encoded key covers demands raised before that key existed.
        // 新关键帧覆盖其产生前的旧需求；产生后的新故障仍保留。
        if (pending && requested_keys < keys) pending = false;
        return true;
    }
    bool expired(uint64_t now)
    {
        if (!waiting || now - last < response_ms) return false;
        waiting = false;
        return true;
    }
};
