// SPDX-License-Identifier: GPL-2.0-or-later
// Native OBS encoded output for the PERI publisher. / PERI 发布端的 OBS 原生编码输出。
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <atomic>
#include <array>
#include <filesystem>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include "recovery-gate.hpp"
#include "uplink-budget.hpp"
#include "encoder-response.hpp"
#include "key-trace.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) { return "PERI AV1/Opus output"; }

namespace {
using Bytes = std::vector<uint8_t>;
uint64_t wall_ms()
{
    FILETIME time{}; GetSystemTimePreciseAsFileTime(&time);
    ULARGE_INTEGER ticks{}; ticks.LowPart=time.dwLowDateTime; ticks.HighPart=time.dwHighDateTime;
    return ticks.QuadPart/10000-11644473600000ULL;
}
constexpr size_t max_payload = 1024 * 1024 - 68;
extern "C" void *peri_live_open(const char *, void **, char *, size_t);
extern "C" bool peri_live_send(void *, const uint8_t *, size_t, char *, size_t);
extern "C" int32_t peri_live_send_budget(void *, const uint8_t *, size_t, uint32_t, char *, size_t);
extern "C" void peri_live_cancel(void *);
extern "C" void peri_live_close(void *);
extern "C" bool peri_register_catalog(const char *, const char *);
struct Recovery {
    uint16_t track;
    uint32_t origin;
    uint64_t id, viewer, after_sequence;
};
static_assert(sizeof(Recovery) == 32);
extern "C" void *peri_live_feedback_open(const void *);
extern "C" int32_t peri_live_feedback_poll(const void *, Recovery *);
struct Transport { uint64_t cwnd, rtt_us, sent_packets, lost_packets; };
static_assert(sizeof(Transport) == 32);
extern "C" int32_t peri_live_feedback_transport(const void *, Transport *);
extern "C" void peri_live_feedback_close(void *);

void append_be(Bytes &buf, uint64_t n, unsigned width)
{
    for (unsigned i = width; i; --i)
        buf.push_back(static_cast<uint8_t>(n >> ((i - 1) * 8)));
}

std::string extra(obs_encoder_t *encoder)
{
    uint8_t *ptr = nullptr;
    size_t size = 0;
    obs_encoder_get_extra_data(encoder, &ptr, &size);
    if (size > 4096 || (size && !ptr))
        throw std::runtime_error("invalid codec configuration");
    std::string value = "[";
    for (size_t i = 0; i < size; ++i) {
        if (i) value += ",";
        value += std::to_string(ptr[i]);
    }
    return value + "]";
}

struct Output {
    explicit Output(obs_output_t *o) : obs(o) {}
    obs_output_t *obs;
    // Opt in until encoded-byte measurements and impaired playback both pass.
    // 实际编码码率与弱网播放均验收前，仅显式开启实验策略。
    const bool uplink_experiment = GetEnvironmentVariableW(L"PERI_OBS_UPLINK_EXPERIMENT", nullptr, 0) != 0;
    // Candidate remains separate: it removed key rejection but regressed a 720p30 trial.
    const bool key_admission_experiment = uplink_experiment &&
        GetEnvironmentVariableW(L"PERI_OBS_KEY_ADMISSION", nullptr, 0) != 0;
    const bool diagnostics = GetEnvironmentVariableW(L"PERI_OBS_DIAGNOSTICS", nullptr, 0) != 0;
    KeyTrace key_trace;
    const char *rejection_reason = "";
    // OBS interleaves encoded packets on one producer; only the writer advances tail.
    // OBS 串行交付交织编码包，只有网络线程推进读指针。
    static constexpr uint32_t capacity = 32;
    std::array<Bytes, capacity> ring;
    std::array<uint64_t, capacity> queued_at{};
    std::array<uint64_t, capacity> queued_key_ids{};
    static constexpr uint64_t media_budget_ms = 750;
    static constexpr size_t byte_capacity = 2 * 1024 * 1024;
    static constexpr size_t key_reserve = max_payload + 17;
    std::atomic<uint64_t> queue_pressure{0};
    uint64_t drained_pressure=0;
    std::atomic<size_t> queued_bytes{0};
    std::atomic<uint32_t> head{0}, tail{0};
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;
    std::thread feedback_worker;
    HANDLE feedback_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    void *feedback = nullptr;
    obs_encoder_t *recovery_encoder = nullptr;
    std::atomic<bool> local_recovery{false};
    std::atomic<bool> congestion{false};
    uint32_t configured_bitrate = 0;
    std::atomic<bool> feedback_failed{false};
    std::atomic<uint64_t> encoded_keys{0};
    std::atomic<uint64_t> encoded_video_bytes{0};
    std::atomic<uint64_t> encoded_key_bytes{0};
    std::atomic<uint64_t> encoded_key_pts{0}, encoded_key_wall{0};
    void *live = nullptr, *cancel = nullptr;
    std::atomic<bool> closing{true}, failed{false};
    std::atomic<uint64_t> bytes{0}, dropped{0};
    char error[512]{};
    bool joined = false, need_key = true;
    int64_t origin = 0;
    uint32_t audio_duration = 0, video_duration = 0;
    ~Output() { if (ready) CloseHandle(ready); if (feedback_stop) CloseHandle(feedback_stop); }

