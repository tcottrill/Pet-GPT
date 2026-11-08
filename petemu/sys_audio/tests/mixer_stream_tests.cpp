// Test the REAL reused mixer using an in-memory output device, without audio hardware.
#include "mixer.h"
#include "audio_backend.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
namespace {
std::mutex mutex;
std::condition_variable changed;
std::vector<int16_t> captured;
unsigned submissions=0, flushes=0;
class MemoryBackend final: public IAudioBackend {
    std::array<uint8_t,4096> pcm{};
    float volume=1;
public:
    bool Init(int,int fps) override { m_rate=48000; m_fps=fps; m_frames_per_update=48000/fps; return true; }
    void Shutdown() override {}
    uint8_t* GetNextBuffer() override { return pcm.data(); }
    bool Submit(uint8_t* data,uint32_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex);
        auto* first=reinterpret_cast<int16_t*>(data);
        captured.assign(first,first+bytes/2); ++submissions; changed.notify_one(); return true;
    }
    void FlushStream() override { std::lock_guard<std::mutex> lock(mutex); captured.clear(); ++flushes; }
    void SetMasterVolume(float x) override { volume=x; }
    float GetMasterVolume() const override { return volume; }
    uint32_t OutputChannelCount() const override { return 2; }
    uint32_t OutputChannelMask() const override { return 3; }
    VoiceHandle* VoiceCreate(const WaveFormat&) override { return nullptr; }
    void VoiceDestroy(VoiceHandle*) override {}
    bool VoiceSubmit(VoiceHandle*,const uint8_t*,uint32_t,bool) override { return false; }
    bool VoiceStart(VoiceHandle*) override { return false; }
    void VoiceStop(VoiceHandle*) override {}
    void VoiceFlush(VoiceHandle*) override {}
    void VoiceExitLoop(VoiceHandle*) override {}
    void VoiceSetVolume(VoiceHandle*,float) override {}
    void VoiceSetFrequencyRatio(VoiceHandle*,float) override {}
    uint32_t VoiceBuffersQueued(VoiceHandle*) override { return 0; }
    uint32_t VoiceInputChannels(VoiceHandle*) override { return 1; }
    bool VoiceSetOutputMatrix(VoiceHandle*,uint32_t,uint32_t,const float*) override { return false; }
};
std::vector<int16_t> mix() {
    std::unique_lock<std::mutex> lock(mutex); const auto before=submissions;
    lock.unlock(); mixer_update(); lock.lock();
    CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return submissions>before;}));
    return captured;
}
}
std::unique_ptr<IAudioBackend> create_audio_backend() { return std::make_unique<MemoryBackend>(); }
int main() {
    CHECK(mixer_init(44100,60)); CHECK(mixer_get_output_rate()==48000);
    stream_start(1,1,16,60,false); stream_set_native_rate(1,44100);
    // A 735-element source must remain safe even when the output device grants 48 kHz.
    std::vector<int16_t> source(735,1000);
    stream_update(1,source.data()); auto out=mix(); CHECK(out.size()==1600);
    CHECK(out[0]==1000 && out[1]==1000 && out.back()==1000);
    mixer_flush(); CHECK(flushes>0); out=mix();
    for(auto s:out) CHECK(s==0);
    stream_update(1,source.data()); pause_audio(); out=mix();
    for(auto s:out) CHECK(s==0);
    restore_audio(); out=mix(); for(auto s:out) CHECK(s==0);
    stream_update(1,source.data()); out=mix(); CHECK(out[0]==1000);
    stream_stop(1,1); mixer_end(); mixer_end();
    std::puts("Mixer native-rate/flush/pause tests passed");
}
