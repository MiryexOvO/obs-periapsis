// Stall the transport worker while feeding the actual OBS output callback.
// 阻塞传输线程，同时持续调用真实的 OBS 编码包回调。
#include "../native/peri-output.cpp"
#include <cstdio>
static HANDLE sending = CreateEventW(nullptr, TRUE, FALSE, nullptr);
static HANDLE release_writer = CreateEventW(nullptr, TRUE, FALSE, nullptr);
extern "C" void *peri_live_open(const char *, void **, char *, size_t) { return nullptr; }
extern "C" bool peri_register_catalog(const char *, const char *) { return true; }
extern "C" void peri_live_close(void *) {}
extern "C" void peri_live_cancel(void *) { SetEvent(release_writer); }
extern "C" bool peri_live_send(void *, const uint8_t *, size_t, char *, size_t)
{
    SetEvent(sending);
    return WaitForSingleObject(release_writer, 5000) == WAIT_OBJECT_0;
}
int main()
{
    Output output(nullptr);
    for (auto &slot : output.ring) slot.reserve(128);
    output.closing = false;
    output.joined = true;
    uint8_t payload[32]{};
    encoder_packet p{};
    p.type = OBS_ENCODER_VIDEO;
    p.data = payload;
    p.size = sizeof payload;
    p.keyframe = true;
    output.worker = std::thread(&Output::write_packets, &output);
    packet(&output, &p);
    if (WaitForSingleObject(sending, 1000) != WAIT_OBJECT_0) return 1;
    p.keyframe = false;
    const auto before = GetTickCount64();
    for (int i=0; i<100000; ++i) packet(&output, &p);
    const auto elapsed = GetTickCount64() - before;
    if (output.head != Output::capacity || output.dropped != 100001-Output::capacity ||
        elapsed > 1000 || !output.need_key) return 2;
    SetEvent(release_writer);
    for (int i=0; i<1000 && output.head!=output.tail; ++i) Sleep(1);
    if (output.head!=output.tail) return 3;
    auto head=output.head.load();
    packet(&output,&p);
    if (output.head!=head) return 4;
    p.keyframe=true;
    packet(&output,&p);
    p.keyframe=false;
    packet(&output,&p);
    if (output.head!=head+2 || output.need_key) return 5;
    output.shutdown();
    printf("100000 callbacks, stalled transport: %llu ms; capacity, drop and keyframe recovery OK\n",elapsed);
    CloseHandle(release_writer);
    CloseHandle(sending);
}