    void trace_key(const char *event, const encoder_packet *p, const char *reason="") {
        if (!diagnostics || p->type!=OBS_ENCODER_VIDEO || !p->keyframe) return;
        key_trace.push({event,reason,encoded_keys.load(),
            joined && p->dts_usec>=origin?uint64_t(p->dts_usec-origin):0,
            wall_ms(),p->size,head.load()-tail.load(),queued_bytes.load()});
    }
    void drain_key_trace() {
        KeyEvent e;
        while(key_trace.pop(e))
            blog(LOG_INFO,"[PERI] PERI_TRACE {\"event\":\"%s\",\"wall_ms\":%llu,\"data\":{\"key_id\":%llu,\"pts_us\":%llu,\"bytes\":%llu,\"queued\":%llu,\"occupied\":%llu,\"reason\":\"%s\"}}",
                e.event,(unsigned long long)e.wall,(unsigned long long)e.id,(unsigned long long)e.pts,
                (unsigned long long)e.bytes,(unsigned long long)e.queued,(unsigned long long)e.occupied,e.reason);
        const auto lost=key_trace.lost.exchange(0);
        if(lost)blog(LOG_WARNING,"[PERI] Key trace overflow: lost=%llu",(unsigned long long)lost);
    }
    void trace_demand(const char *source) {
        if(diagnostics)blog(LOG_INFO,"[PERI] PERI_TRACE {\"event\":\"recovery_demand\",\"wall_ms\":%llu,\"data\":{\"source\":\"%s\",\"encoded_keys\":%llu}}",
            (unsigned long long)wall_ms(),source,(unsigned long long)encoded_keys.load());
    }

