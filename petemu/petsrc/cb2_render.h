// -----------------------------------------------------------------------------
// cb2_render.h
// CB2 (1-bit PET speaker) -> PCM reconstruction, extracted from emulator.cpp so
// it can be unit-tested in isolation and shared by the emulator and the tests.
//
// The PET speaker is driven directly by the VIA CB2 line. We are handed a list
// of CB2 transitions (edges) with cycle timestamps, plus the CB2 level at the
// start of the window. For each output sample we compute the exact fraction of
// time CB2 was high over that sample's cycle window (a boxcar/area integral,
// which is the correct band-limiting average), then band-limit to the PET's
// audio bandwidth and remove DC.
//
// Why the low-pass matters: programs sometimes park the VIA shift register at an
// ULTRASONIC frequency (e.g. Space Invaders idles it at ~31 kHz). On real PET
// hardware the speaker/amp can't reproduce that, so it's silent. Without band-
// limiting it aliases into the audible range as a high-pitched whine. A proper
// low-pass (modelling the PET's limited audio bandwidth) removes it while leaving
// real notes (<= ~5 kHz) intact.
//
// This header is dependency-light (CB2Edge from via6522.h + <cmath>) so it links
// into a standalone test harness.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cmath>
#include "via6522.h"   // CB2Edge

// 2-pole (biquad) low-pass section; two cascaded sections give a ~24 dB/oct rolloff.
struct Cb2Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void reset() { x1 = x2 = y1 = y2 = 0; }

    void designLowpass(double fc, double fs)
    {
        const double PI = 3.14159265358979323846;
        if (fc > fs * 0.45) fc = fs * 0.45;
        const double w0 = 2.0 * PI * fc / fs;
        const double c = std::cos(w0), s = std::sin(w0);
        const double Q = 0.70710678;          // Butterworth
        const double alpha = s / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        b0 = ((1.0 - c) * 0.5) / a0;
        b1 = (1.0 - c) / a0;
        b2 = ((1.0 - c) * 0.5) / a0;
        a1 = (-2.0 * c) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    inline double process(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

class Cb2Render
{
public:
    // Configuration (set once; safe defaults for the PET).
    double sampleRate = 44100.0;   // output PCM rate
    double cpuHz      = 1000000.0; // PET CPU / CB2 tick rate
    double volume     = 8000.0;    // output scale into int16
    double cutoffHz   = 7000.0;    // audio bandwidth (tuning knob)
    int    oversample = 8;         // internal oversampling factor (anti-aliasing)

    void configure(double fs, double cpu, double vol)
    {
        sampleRate = fs; cpuHz = cpu; volume = vol;
        designFilter();
        reset();
    }

    void setCutoff(double fc) { cutoffHz = fc; designFilter(); }

    // Reset persistent filter state (call on machine reset / audio restart).
    void reset()
    {
        lp1_.reset();
        lp2_.reset();
        dcPrevIn_ = 0.0;
        dcOut_ = 0.0;
    }

    // Render `numSamples` mono int16 samples from the CB2 edge list.
    //   edges        - transitions with frame-relative cycle stamps, ascending
    //   edgeCount    - number of edges
    //   startLevel   - CB2 level at cycle 0 of this buffer
    //   out          - output buffer (numSamples int16)
    //   totalCycles  - actual cycle span this buffer covers (e.g. the VIA tick
    //                  counter at frame end). When > 0 the edge->sample mapping
    //                  uses the REAL span so no tail transition is dropped and
    //                  audio-time matches game-time. <= 0 falls back to the
    //                  nominal cpuHz/sampleRate mapping.
    // Filter state persists across calls for inter-frame continuity.
    void render(const CB2Edge* edges, uint32_t edgeCount, bool startLevel,
                int16_t* out, int numSamples, double totalCycles = 0.0)
    {
        const int OS = (oversample < 1) ? 1 : oversample;
        const double cps = (totalCycles > 0.0 && numSamples > 0)
                               ? (totalCycles / (double)numSamples)
                               : (cpuHz / sampleRate);
        const double subCps = cps / (double)OS;   // cycles per oversampled sub-sample
        uint32_t idx = 0;
        bool level = startLevel;

        for (int i = 0; i < numSamples; ++i)
        {
            // Reconstruct + low-pass at the OVERSAMPLED rate, where an ultrasonic
            // toggle (e.g. SI's 31 kHz idle) is a real, in-band signal that the
            // low-pass removes cleanly -- so it never folds back as an audible
            // whine. Then decimate (average) down to the output rate.
            double acc = 0.0;
            for (int k = 0; k < OS; ++k)
            {
                const long sub = (long)i * OS + k;
                const double s0 = sub * subCps;
                const double s1 = (sub + 1) * subCps;
                const double ratio = duty(edges, edgeCount, &idx, s0, s1, &level);
                const double raw = (ratio * 2.0) - 1.0;     // [0,1] -> [-1,1]
                acc += lp2_.process(lp1_.process(raw));      // filter at fs*OS
            }
            const double dec = acc / (double)OS;             // boxcar decimate

            // DC blocker at the output rate.
            double o = dec - dcPrevIn_ + kDcPole * dcOut_;
            dcPrevIn_ = dec;
            dcOut_ = o;

            if (o > 1.0) o = 1.0;
            if (o < -1.0) o = -1.0;
            out[i] = (int16_t)(o * volume);
        }
    }

    static constexpr double kDcPole = 0.9995;  // DC blocker pole

    // Exact time-high fraction of [startCycle, endCycle). Public for tests.
    static double duty(const CB2Edge* edges, uint32_t edgeCount, uint32_t* ioEdgeIdx,
                       double startCycle, double endCycle, bool* ioLevel)
    {
        if (!ioEdgeIdx || !ioLevel) return 0.0;
        if (endCycle <= startCycle) return 0.0;

        uint32_t idx = *ioEdgeIdx;
        bool level = *ioLevel;
        double timeHigh = 0.0;
        double currentPos = startCycle;

        while (idx < edgeCount && (double)edges[idx].cycle < endCycle)
        {
            const double edgeTime = (double)edges[idx].cycle;
            if (edgeTime < startCycle) { level = edges[idx].level; idx++; continue; }
            if (edgeTime > currentPos && level) timeHigh += (edgeTime - currentPos);
            currentPos = edgeTime;
            level = edges[idx].level;
            idx++;
        }
        if (endCycle > currentPos && level) timeHigh += (endCycle - currentPos);

        *ioEdgeIdx = idx;
        *ioLevel = level;
        return timeHigh / (endCycle - startCycle);
    }

private:
    void designFilter()
    {
        // The low-pass runs at the OVERSAMPLED rate so it can remove ultrasonic
        // content (e.g. 31 kHz) before decimation -> no aliasing.
        const int OS = (oversample < 1) ? 1 : oversample;
        const double fos = sampleRate * (double)OS;
        lp1_.designLowpass(cutoffHz, fos);
        lp2_.designLowpass(cutoffHz, fos);
    }

    Cb2Biquad lp1_, lp2_;     // cascaded -> ~24 dB/oct
    double dcPrevIn_ = 0.0;
    double dcOut_ = 0.0;
};
