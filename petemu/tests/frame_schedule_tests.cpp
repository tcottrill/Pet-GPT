#include "../wx/frame_schedule.h"
#include "../wx/host_options.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    auto options = parse_host_options({"pet-gpt", "-SCALE", "99", "-ROM", "First.PRG", "second.prg", "-MONITOR", "oops"});
    require(options.scale == 0 && options.monitor == 1 && options.rom == "First.PRG", "legacy CLI case, invalid geometry, explicit program precedence");
    options = parse_host_options({"pet-gpt", "/tmp/first.prg", "second.prg", "-monitorid", "DISPLAY", "-monitor", "2"});
    require(options.rom == "/tmp/first.prg" && options.monitorId.empty() && options.monitor == 2, "absolute Linux path and explicit monitor override");
    options = parse_host_options({"pet-gpt", "-disk", "image.d64", "-scale", "FIT", "-?"});
    require(options.rom.empty() && options.scale == 0 && options.help, "disk argument is not also loaded as a program; legacy help");
    require(host_effective_scale(3, 700, 500, 320, 240) == 2, "large requested scale falls back to lower preset");
    require(host_effective_scale(1, 300, 220, 320, 240) == 0, "tiny monitor falls back to Fit");
    FrameSchedule schedule;
    const auto start = FrameSchedule::Clock::time_point{};
    schedule.Reset(start);
    require(schedule.Due(start) == 1, "first frame is immediately due");
    require(schedule.Due(start) == 0, "paint/repeated tick cannot step twice");
    require(schedule.Due(start + FrameSchedule::period - std::chrono::nanoseconds(1)) == 0, "early timer must not speed emulation");
    require(schedule.Due(start + FrameSchedule::period * 3) == 3, "small scheduling delay catches up exactly");
    bool dropped = false;
    require(schedule.Due(start + std::chrono::seconds(5), &dropped) == 1 && dropped, "long stall discards debt and flags audio flush");
    schedule.Reset(start);
    require(schedule.Due(start + FrameSchedule::period * 4) == 4, "catch-up batch never exceeds four");
    schedule.Reset(start + std::chrono::seconds(10));
    require(schedule.Due(start + std::chrono::seconds(10)) == 1, "modal resume starts fresh");
    schedule.Reset(start);
    int frames = 0;
    for (int ms = 0; ms < 1000; ms += 4) frames += schedule.Due(start + std::chrono::milliseconds(ms));
    require(frames == 60, "4 ms timer produces 60 frames per second");
    std::puts("frame schedule tests passed");
}
