// test_mono_stereo_width.cpp - pins the soundbar/TV mono widening that was
// proven by ear on the Hisense -> eARC -> Sonos chain (commit 084012c).
// Build and run with build_mono_stereo_width.bat.
//
// If one of these fails you changed the widening. Read
// docs/audio-soundbar-widening.md first: the "obvious improvements" (lower
// gain, low-cut, shorter delay) are exactly the variants that left the Sonos
// rears and sub silent. Change this test only together with a new on-hardware
// A/B and an update to that document.
#include "../mono_stereo_width.h"
#include "microtest.h"
#include <cmath>
#include <cstdint>
#include <vector>

static const unsigned kRate = 48000;
static const int      kDelay = (int)(kRate * 12 / 1000);   // 576 samples = 12 ms

static void test_disabled_by_default_and_when_configured_off()
{
    MonoStereoWidth w;
    CHECK(!w.enabled());
    w.configure(false, kRate);
    CHECK(!w.enabled());
    w.configure(true, kRate);
    CHECK(w.enabled());
}

static void test_impulse_comes_out_12ms_late_at_0_7_plus_left_minus_right()
{
    MonoStereoWidth w;
    w.configure(true, kRate);
    const int amp = 10000;
    int hitAt = -1, dlAtHit = 0, drAtHit = 0, nonZero = 0;
    for (int n = 0; n < kDelay * 3; ++n) {
        int dl = 0, dr = 0;
        w.process(n == 0 ? amp : 0, dl, dr);
        if (dl != 0 || dr != 0) {
            ++nonZero;
            if (hitAt < 0) { hitAt = n; dlAtHit = dl; drAtHit = dr; }
        }
    }
    CHECK_EQ(nonZero, 1);          // one echo, no feedback, no filter smear
    CHECK_EQ(hitAt, kDelay + 1);   // delay line of 1 + 12 ms: the shipped timing
    CHECK_EQ(dlAtHit, 7000);       // 0.7, full band (an impulse is all bands)
    CHECK_EQ(drAtHit, -7000);      // subtracted from R: antiphase pair
}

static void test_dc_passes_full_band_bass_included()
{
    // A low-cut on the delayed copy was one of the variants that failed (no
    // sub). DC must come through at the full 0.7 once the line has filled.
    MonoStereoWidth w;
    w.configure(true, kRate);
    int dl = 0, dr = 0;
    for (int n = 0; n < kDelay * 4; ++n) w.process(8000, dl, dr);
    CHECK_EQ(dl, 5600);
    CHECK_EQ(dr, -5600);
}

static void test_widened_mono_has_lr_correlation_about_0_34()
{
    // Dual mono (L == R) has correlation 1.0 and gets no rears. The shipped
    // setting gives (1 - 0.49) / (1 + 0.49) = 0.342 on broadband content.
    MonoStereoWidth w;
    w.configure(true, kRate);
    uint32_t seed = 12345;
    double sLL = 0, sRR = 0, sLR = 0;
    for (int n = 0; n < (int)kRate * 4; ++n) {
        seed = seed * 1664525u + 1013904223u;
        const int mono = (int)((seed >> 16) & 0x3FFF) - 0x2000;   // white, +-8192
        int dl = 0, dr = 0;
        w.process(mono, dl, dr);
        if (n < kDelay * 2) continue;   // let the delay line fill
        const double L = mono + dl, R = mono + dr;
        sLL += L * L; sRR += R * R; sLR += L * R;
    }
    const double corr = sLR / std::sqrt(sLL * sRR);
    std::printf("widened mono L/R correlation = %.3f (expect ~0.342)\n", corr);
    CHECK(std::fabs(corr - 0.342) < 0.03);
}

static void test_reconfigure_clears_the_delay_line()
{
    // configure() runs at every backend Init (each game start); a stale tail
    // from the previous game must not leak into the next.
    MonoStereoWidth w;
    w.configure(true, kRate);
    int dl = 0, dr = 0;
    for (int n = 0; n < 10; ++n) w.process(10000, dl, dr);
    w.configure(true, kRate);
    int nonZero = 0;
    for (int n = 0; n < kDelay * 2; ++n) {
        w.process(0, dl, dr);
        if (dl || dr) ++nonZero;
    }
    CHECK_EQ(nonZero, 0);
}

static void run_all()
{
    test_disabled_by_default_and_when_configured_off();
    test_impulse_comes_out_12ms_late_at_0_7_plus_left_minus_right();
    test_dc_passes_full_band_bass_included();
    test_widened_mono_has_lr_correlation_about_0_34();
    test_reconfigure_clears_the_delay_line();
}

TEST_MAIN()