    void recover()
    {
        RecoveryGate gate;
        UplinkBudget rate(configured_bitrate);
        EncoderResponse response;
        Recovery request{};
        Recovery active{};
        bool warned = false;
        auto meter_at = GetTickCount64();
        uint64_t meter_bytes = 0;
        uint64_t meter_keys = 0, meter_key_bytes = 0;
        while (WaitForSingleObject(feedback_stop, 10) == WAIT_TIMEOUT) {
            if (closing.load()) break;
            drain_key_trace();
            for (unsigned i = 0; i < 8; ++i) {
                Recovery next{};
                const auto result = peri_live_feedback_poll(feedback, &next);
                if (result < 0) { feedback_failed = true; return; }
                if (result == 0) break;
                if (next.track != 1) continue;
                if (!gate.pending) {request = next;trace_demand("remote");}
                gate.request(encoded_keys.load());
                blog(LOG_INFO, "[PERI] Recovery received / 收到恢复请求: origin=%u id=%llu viewer=%llu after=%llu",
                    next.origin, (unsigned long long)next.id, (unsigned long long)next.viewer,
                    (unsigned long long)next.after_sequence);
            }
            if (local_recovery.exchange(false)) {
                if (!gate.pending) {request = {};trace_demand("local");}
                gate.request(encoded_keys.load());
            }
            const auto now = GetTickCount64();
            if (now - meter_at >= 1000) {
                Transport path{};
                if (diagnostics && peri_live_feedback_transport(feedback, &path)==1)
                    blog(LOG_INFO,"[PERI] QUIC path: cwnd=%llu rtt_us=%llu sent=%llu lost=%llu",
                        (unsigned long long)path.cwnd,(unsigned long long)path.rtt_us,
                        (unsigned long long)path.sent_packets,(unsigned long long)path.lost_packets);
                const auto total = encoded_video_bytes.load();
                const auto keys = encoded_keys.load(), key_bytes = encoded_key_bytes.load();
                const auto actual_kbps=(total-meter_bytes)*8/(now-meter_at);
                if(response.observe(now,rate.current,actual_kbps))
                    blog(LOG_WARNING,"[PERI] Encoder output limited=%d target=%u actual_kbps=%llu; next-session lower-resolution profile available / 编码输出能力判定",
                        response.limited,rate.current,(unsigned long long)actual_kbps);
                if (diagnostics) blog(LOG_INFO, "[PERI] Encoded video / 实际编码视频: kbps=%llu target=%u keys=%llu key_bytes=%llu",
                    (unsigned long long)((total - meter_bytes) * 8 / (now - meter_at)), rate.current,
                    (unsigned long long)(keys - meter_keys), (unsigned long long)(key_bytes - meter_key_bytes));
                meter_keys = keys; meter_key_bytes = key_bytes;
                meter_bytes = total; meter_at = now;
            }
            if (congestion.exchange(false)) rate.pressure(now);
            if (uplink_experiment && recovery_encoder && rate.can_raise(now)) {
                if(!gate.pending)trace_demand("bitrate_raise");
                gate.request(encoded_keys.load());
            }
            if (gate.observed(encoded_keys.load()))
                blog(LOG_INFO, "[PERI] Recovery key observed / 已观察编码关键帧: origin=%u id=%llu elapsed_ms=%llu pts_us=%llu wall_ms=%llu",
                    active.origin, (unsigned long long)active.id, (unsigned long long)(now - gate.last),
                    (unsigned long long)encoded_key_pts.load(), (unsigned long long)encoded_key_wall.load());
            if (gate.expired(now))
                blog(LOG_WARNING, "[PERI] Recovery key not observed within 1000 ms / 恢复关键帧尚未观察到");
            if (!gate.due(now)) continue;
            // Keep congestion pacing: the 333 ms-only trial amplified key bursts and freezes.
            // 弱网下取消额外节流会放大关键帧突发；保留已验证的拥塞节流。
            if (uplink_experiment && rate.pressured(now) && gate.issued && now - gate.last < 1000) continue;
            if (!recovery_encoder) {
                gate.pending = false;
                if (!warned) blog(LOG_WARNING, "[PERI] Active keyframe recovery unavailable for this encoder / 当前编码器不支持主动关键帧恢复");
                warned = true;
                continue;
            }
            // OBS 32.1.2 NVENC CBR update requests resetEncoder + forceIDR.
            // Keep settings unless local overload changes the bitrate; OBS applies on the encoder thread.
            // 无过载时保留设置；码率更新由编码线程应用，编码包回调不等待网络。
            gate.submitted(now, encoded_keys.load());
            active = request;
            const auto previous = rate.current;
            const auto bitrate = uplink_experiment ? rate.next(now) : rate.current;
            if (bitrate && bitrate != previous) {
                auto *settings = obs_data_create();
                obs_data_set_int(settings, "bitrate", bitrate);
                obs_encoder_update(recovery_encoder, settings);
                obs_data_release(settings);
                blog(LOG_INFO, "[PERI] Uplink bitrate / 上行码率: %u -> %u kbps", previous, bitrate);
            } else obs_encoder_update(recovery_encoder, nullptr);
            blog(LOG_INFO, "[PERI] Recovery submitted / 已提交编码恢复: origin=%u id=%llu viewer=%llu after=%llu wall_ms=%llu",
                request.origin, (unsigned long long)request.id, (unsigned long long)request.viewer,
                (unsigned long long)request.after_sequence, (unsigned long long)wall_ms());
        }
    }

