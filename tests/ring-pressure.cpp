// Stall the transport worker while feeding the actual OBS output callback.
// 阻塞传输线程，同时持续调用真实的 OBS 编码包回调。
#include "../native/peri-output.cpp"
#include <cstdio>
static HANDLE sending = CreateEventW(nullptr, TRUE, FALSE, nullptr);
static HANDLE release_writer = CreateEventW(nullptr, TRUE, FALSE, nullptr);
static std::atomic<unsigned> expired{0};
extern "C" void *peri_live_open(const char *, void **, char *, size_t) { return nullptr; }
extern "C" bool peri_register_catalog(const char *, const char *) { return true; }
extern "C" void peri_live_close(void *) {}
extern "C" void *peri_live_feedback_open(const void *) { return nullptr; }
extern "C" int32_t peri_live_feedback_poll(const void *, Recovery *) { return 0; }
extern "C" int32_t peri_live_feedback_transport(const void *, Transport *) { return 0; }
extern "C" void peri_live_feedback_close(void *) {}
extern "C" void peri_live_cancel(void *) { SetEvent(release_writer); }
extern "C" bool peri_live_send(void *, const uint8_t *, size_t, char *, size_t)
{
    SetEvent(sending);
    return WaitForSingleObject(release_writer, 5000) == WAIT_OBJECT_0;
}
extern "C" int32_t peri_live_send_budget(void *, const uint8_t *, size_t, uint32_t remaining, char *, size_t)
{
    if (!remaining) { ++expired; return 0; }
    if (remaining > Output::media_budget_ms) std::abort();
    return 1;
}
int main()
{
    SetEnvironmentVariableW(L"PERI_OBS_UPLINK_EXPERIMENT", L"1");
    SetEnvironmentVariableW(L"PERI_OBS_KEY_ADMISSION", L"1");
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
    if (output.head != Output::capacity-1 || output.dropped != 100001-(Output::capacity-1) ||
        elapsed > 1000 || !output.need_key || !output.local_recovery) return 2;
    // Writer is blocked on slot zero; age queued media without sleeping.
    // 写线程仍阻塞在首槽，直接推进其余排队媒体的年龄以确定性验证过期。
    for (unsigned i=1; i<Output::capacity-1; ++i)
        output.queued_at[i] = GetTickCount64() - Output::media_budget_ms - 50;
    SetEvent(release_writer);
    for (int i=0; i<1000 && output.head!=output.tail; ++i) Sleep(1);
    if (output.head!=output.tail) return 3;
    if (expired != Output::capacity-2 || output.queued_bytes != 0 || output.failed) return 6;
    if (output.reserve_slot(Output::byte_capacity+1)) return 7;
    auto head=output.head.load();
    packet(&output,&p);
    if (output.head!=head) return 4;
    p.keyframe=true;
    packet(&output,&p);
    p.keyframe=false;
    packet(&output,&p);
    if (output.head!=head+2 || output.need_key) return 5;
    output.shutdown();
    Output protected_key(nullptr);
    for(unsigned i=0;i<31;++i){
        auto *slot=protected_key.reserve_slot(1);if(!slot)return 10;
        slot->push_back(2);protected_key.commit();
    }
    if(protected_key.reserve_slot(1))return 11;
    auto *key=protected_key.reserve_slot(Output::key_reserve,true);if(!key)return 12;
    key->push_back(1);protected_key.queued_key_ids[31]=99;protected_key.commit();
    if(protected_key.prioritize_key()!=31 || protected_key.tail!=31 ||
        protected_key.queued_bytes!=1 || protected_key.ring[31][0]!=1)return 13;
    Output byte_bound(nullptr);
    if(byte_bound.reserve_slot(Output::byte_capacity-Output::key_reserve+1))return 14;
    if(!byte_bound.reserve_slot(Output::key_reserve,true))return 15;
    SetEnvironmentVariableW(L"PERI_OBS_KEY_ADMISSION", nullptr);
    Output existing_experiment(nullptr);
    for(unsigned i=0;i<32;++i){
        auto *slot=existing_experiment.reserve_slot(1);if(!slot)return 17;
        slot->push_back(2);existing_experiment.commit();
    }
    if(existing_experiment.reserve_slot(1))return 18;
    existing_experiment.queued_key_ids[31]=2;
    if(existing_experiment.prioritize_key()!=0 || existing_experiment.tail!=0)return 19;
    SetEnvironmentVariableW(L"PERI_OBS_UPLINK_EXPERIMENT", nullptr);
    Output baseline(nullptr);
    for(unsigned i=0;i<32;++i){
        auto *slot=baseline.reserve_slot(1);if(!slot)return 8;
        slot->push_back(0);baseline.commit();
    }
    if(baseline.reserve_slot(1) || baseline.queued_bytes!=32)return 9;
    baseline.queued_key_ids[31]=1;
    if(baseline.prioritize_key()!=0 || baseline.tail!=0)return 16;
    printf("100000 callbacks, stalled transport: %llu ms; age/byte bounds, drops and keyframe recovery OK\n",elapsed);
    CloseHandle(release_writer);
    CloseHandle(sending);
}
