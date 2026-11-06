// SPDX-License-Identifier: GPL-2.0-or-later
// Native OBS encoded output for the PERI publisher. / PERI 发布端的 OBS 原生编码输出。
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <atomic>
#include <array>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) { return "PERI AV1/Opus output"; }

namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t max_payload = 1024 * 1024 - 68;
extern "C" void *peri_live_open(const char *, void **, char *, size_t);
extern "C" bool peri_live_send(void *, const uint8_t *, size_t, char *, size_t);
extern "C" void peri_live_cancel(void *);
extern "C" void peri_live_close(void *);
extern "C" bool peri_register_catalog(const char *, const char *);

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
    // OBS interleaves encoded packets on one producer; only the writer advances tail.
    // OBS 串行交付交织编码包，只有网络线程推进读指针。
    static constexpr uint32_t capacity = 32;
    std::array<Bytes, capacity> ring;
    std::atomic<uint32_t> head{0}, tail{0};
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;
    void *live = nullptr, *cancel = nullptr;
    std::atomic<bool> closing{true}, failed{false};
    std::atomic<uint64_t> bytes{0}, dropped{0};
    char error[512]{};
    bool joined = false, need_key = true;
    int64_t origin = 0;
    uint32_t audio_duration = 0, video_duration = 0;
    ~Output() { if (ready) CloseHandle(ready); }

    Bytes *reserve_slot()
    {
        auto write = head.load(std::memory_order_relaxed);
        if (write - tail.load(std::memory_order_acquire) >= capacity) return nullptr;
        auto &slot = ring[write % capacity];
        slot.clear();
        return &slot;
    }
    void commit()
    {
        head.fetch_add(1, std::memory_order_release);
        SetEvent(ready);
    }
    void write_packets()
    {
        while (!closing.load()) {
            auto read = tail.load(std::memory_order_relaxed);
            if (read == head.load(std::memory_order_acquire)) {
                WaitForSingleObject(ready, INFINITE);
                continue;
            }
            auto &block = ring[read % capacity];
            if (!peri_live_send(live, block.data(), block.size(), error, sizeof error)) {
                failed.store(true);
                return;
            }
            bytes += block.size();
            tail.store(read + 1, std::memory_order_release);
        }
    }
    void shutdown()
    {
        closing = true;
        if (ready) SetEvent(ready);
        peri_live_cancel(cancel);
        cancel = nullptr;
        if (worker.joinable()) worker.join();
        peri_live_close(live);
        live = nullptr;
        if (auto count = dropped.exchange(0))
            blog(LOG_INFO, "[PERI] Backpressure drops / 拥塞丢弃包数: %llu", (unsigned long long)count);
        head = 0;
        tail = 0;
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
        if (obs_data_get_int(settings, "bf") > 0) {
            auto *copy = obs_data_create();
            obs_data_apply(copy, settings);
            obs_data_set_int(copy, "bf", 0);
            auto *ordered = obs_video_encoder_create(obs_encoder_get_id(video), "PERI AV1 (no B frames)", copy, nullptr);
            obs_data_release(copy);
            if (!ordered) { obs_data_release(settings); return false; }
            obs_encoder_set_video(ordered, obs_encoder_video(video));
            if (obs_encoder_scaling_enabled(video)) {
                obs_encoder_set_scaled_size(ordered, obs_encoder_get_width(video), obs_encoder_get_height(video));
                obs_encoder_set_gpu_scale_type(ordered, obs_encoder_get_scale_type(video));
            }
            obs_encoder_set_preferred_video_format(ordered, obs_encoder_get_preferred_video_format(video));
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
        if (!o->ready) throw std::runtime_error("Cannot create PERI writer event");
        for (auto &slot : o->ring) slot.reserve(max_payload + 17);
        o->live = peri_live_open(key, &o->cancel, o->error, sizeof o->error);
        if (!o->live) throw std::runtime_error(o->error);
        o->need_key = true;
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
        if (o->failed) throw std::runtime_error(o->error);
        if (!p || !p->data || !p->size)
            throw std::runtime_error("Invalid encoded packet or publisher disconnected.");
        bool video = p->type == OBS_ENCODER_VIDEO;
        if (p->size > max_payload) {
            if (video) o->need_key = true;
            ++o->dropped;
            return;
        }
        if (video && p->pts != p->dts) throw std::runtime_error("PERI requires no frame reordering. Set B-frames to 0 in OBS. / 请在 OBS 直播输出将 B 帧设为 0。");
        if (!o->joined) {
            if (!video || !p->keyframe) return;
            auto *slot = o->reserve_slot();
            if (!slot) return;
            auto catalog = o->catalog();
            slot->assign(catalog.begin(), catalog.end());
            o->commit();
            o->origin = p->dts_usec;
            o->joined = true;
        }
        if (p->dts_usec < o->origin) return;
        if (video && o->need_key && !p->keyframe) { ++o->dropped; return; }
        auto *slot = o->reserve_slot();
        if (!slot) {
            if (video) o->need_key = true;
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