    Bytes *reserve_slot(size_t expected = 0, bool key = false)
    {
        auto write = head.load(std::memory_order_relaxed);
        const auto slots=key_admission_experiment && !key?capacity-1:capacity;
        const auto bytes_limit=key_admission_experiment && !key?byte_capacity-key_reserve:byte_capacity;
        if (write - tail.load(std::memory_order_acquire) >= slots) {
            ++queue_pressure;
            rejection_reason="slots";return nullptr;
        }
        if (expected>bytes_limit || queued_bytes.load()>bytes_limit-expected) {
            ++queue_pressure;
            rejection_reason="bytes";return nullptr;
        }
        queued_key_ids[write % capacity]=0;
        auto &slot = ring[write % capacity];
        slot.clear();
        return &slot;
    }
    unsigned prioritize_key() {
        const auto pressure=queue_pressure.load();
        if(!key_admission_experiment || pressure==drained_pressure)return 0;
        const auto read=tail.load(std::memory_order_relaxed),end=head.load(std::memory_order_acquire);
        // Snapshot pressure before checking emptiness so a concurrent producer cannot lose a new signal.
        if(read==end){drained_pressure=pressure;return 0;}
        auto key=read;
        for(auto i=read+1;i!=end;++i)if(queued_key_ids[i%capacity])key=i;
        if(key==read)return 0;
        size_t removed=0;
        for(auto i=read;i!=key;++i){
            removed+=ring[i%capacity].size();
            if(diagnostics && queued_key_ids[i%capacity])
                blog(LOG_INFO,"[PERI] PERI_TRACE {\"event\":\"key_superseded\",\"wall_ms\":%llu,\"data\":{\"key_id\":%llu}}",
                    (unsigned long long)wall_ms(),(unsigned long long)queued_key_ids[i%capacity]);
        }
        // Only the consumer advances tail; the producer cannot overwrite the selected key.
        // 仅写线程释放旧槽，编码回调不碰tail，也不修改写线程正在使用的数据。
        queued_bytes-=removed;dropped+=key-read;
        tail.store(key,std::memory_order_release);
        if(diagnostics)blog(LOG_INFO,"[PERI] PERI_TRACE {\"event\":\"queue_advance_to_key\",\"wall_ms\":%llu,\"data\":{\"key_id\":%llu,\"frames\":%u,\"bytes\":%llu}}",
            (unsigned long long)wall_ms(),(unsigned long long)queued_key_ids[key%capacity],key-read,(unsigned long long)removed);
        return key-read;
    }
    void commit()
    {
        const auto write = head.load(std::memory_order_relaxed);
        queued_at[write % capacity] = GetTickCount64();
        queued_bytes += ring[write % capacity].size();
        head.fetch_add(1, std::memory_order_release);
        SetEvent(ready);
    }
    void write_packets()
    {
        bool catalog_sent = false;
        while (!closing.load()) {
            if(catalog_sent)prioritize_key();
            auto read = tail.load(std::memory_order_relaxed);
            if (read == head.load(std::memory_order_acquire)) {
                WaitForSingleObject(ready, INFINITE);
                continue;
            }
            auto &block = ring[read % capacity];
            // One deadline covers queue residence, QUIC write and transport receipt.
            // 同一期限覆盖队列驻留、QUIC 写入及传输确认；目录仍使用独立握手预算。
            const auto age = GetTickCount64() - queued_at[read % capacity];
            const auto remaining = age >= media_budget_ms ? 0u : uint32_t(media_budget_ms - age);
            const auto key_id=queued_key_ids[read % capacity];
            uint64_t pts=0;
            if(key_id && diagnostics){
                for(unsigned i=1;i<9;++i)pts=(pts<<8)|block[i];
                blog(LOG_INFO,"[PERI] PERI_TRACE {\"event\":\"key_send_begin\",\"wall_ms\":%llu,\"data\":{\"key_id\":%llu,\"pts_us\":%llu,\"queue_ms\":%llu,\"budget_ms\":%u}}",
                    (unsigned long long)wall_ms(),(unsigned long long)key_id,(unsigned long long)pts,(unsigned long long)age,remaining);
            }
            const auto result = !catalog_sent || !uplink_experiment
                ? (peri_live_send(live, block.data(), block.size(), error, sizeof error) ? 1 : -1)
                : peri_live_send_budget(live, block.data(), block.size(), remaining, error, sizeof error);
            if(key_id && diagnostics)
                blog(LOG_INFO,"[PERI] PERI_TRACE {\"event\":\"key_send_end\",\"wall_ms\":%llu,\"data\":{\"key_id\":%llu,\"pts_us\":%llu,\"result\":%d}}",
                    (unsigned long long)wall_ms(),(unsigned long long)key_id,(unsigned long long)pts,result);
            catalog_sent = true;
            if (result < 0) {
                failed.store(true);
                return;
            }
            if (result != 1) {
                ++dropped;
                if (result == 0) { local_recovery = true; congestion = true; }
                if (result == 3) congestion = true;
            } else bytes += block.size();
            queued_bytes -= block.size();
            tail.store(read + 1, std::memory_order_release);
        }
    }
    void stop_feedback()
    {
        closing = true;
        if (feedback_stop) SetEvent(feedback_stop);
        if (feedback_worker.joinable()) feedback_worker.join();
    }
    void shutdown()
    {
        stop_feedback();
        if (ready) SetEvent(ready);
        peri_live_cancel(cancel);
        cancel = nullptr;
        if (worker.joinable()) worker.join();
        drain_key_trace();
        peri_live_feedback_close(feedback);
        feedback = nullptr;
        peri_live_close(live);
        live = nullptr;
        if (recovery_encoder) obs_encoder_release(recovery_encoder);
        recovery_encoder = nullptr;
        if (auto count = dropped.exchange(0))
            blog(LOG_INFO, "[PERI] Backpressure drops / 拥塞丢弃包数: %llu", (unsigned long long)count);
        head = 0;
        tail = 0;
        queued_bytes = 0;
        queue_pressure=0;drained_pressure=0;
    }
    Bytes catalog()
    {
        auto *v = obs_output_get_video_encoder(obs);
        auto *a = obs_output_get_audio_encoder(obs, 0);
        auto *info = video_output_get_info(obs_encoder_video(v));
        auto rate = obs_encoder_get_sample_rate(a);
        auto channels = audio_output_get_channels(obs_encoder_audio(a));
        if (!info || !info->fps_num || !rate || channels < 1 || channels > 2)
            throw std::runtime_error("expected a mono/stereo program with valid timing");
        // Encoder video output already applies its frame-rate divisor.
        // 编码器 video_output 的帧率已包含分频，不能再次乘以 divisor。
        video_duration = uint32_t(1000000ULL * info->fps_den / info->fps_num);
        audio_duration = uint32_t(1000000ULL * obs_encoder_get_frame_size(a) / rate);
        if (!audio_duration || !video_duration) throw std::runtime_error("invalid encoder duration");
        std::string c = std::string("{\"tracks\":[{\"id\":1,\"kind\":\"video\",\"codec\":\"video/av1\",\"width\":")
            + std::to_string(obs_encoder_get_width(v)) + ",\"height\":" + std::to_string(obs_encoder_get_height(v))
            + ",\"init\":" + extra(v) + "},{\"id\":2,\"kind\":\"audio\",\"codec\":\"audio/opus\",\"sample_rate\":"
            + std::to_string(rate) + ",\"channels\":" + std::to_string(channels) + ",\"init\":" + extra(a) + "}]}";
        Bytes block;
        append_be(block, c.size(), 4);
        block.insert(block.end(), c.begin(), c.end());
        return block;
    }
};

obs_encoder_t *fallback_video_encoder()
{
    auto *settings = obs_data_create();
    obs_data_set_string(settings, "rate_control", "CBR");
    obs_data_set_int(settings, "bitrate", 20000);
    obs_data_set_int(settings, "keyint_sec", 1);
    obs_data_set_int(settings, "bf", 0);
    obs_data_set_bool(settings, "lookahead", false);
    obs_data_set_string(settings, "tune", "ll");
    obs_encoder_t *v = nullptr;
    for (const char *id : {"obs_nvenc_av1_tex", "ffmpeg_svt_av1", "ffmpeg_aom_av1"}) {
        if (obs_get_encoder_codec(id)) v = obs_video_encoder_create(id, "PERI AV1", settings, nullptr);
        if (v) break;
    }
    obs_data_release(settings);
    return v;
}

bool uses_codec(obs_encoder_t *encoder, const char *codec)
{
    return encoder && std::strcmp(obs_encoder_get_codec(encoder), codec) == 0;
}

bool configure_encoders(Output *o)
{
    // OBS owns the attached encoders. Preserve their profile settings when compatible.
    // OBS 持有已挂载的编码器，格式兼容时直接保留直播配置。
    auto *video = obs_output_get_video_encoder(o->obs);
    if (!uses_codec(video, "av1")) {
        video = fallback_video_encoder();
        if (!video) return false;
        obs_encoder_set_video(video, obs_get_video());
        obs_output_set_video_encoder(o->obs, video);
        obs_encoder_release(video);
        blog(LOG_INFO, "[PERI] Using fallback AV1 encoder at 20000 kbps / 使用默认 AV1 编码");
    } else {
        // PERI carries one video timestamp. Clone incompatible settings locally so
        // recording encoders and saved OBS profiles keep their original configuration.
        // PERI 仅携带一个视频时间戳；局部复制配置，保留录制与用户档案。
        auto *settings = obs_encoder_get_settings(video);
        const bool recovery_owned = obs_get_version() == ((32u << 24) | (1u << 16) | 2u) &&
            std::strcmp(obs_encoder_get_id(video), "obs_nvenc_av1_tex") == 0 &&
            std::strcmp(obs_data_get_string(settings, "rate_control"), "CBR") == 0;
        // Recovery updates must not force keyframes into a shared recording encoder.
        // 恢复热更新使用独立直播编码器，避免干扰共用原编码器的录制输出。
        if (obs_data_get_int(settings, "bf") > 0 || recovery_owned) {
            auto *copy = obs_data_create();
            obs_data_apply(copy, settings);
            obs_data_set_int(copy, "bf", 0);
            const char *profile = o->uplink_experiment && recovery_owned ? std::getenv("PERI_OBS_PROFILE") : nullptr;
            if (profile && *profile && std::strcmp(profile,"source")!=0) {
                const uint32_t cap=std::strcmp(profile,"720p30")==0?4000:
                    std::strcmp(profile,"360p30")==0?1000:0;
                if(!cap){obs_data_release(copy);obs_data_release(settings);return false;}
                if(obs_data_get_int(copy,"bitrate")>cap)obs_data_set_int(copy,"bitrate",cap);
            }
            if (recovery_owned && o->uplink_experiment &&
                GetEnvironmentVariableW(L"PERI_OBS_ENCODER_BUDGET", nullptr, 0)) {
                // Limit encoder reservoir to about one input frame at the configured rate.
                // 实验中限制编码缓冲预算，避免恢复关键帧消耗整秒码率额度。
                const auto *info = video_output_get_info(obs_encoder_video(video));
                auto bits = uint64_t(obs_data_get_int(copy,"bitrate")) * 1000 * info->fps_den / info->fps_num;
                bits = bits < 64000 ? 64000 : bits > 1000000 ? 1000000 : bits;
                obs_data_set_string(copy,"tune","ull");
                obs_data_set_bool(copy,"lookahead",false);
                obs_data_set_bool(copy,"adaptive_quantization",false);
                std::string opts=obs_data_get_string(copy,"opts");
                opts += " vbvBufferSize="+std::to_string(bits)+" vbvInitialDelay="+std::to_string(bits)+
                    " enableLookahead=0 enableAQ=0 enableTemporalAQ=0";
                obs_data_set_string(copy,"opts",opts.c_str());
            }
            auto *ordered = obs_video_encoder_create(obs_encoder_get_id(video), "PERI AV1 (live)", copy, nullptr);
            obs_data_release(copy);
            if (!ordered) { obs_data_release(settings); return false; }
            obs_encoder_set_video(ordered, obs_encoder_parent_video(video));
            if (!obs_encoder_set_frame_rate_divisor(ordered, obs_encoder_get_frame_rate_divisor(video))) {
                obs_encoder_release(ordered); obs_data_release(settings); return false;
            }
            if (obs_encoder_scaling_enabled(video)) {
                obs_encoder_set_scaled_size(ordered, obs_encoder_get_width(video), obs_encoder_get_height(video));
                obs_encoder_set_gpu_scale_type(ordered, obs_encoder_get_scale_type(video));
            }
            if (o->uplink_experiment && recovery_owned) {
                if(profile && *profile && std::strcmp(profile,"source")!=0) {
                    const uint32_t height=std::strcmp(profile,"720p30")==0?720:
                        std::strcmp(profile,"360p30")==0?360:0;
                    if(!height){obs_encoder_release(ordered);obs_data_release(settings);return false;}
                    const auto h=obs_encoder_get_height(video),w=obs_encoder_get_width(video);
                    if(h>height){obs_encoder_set_scaled_size(ordered,(uint64_t(w)*height/h)&~1u,height);
                        obs_encoder_set_gpu_scale_type(ordered,OBS_SCALE_BICUBIC);}
                    const auto *info=video_output_get_info(obs_encoder_parent_video(video));
                    const auto divisor=(info->fps_num+30*info->fps_den-1)/(30*info->fps_den);
                    if(!obs_encoder_set_frame_rate_divisor(ordered,divisor?divisor:1)){
                        obs_encoder_release(ordered);obs_data_release(settings);return false;
                    }
                }
            }
            obs_encoder_set_preferred_video_format(ordered, obs_encoder_get_preferred_video_format(video));
            obs_encoder_set_preferred_color_space(ordered, obs_encoder_get_preferred_color_space(video));
            obs_encoder_set_preferred_range(ordered, obs_encoder_get_preferred_range(video));
            obs_output_set_video_encoder(o->obs, ordered);
            obs_encoder_release(ordered);
            video = obs_output_get_video_encoder(o->obs);
        }
        obs_data_release(settings);
        blog(LOG_INFO, "[PERI] Using OBS streaming video encoder / 沿用 OBS 直播视频编码器: %s",
             obs_encoder_get_id(video));
    }
    auto *audio = obs_output_get_audio_encoder(o->obs, 0);
    if (uses_codec(audio, "opus")) return true;
    auto *settings = obs_data_create();
    obs_data_set_int(settings, "bitrate", 128);
    audio = obs_audio_encoder_create("ffmpeg_opus", "PERI Opus", settings, 0, nullptr);
    obs_data_release(settings);
    if (!audio) return false;
    obs_encoder_set_audio(audio, obs_get_audio());
    obs_output_set_audio_encoder(o->obs, audio, 0);
    obs_encoder_release(audio);
    return true;
}

bool start(void *data)
{
    auto *o = static_cast<Output *>(data);
    o->shutdown();
    try {
        if (!configure_encoders(o) || !obs_output_can_begin_data_capture(o->obs, 0) ||
            !obs_output_initialize_encoders(o->obs, 0))
            throw std::runtime_error("Cannot initialize OBS AV1 and Opus encoders.");
        auto *service = obs_output_get_service(o->obs);
        const char *key = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY) : nullptr;
        if (!key || !*key) throw std::runtime_error("请填写服务器签发的 PERI Token / Missing publish token");
        if (!o->ready || !o->feedback_stop) throw std::runtime_error("Cannot create PERI worker events");
        for (auto &slot : o->ring) slot.reserve(max_payload + 17);
        o->live = peri_live_open(key, &o->cancel, o->error, sizeof o->error);
        if (!o->live) throw std::runtime_error(o->error);
        o->feedback = peri_live_feedback_open(o->live);
        auto *encoder = obs_output_get_video_encoder(o->obs);
        auto *settings = obs_encoder_get_settings(encoder);
        const auto bitrate = obs_data_get_int(settings, "bitrate");
        o->configured_bitrate = bitrate > 0 && bitrate <= 1000000 ? uint32_t(bitrate) : 0;
        // Backend-specific capability; do not assume other OBS versions or rate modes.
        // 仅启用已核对的版本和后端，其他配置继续等待周期关键帧。
        if (obs_get_version() == ((32u << 24) | (1u << 16) | 2u) &&
            std::strcmp(obs_encoder_get_id(encoder), "obs_nvenc_av1_tex") == 0 &&
            std::strcmp(obs_data_get_string(settings, "rate_control"), "CBR") == 0 && o->configured_bitrate)
            o->recovery_encoder = obs_encoder_get_ref(encoder);
        obs_data_release(settings);
        o->need_key = true;
        o->failed = false;
        o->closing = false;
        o->joined = false;
        o->bytes = 0;
        o->encoded_keys = 0;
        o->encoded_video_bytes = 0;
        o->encoded_key_bytes = 0;
        o->local_recovery = false;
        o->congestion = false;
        o->feedback_failed = false;
        ResetEvent(o->feedback_stop);
        o->worker = std::thread(&Output::write_packets, o);
        o->feedback_worker = std::thread(&Output::recover, o);
        if (obs_output_begin_data_capture(o->obs, 0)) return true;
        throw std::runtime_error("Cannot capture OBS program.");
    } catch (const std::exception &e) {
        obs_output_set_last_error(o->obs, e.what());
        o->shutdown();
        return false;
    }
}
void packet(void *data, encoder_packet *p)
{
    auto *o = static_cast<Output *>(data);
    if (o->closing) return;
    try {
        if (o->failed) throw std::runtime_error(o->error);
        if (o->feedback_failed) throw std::runtime_error("Invalid PERI feedback / PERI 反馈校验失败");
        if (!p || !p->data || !p->size)
            throw std::runtime_error("Invalid encoded packet or publisher disconnected.");
        bool video = p->type == OBS_ENCODER_VIDEO;
        if (video) o->encoded_video_bytes += p->size;
        if (video && p->keyframe) {
            o->encoded_key_bytes += p->size;
            o->encoded_key_pts = o->joined && p->dts_usec >= o->origin ? uint64_t(p->dts_usec-o->origin) : 0;
            o->encoded_key_wall = wall_ms();
            ++o->encoded_keys;
            o->trace_key("key_encoded",p);
        }
        if (p->size > max_payload) {
            o->trace_key("key_rejected",p,"oversize");
            if (video) { o->need_key = true; o->local_recovery = true; o->congestion = true; }
            ++o->dropped;
            return;
        }
        if (video && p->pts != p->dts) throw std::runtime_error("PERI requires no frame reordering. Set B-frames to 0 in OBS. / 请在 OBS 直播输出将 B 帧设为 0。");
        if (!o->joined) {
            if (!video || !p->keyframe) return;
            auto *slot = o->reserve_slot();
            if (!slot) { o->trace_key("key_rejected",p,"catalog_queue");return; }
            auto catalog = o->catalog();
            slot->assign(catalog.begin(), catalog.end());
            o->commit();
            o->origin = p->dts_usec;
            o->joined = true;
        }
        if (p->dts_usec < o->origin) {o->trace_key("key_rejected",p,"before_origin");return;}
        if (video && o->need_key && !p->keyframe) { ++o->dropped; return; }
        auto *slot = o->reserve_slot(p->size + 17,video && p->keyframe);
        if (!slot) {
            o->trace_key("key_rejected",p,o->rejection_reason);
            if (video) { o->need_key = true; o->local_recovery = true; o->congestion = true; }
            ++o->dropped;
            return;
        }
        if (video) o->need_key = false;
        auto &block = *slot;
        block.push_back(video ? (p->keyframe ? 1 : 2) : 3);
        append_be(block, uint64_t(p->dts_usec - o->origin), 8);
        append_be(block, video ? o->video_duration : o->audio_duration, 4);
        append_be(block, p->size, 4);
        block.insert(block.end(), p->data, p->data + p->size);
        if(video && p->keyframe)o->queued_key_ids[o->head.load()%Output::capacity]=o->encoded_keys.load();
        o->trace_key("key_admitted",p);
        o->commit();
    } catch (const std::exception &e) {
        obs_output_set_last_error(o->obs, e.what());
        o->closing = true;
        SetEvent(o->ready);
        obs_output_signal_stop(o->obs, OBS_OUTPUT_ERROR);
    }
}
void stop(void *data, uint64_t)
{
    auto *o = static_cast<Output *>(data);
    // Stop encoder control before OBS tears down capture/encoder state.
    // 先停止编码控制，再让 OBS 结束采集和编码器状态，避免停播时并发热更新。
    o->stop_feedback();
    obs_output_end_data_capture(o->obs);
    o->shutdown();
}
const char *name(void *) { return "PERI"; }
} // namespace

