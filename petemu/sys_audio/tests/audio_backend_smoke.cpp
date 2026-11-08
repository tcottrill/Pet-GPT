// Exercise the reused AAE platform backend, including PET's fixed-rate stream.
#include "mixer.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
int main() {
    if (!mixer_init(44100,60)) { std::puts("No audio device; hardware smoke skipped"); return 77; }
    stream_start(1,1,16,60,false);
    stream_set_native_rate(1,44100);
    std::array<int16_t,735> pcm{};
    for(int i=0;i<735;++i) pcm[i]=(i%80<40)?1000:-1000;
    for(int frame=0;frame<10;++frame) {
        stream_update(1,pcm.data()); mixer_update();
        std::this_thread::sleep_for(std::chrono::milliseconds(17));
    }
    pause_audio(); mixer_flush(); restore_audio();
    stream_update(1,pcm.data()); mixer_update();
    stream_stop(1,1); mixer_end(); mixer_end();
    std::puts("Native audio backend stream/pause/shutdown smoke passed");
}
