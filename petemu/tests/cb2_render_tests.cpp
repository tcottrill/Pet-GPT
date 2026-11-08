// Standalone test of the CB2 reconstruction (cb2_render.h).
// Verifies the oversampled low-pass removes ultrasonic content (e.g. SI's 31 kHz
// idle toggle) while preserving real notes.
#define _USE_MATH_DEFINES
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include "via6522.h"     // CB2Edge
#include "cb2_render.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Square wave: CB2 toggles every halfPeriod cycles for totalCycles.
static std::vector<CB2Edge> makeSquare(uint32_t halfPeriod, uint32_t totalCycles)
{
    std::vector<CB2Edge> e; bool lv = false;
    for (uint32_t c = halfPeriod; c < totalCycles; c += halfPeriod) { lv = !lv; e.push_back(CB2Edge(c, lv)); }
    return e;
}
static double rms(const int16_t* x, int N) { double s = 0; for (int i = 0; i < N; i++) s += double(x[i]) * x[i]; return std::sqrt(s / N); }
static double goertzel(const int16_t* x, int N, double f, double fs)
{
    double w = 2 * M_PI * f / fs, coeff = 2 * std::cos(w), s0 = 0, s1 = 0, s2 = 0;
    for (int n = 0; n < N; n++) { s0 = x[n] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    double p = s1 * s1 + s2 * s2 - coeff * s1 * s2; return std::sqrt(p > 0 ? p : 0) / (N / 2.0);
}

int main()
{
    const double fs = 44100, cpu = 1e6;
    const int N = 22050;                       // 0.5 s
    const double totalCycles = N * cpu / fs;   // CB2 cycles spanned
    std::vector<int16_t> out(N);
    int fails = 0;

    // 1) 31.25 kHz idle toggle (gap 16) = the Space Invaders whine source.
    {
        auto e = makeSquare(16, (uint32_t)totalCycles);
        Cb2Render r; r.configure(fs, cpu, 8000);
        r.render(e.data(), (uint32_t)e.size(), false, out.data(), N, totalCycles);
        double rr = rms(out.data() + 4410, N - 4410);   // skip 0.1s filter warm-up
        std::printf("31.25kHz toggle -> rms=%.1f  (want near 0)\n", rr);
        if (rr > 50.0) { std::printf("  FAIL: ultrasonic whine not suppressed\n"); fails++; }
    }
    // 2) 1 kHz square (gap 500) = a real note; must survive.
    {
        auto e = makeSquare(500, (uint32_t)totalCycles);
        Cb2Render r; r.configure(fs, cpu, 8000);
        r.render(e.data(), (uint32_t)e.size(), false, out.data(), N, totalCycles);
        double g = goertzel(out.data() + 4410, N - 4410, 1000.0, fs);
        std::printf("1kHz square    -> 1kHz mag=%.1f  (want strong)\n", g);
        if (g < 1000.0) { std::printf("  FAIL: real note attenuated\n"); fails++; }
    }
    // 3) 4 kHz square (gap 125) = a high note; should still pass reasonably.
    {
        auto e = makeSquare(125, (uint32_t)totalCycles);
        Cb2Render r; r.configure(fs, cpu, 8000);
        r.render(e.data(), (uint32_t)e.size(), false, out.data(), N, totalCycles);
        double g = goertzel(out.data() + 4410, N - 4410, 4000.0, fs);
        std::printf("4kHz square    -> 4kHz mag=%.1f  (want present)\n", g);
        if (g < 500.0) { std::printf("  FAIL: high note lost\n"); fails++; }
    }

    std::printf("\n%s\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}