bool obs_module_load(void)
{
    obs_output_info output{};
    output.id = "peri_native_output";
    output.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
    output.protocols = "PERI";
    output.encoded_video_codecs = "av1";
    output.encoded_audio_codecs = "opus";
    output.get_name = name;
    output.create = [](obs_data_t *, obs_output_t *o) -> void * { return new Output(o); };
    output.destroy = [](void *d) { auto *o = static_cast<Output *>(d); o->shutdown(); delete o; };
    output.get_total_bytes = [](void *d) { return static_cast<Output *>(d)->bytes.load(); };
    output.start = start;
    output.stop = stop;
    output.encoded_packet = packet;
    obs_register_output(&output);

    return true;
}

namespace {
HANDLE catalog_stop = nullptr;
std::thread catalog_thread;

// The OBS service updater can replace services.json after module initialization.
// OBS 内置更新器可能在插件初始化后替换清单；监测文件变动并重新合并服务。
void follow_catalog(std::string stock, std::string user)
{
    auto dir = std::filesystem::u8path(user).parent_path();
    HANDLE changed = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    HANDLE events[] = {catalog_stop, changed};
    while (WaitForSingleObject(catalog_stop, 0) != WAIT_OBJECT_0) {
        bool ok = peri_register_catalog(stock.c_str(), user.c_str());
        if (changed == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(catalog_stop, 3000) == WAIT_OBJECT_0) break;
            changed = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
            events[1] = changed;
            continue;
        }
        auto result = WaitForMultipleObjects(2, events, FALSE, ok ? INFINITE : 1000);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        if (result == WAIT_OBJECT_0 + 1 && !FindNextChangeNotification(changed)) break;
    }
    if (changed != INVALID_HANDLE_VALUE) FindCloseChangeNotification(changed);
}
}
void obs_module_post_load(void)
{
    auto *services = obs_get_module("rtmp-services");
    if (!services || catalog_thread.joinable()) return;
    char *stock = obs_find_module_file(services, "services.json");
    char *user = obs_module_get_config_path(services, "services.json");
    if (stock && user) {
        if (!peri_register_catalog(stock, user))
            blog(LOG_WARNING, "[PERI] Service catalog busy, retrying / 服务清单稍后重试");
        catalog_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (catalog_stop) catalog_thread = std::thread(follow_catalog, std::string(stock), std::string(user));
    }
    bfree(stock);
    bfree(user);
}
void obs_module_unload(void)
{
    if (catalog_stop) SetEvent(catalog_stop);
    if (catalog_thread.joinable()) catalog_thread.join();
    if (catalog_stop) CloseHandle(catalog_stop);
    catalog_stop = nullptr;
}
