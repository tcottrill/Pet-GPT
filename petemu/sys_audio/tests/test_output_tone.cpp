// test_output_tone.cpp - pins the global "cabinet tone" output filter
// ([main] audio_filter). Build and run with build_output_tone.bat.
#include "../output_tone.h"
#include "microtest.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

static const double kPi = 3.14159265358979323846;

// Gain in dB of a steady sine through the filter (RMS of the last half).
static double gain_db(unsigned rate, double freq, int preset, unsigned channels = 2)
{
    OutputTone t;
    t.configure(rate, channels);
    const unsigned frames = rate;   // 1 s
    std::vector<float> buf((size_t)frames * channels);
    for (unsigned i = 0; i < frames; ++i)
        for (unsigned c = 0; c < channels; ++c)
            buf[(size_t)i * channels + c] = (float)(0.5 * std::sin(2.0 * kPi * freq * i / rate));
    t.process(buf.data(), frames, preset);
    double acc = 0; size_t n = 0;
    for (unsigned i = frames / 2; i < frames; ++i, ++n) {
        const double v = buf[(size_t)i * channels];   // channel 0
        acc += v * v;
    }
    const double rms = std::sqrt(acc / n);
    return 20.0 * std::log10(rms / (0.5 / std::sqrt(2.0)));
}

static void test_off_is_bit_identical()
{
    OutputTone t;
    t.configure(44100, 2);
    std::vector<float> a(2000), b;
    for (size_t i = 0; i < a.size(); ++i) a[i] = (float)std::sin(0.01 * i) * 0.7f;
    b = a;
    t.process(b.data(), 1000, 0);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);

    std::vector<int16_t> x(2000), y;
    for (size_t i = 0; i < x.size(); ++i) x[i] = (int16_t)(20000 * std::sin(0.02 * i));
    y = x;
    t.process_i16(y.data(), 1000, 0);
    CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(int16_t)) == 0);
}

static void test_light_response_44100()
{
    const double g1k = gain_db(44100, 1000, 1);
    const double g12k = gain_db(44100, 12000, 1);
    const double g18k = gain_db(44100, 18000, 1);
    std::printf("LIGHT 44100: 1k %.2f dB, 12k %.2f dB, 18k %.2f dB\n", g1k, g12k, g18k);
    CHECK(std::fabs(g1k) < 0.5);
    CHECK(std::fabs(g12k + 3.0) < 0.5);
    CHECK(g18k < -6.0);
}

static void test_warm_response()
{
    const double g7k = gain_db(44100, 7000, 2);
    const double g1k = gain_db(44100, 1000, 2);
    std::printf("WARM 44100: 1k %.2f dB, 7k %.2f dB\n", g1k, g7k);
    CHECK(std::fabs(g7k + 3.0) < 0.5);
    CHECK(std::fabs(g1k) < 0.5);
}

static void test_dc_decays()
{
    OutputTone t;
    t.configure(44100, 2);
    std::vector<float> buf(44100 * 2, 0.5f);
    t.process(buf.data(), 44100, 1);
    CHECK(std::fabs(buf[(44100 - 1) * 2]) < 0.01f);
    CHECK(std::fabs(buf[(44100 - 1) * 2 + 1]) < 0.01f);
}

static void test_i16_square_no_wrap()
{
    OutputTone t;
    t.configure(44100, 2);
    const unsigned frames = 4000;
    std::vector<int16_t> buf((size_t)frames * 2);
    for (unsigned i = 0; i < frames; ++i) {
        const int16_t v = ((i / 20) & 1) ? 32767 : -32768;
        buf[(size_t)i * 2] = v;
        buf[(size_t)i * 2 + 1] = v;
    }
    t.process_i16(buf.data(), frames, 1);
    // A wrapped overshoot would flip sign inside a high half-cycle. Check the
    // settled middle of each half cycle keeps the input's sign (after the
    // initial DC-block / filter transient).
    int flips = 0;
    for (unsigned i = 1000; i < frames; ++i) {
        const unsigned phase = i % 20;
        if (phase < 8 || phase > 12) continue;
        const bool high = ((i / 20) & 1) != 0;
        const int16_t v = buf[(size_t)i * 2];
        if (high && v < 0) ++flips;
        if (!high && v > 0) ++flips;
    }
    CHECK_EQ(flips, 0);
}

static void test_switch_no_jump()
{
    OutputTone t;
    t.configure(44100, 2);
    const unsigned frames = 20000;
    std::vector<float> buf((size_t)frames * 2);
    for (unsigned i = 0; i < frames; ++i) {
        const float v = (float)(0.5 * std::sin(2.0 * kPi * 3000.0 * i / 44100.0));
        buf[(size_t)i * 2] = v; buf[(size_t)i * 2 + 1] = v;
    }
    t.process(buf.data(), 10000, 1);
    t.process(buf.data() + 10000 * 2, 10000, 2);   // switch LIGHT -> WARM
    float maxJump = 0;
    for (unsigned i = 9000; i < 11000; ++i)
        maxJump = std::fmax(maxJump, std::fabs(buf[(size_t)(i + 1) * 2] - buf[(size_t)i * 2]));
    std::printf("max sample step across switch: %.4f\n", maxJump);
    // 3 kHz sine, amp 0.5: natural max step ~ 0.5*2*pi*3000/44100 = 0.21
    CHECK(maxJump < 0.3f);
}

static void test_rates_and_channels()
{
    const double g48 = gain_db(48000, 1000, 1);
    const double g48_12k = gain_db(48000, 12000, 1);
    CHECK(std::fabs(g48) < 0.5);
    CHECK(std::fabs(g48_12k + 3.0) < 0.5);
    CHECK(std::fabs(gain_db(48000, 1000, 1, 6)) < 0.5);
    CHECK(std::fabs(gain_db(48000, 1000, 1, 2)) < 0.5);
    // fc clamp: at 22050 Hz, 12 kHz > 0.45*rate; must stay stable and finite.
    const double gc = gain_db(22050, 1000, 1);
    CHECK(std::isfinite(gc));
    CHECK(std::fabs(gc) < 1.0);
}

static void run_all()
{
    test_off_is_bit_identical();
    test_light_response_44100();
    test_warm_response();
    test_dc_decays();
    test_i16_square_no_wrap();
    test_switch_no_jump();
    test_rates_and_channels();
}

TEST_MAIN()
