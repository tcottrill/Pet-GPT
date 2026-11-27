#include "../wx/frame_schedule.h"
#include "../wx/host_options.h"
#include "../petsrc/frame_pacing.h"
#include "../petsrc/phosphor_profile.h"
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
    require(options.disk == "image.d64" && options.model == 0, "disk path captured; no model given");
    options = parse_host_options({"pet-gpt", "--disk=My Disk.D64", "-8032"});
    require(options.disk == "My Disk.D64" && options.model == 8, "--disk=value keeps case and spaces; -8032");
    options = parse_host_options({"pet-gpt", "-DISK", "a.d64", "--4032"});
    require(options.disk == "a.d64" && options.model == 12, "upper-case -DISK; double-dash model flag");
    options = parse_host_options({"pet-gpt", "-basic4", "-PET2001"});
    require(options.model == 1, "last model flag wins, any case");
    options = parse_host_options({"pet-gpt", "-8032"});
    require(options.video50 == -1, "no -50hz/-60hz given leaves video50 at -1");
    options = parse_host_options({"pet-gpt", "--50HZ", "-4032"});
    require(options.video50 == 1 && options.model == 12, "--50HZ any case, double dash");
    options = parse_host_options({"pet-gpt", "-50hz", "-60Hz"});
    require(options.video50 == 0, "last of -50hz/-60hz wins");
    options = parse_host_options({"pet-gpt", "-8296"});
    require(options.model == 96 && options.video50 == -1, "-8296 selects model 96 and leaves the 50 Hz choice alone");
    options = parse_host_options({"pet-gpt", "--8296", "-50hz"});
    require(options.model == 96 && options.video50 == 1, "--8296 with -50hz is model 96 at 50 Hz");
    options = parse_host_options({"pet-gpt", "-8296D"});
    require(options.model == 96 && options.video50 == 1, "-8296d (any case) is model 96 at 50 Hz");
    options = parse_host_options({"pet-gpt", "-8296d", "-60hz"});
    require(options.model == 96 && options.video50 == 0, "-60hz after -8296d wins");
    options = parse_host_options({"pet-gpt", "-8296", "-8032"});
    require(options.model == 8, "last model flag wins");
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
    require(pacing_frame_cycles(0) == kNominalFrameCycles, "no CRTC: nominal pacing");
    require(pacing_frame_cycles(1) == kNominalFrameCycles, "unprogrammed CRTC nonsense: nominal pacing");
    require(pacing_frame_cycles(16650) == kNominalFrameCycles && pacing_frame_cycles(16640) == kNominalFrameCycles, "60 Hz machines keep nominal pacing");
    require(pacing_frame_cycles(20250) == 20250, "49.4 Hz frame is followed");
    require(pacing_frame_cycles(30000) == kNominalFrameCycles && pacing_frame_cycles(12000) == kNominalFrameCycles, "out-of-range frames fall back to nominal");
    // 20.25 ms period: the first frame is due at t=0, then at 20.25, 40.5, ... ms. Frame k is
    // due at k*20.25 ms; ticks run at 0,4,...,996 ms, so the frames due are those with
    // k*20.25 <= 996, i.e. k <= 49.18, k = 0..49 -> 50 frames.
    schedule.Reset(start);
    schedule.SetPeriod(std::chrono::microseconds(20250));
    frames = 0;
    for (int ms = 0; ms < 1000; ms += 4) frames += schedule.Due(start + std::chrono::milliseconds(ms));
    require(frames == 50, "4 ms timer produces 50 frames in 1 s at a 20.25 ms period");
    schedule.SetPeriod(FrameSchedule::period);
    schedule.Reset(start);
    frames = 0;
    for (int ms = 0; ms < 1000; ms += 4) frames += schedule.Due(start + std::chrono::milliseconds(ms));
    require(frames == 60, "default period gives 60 frames again");
    PhosphorDefaults tube = phosphor_defaults(96);
    require(tube.persistence == 1.0f && tube.decayMs == 1300.0f && tube.tail == 0.5f && tube.tailMs == 3000.0f, "8296 row is the fitted long-glow tube");
    tube = phosphor_defaults(1);
    require(tube.persistence == 0.10f && tube.decayMs == 30.0f && tube.tail == 0.0f && tube.tailMs == 4000.0f, "2001 row");
    for (int id : {2, 4}) {
        tube = phosphor_defaults(id);
        require(tube.persistence == 0.12f && tube.decayMs == 60.0f && tube.tail == 0.0f && tube.tailMs == 4000.0f, "2001N and 4000 9-inch share a row");
    }
    for (int id : {8, 12, 0, 3, 99, -1}) {
        tube = phosphor_defaults(id);
        require(tube.persistence == 0.15f && tube.decayMs == 100.0f && tube.tail == 0.0f && tube.tailMs == 4000.0f, "8032, 4000 12-inch and unknown ids use the 8032 row");
    }
    for (int id : {1, 2, 4, 8, 12, 96, 99}) {
        tube = phosphor_defaults(id);
        require(tube.bloomSpread == 1.0f && tube.bloomTail == 0.6f && tube.bloomCore == 0.0f, "bloom spread, tail and core are the same on every machine");
        require(tube.bloom == (id == 96 ? 0.35f : 0.15f), "bloom amount: 8296 stronger, all others 0.15");
    }
    require(phosphor_key("mono_bloom", 96) == "mono_bloom_m96", "bloom key gets the model suffix");
    require(phosphor_key("mono_persistence", 96) == "mono_persistence_m96", "persistence key gets the model suffix");
    require(phosphor_key("mono_persist_tail_ms", 8) == "mono_persist_tail_ms_m8", "suffix is _m<id> after the full knob name");
    std::puts("frame schedule tests passed");
}
