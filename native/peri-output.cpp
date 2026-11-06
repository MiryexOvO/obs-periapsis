// SPDX-License-Identifier: AGPL-3.0-only
// Native OBS encoded output for the PERI publisher. / PERI 发布端的 OBS 原生编码输出。
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) { return "PERI AV1/Opus output"; }

namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t max_payload = 1024 * 1024 - 68;
std::mutex binding_lock;
HANDLE pending = nullptr;

// A one-use handle is never stored in an OBS profile. / 管道句柄只使用一次，不写入 OBS 配置。
HANDLE take_binding()
{
    std::lock_guard guard(binding_lock);
    HANDLE pipe = pending;
    pending = nullptr;
    return pipe;
}

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
    HANDLE pipe = nullptr;
    std::thread worker;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Bytes> queue;
    size_t queued = 0;
    std::atomic<bool> closing{true}, failed{false};
    std::atomic<uint64_t> bytes{0};
    bool joined = false;
    int64_t origin = 0;
    uint32_t audio_duration = 0, video_duration = 0;

    bool enqueue(Bytes block)
    {
        std::lock_guard guard(lock);
        if (closing || failed || queue.size() >= 32 || queued + block.size() > 4 * 1024 * 1024)
            return false;
        queued += block.size();
        queue.push_back(std::move(block));
        wake.notify_one();
        return true;
    }
    void write_packets()
    {
        while (true) {
            Bytes next;
            {
                std::unique_lock guard(lock);
                wake.wait(guard, [&] { return closing || !queue.empty(); });
                if (closing) return;
                next = std::move(queue.front());
                queue.pop_front();
                queued -= next.size();
            }
            DWORD count = 0;
            if (!WriteFile(pipe, next.data(), static_cast<DWORD>(next.size()), &count, nullptr) ||
                count != next.size()) {
                failed = true;
                return;
            }
            bytes += count;
        }
    }
    void shutdown()
    {
        {
            std::lock_guard guard(lock);
            closing = true;
            queue.clear();
            queued = 0;
        }
        wake.notify_all();
        if (worker.joinable()) {
            // Cancel even if the writer entered WriteFile after our stop signal.
            // 即使写线程稍晚进入 WriteFile，也能取消并结束。
            HANDLE thread = worker.native_handle();
            while (WaitForSingleObject(thread, 10) == WAIT_TIMEOUT) CancelSynchronousIo(thread);
            worker.join();
        }
        if (pipe) CloseHandle(pipe);
        pipe = nullptr;
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

bool configure_encoders(Output *o)
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
    auto *audio_settings = obs_data_create();
    obs_data_set_int(audio_settings, "bitrate", 128);
    auto *a = obs_audio_encoder_create("ffmpeg_opus", "PERI Opus", audio_settings, 0, nullptr);
    obs_data_release(audio_settings);
    if (v && a) {
        obs_encoder_set_video(v, obs_get_video());
        obs_encoder_set_audio(a, obs_get_audio());
        obs_output_set_video_encoder(o->obs, v);
        obs_output_set_audio_encoder(o->obs, a, 0);
    }
    const bool ok = v && a;
    obs_encoder_release(v);
    obs_encoder_release(a);
    return ok;
}

bool start(void *data)
{
    auto *o = static_cast<Output *>(data);
    o->shutdown();
    o->pipe = take_binding();
    if (!o->pipe) { obs_output_set_last_error(o->obs, "Start PERI from its publisher settings."); return false; }
    try {
        if (!configure_encoders(o) || !obs_output_can_begin_data_capture(o->obs, 0) ||
            !obs_output_initialize_encoders(o->obs, 0))
            throw std::runtime_error("Cannot initialize OBS AV1 and Opus encoders.");
        o->failed = false;
        o->closing = false;
        o->joined = false;
        o->bytes = 0;
        o->worker = std::thread(&Output::write_packets, o);
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
        if (!p || !p->data || !p->size || p->size > max_payload || o->failed)
            throw std::runtime_error("Invalid encoded packet or publisher disconnected.");
        bool video = p->type == OBS_ENCODER_VIDEO;
        if (video && p->pts != p->dts) throw std::runtime_error("PERI requires video without frame reordering.");
        if (!o->joined) {
            if (!video || !p->keyframe) return;
            if (!o->enqueue(o->catalog())) throw std::runtime_error("Cannot submit catalog.");
            o->origin = p->dts_usec;
            o->joined = true;
        }
        if (p->dts_usec < o->origin) return;
        Bytes block;
        block.reserve(17 + p->size);
        block.push_back(video ? (p->keyframe ? 1 : 2) : 3);
        append_be(block, uint64_t(p->dts_usec - o->origin), 8);
        append_be(block, video ? o->video_duration : o->audio_duration, 4);
        append_be(block, p->size, 4);
        block.insert(block.end(), p->data, p->data + p->size);
        if (!o->enqueue(std::move(block))) throw std::runtime_error("PERI publisher queue is full.");
    } catch (const std::exception &e) {
        obs_output_set_last_error(o->obs, e.what());
        o->closing = true;
        o->wake.notify_all();
        obs_output_signal_stop(o->obs, OBS_OUTPUT_ERROR);
    }
}
void stop(void *data, uint64_t)
{
    auto *o = static_cast<Output *>(data);
    obs_output_end_data_capture(o->obs);
    o->shutdown();
}
const char *name(void *) { return "PERI"; }
} // namespace

extern "C" __declspec(dllexport) bool peri_bind_sink(HANDLE handle)
{
    std::lock_guard guard(binding_lock);
    if (pending) CloseHandle(pending);
    pending = nullptr;
    return !handle || DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &pending, 0, FALSE, DUPLICATE_SAME_ACCESS);
}
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

    obs_service_info service{};
    service.id = "peri_native_service";
    service.get_name = name;
    service.create = [](obs_data_t *, obs_service_t *s) -> void * { return s; };
    service.destroy = [](void *) {};
    service.get_protocol = [](void *) { return "PERI"; };
    service.get_output_type = [](void *) { return "peri_native_output"; };
    service.get_connect_info = [](void *, uint32_t type) { return type == OBS_SERVICE_CONNECT_INFO_SERVER_URL ? "peri://relay" : ""; };
    service.can_try_to_connect = [](void *) { std::lock_guard guard(binding_lock); return pending != nullptr; };
    obs_register_service(&service);
    return true;
}
