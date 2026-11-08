// test_sample_fold.cpp - dual-mono 2-channel sample folding (sample_fold.h).
// Build and run with build_sample_fold.bat. See docs/audio-soundbar-widening.md.
#include "../sample_fold.h"
#include "microtest.h"
#include <cstdint>
#include <vector>

static void test_identical_channels_fold_exactly()
{
    const int16_t in[] = { 100, 100, -2000, -2000, 32767, 32767, -32768, -32768, 0, 0 };
    std::vector<int16_t> mono;
    CHECK(fold_dual_mono_16(in, 5, mono));
    CHECK_EQ((int)mono.size(), 5);
    CHECK_EQ((int)mono[0], 100);
    CHECK_EQ((int)mono[1], -2000);
    CHECK_EQ((int)mono[2], 32767);
    CHECK_EQ((int)mono[3], -32768);
    CHECK_EQ((int)mono[4], 0);
}

static void test_difference_of_2_folds_to_average()
{
    const int16_t in[] = { 100, 102, 50, 48, -7, -9 };
    std::vector<int16_t> mono;
    CHECK(fold_dual_mono_16(in, 3, mono));
    CHECK_EQ((int)mono[0], 101);
    CHECK_EQ((int)mono[1], 49);
    CHECK_EQ((int)mono[2], -8);
}

static void test_difference_of_3_anywhere_rejects()
{
    std::vector<int16_t> in(2000, 500);
    std::vector<int16_t> mono;
    CHECK(fold_dual_mono_16(in.data(), 1000, mono));
    in[2 * 999 + 1] = 503;   // last frame only
    CHECK(!fold_dual_mono_16(in.data(), 1000, mono));
    CHECK(mono.empty());
    in[2 * 999 + 1] = 500;
    in[0] = 497;             // first frame only
    CHECK(!fold_dual_mono_16(in.data(), 1000, mono));
    CHECK(mono.empty());
}

static void test_8bit_fold_and_reject()
{
    const uint8_t in[] = { 128, 128, 200, 202, 10, 8 };
    std::vector<uint8_t> mono;
    CHECK(fold_dual_mono_8(in, 3, mono));
    CHECK_EQ((int)mono[0], 128);
    CHECK_EQ((int)mono[1], 201);
    CHECK_EQ((int)mono[2], 9);
    const uint8_t bad[] = { 128, 128, 200, 203 };
    CHECK(!fold_dual_mono_8(bad, 2, mono));
    CHECK(mono.empty());
}

static void test_empty_input()
{
    std::vector<int16_t> m16;
    std::vector<uint8_t> m8;
    const int16_t d16[2] = { 0, 0 };
    CHECK(!fold_dual_mono_16(d16, 0, m16));
    CHECK(!fold_dual_mono_16(nullptr, 4, m16));
    CHECK(!fold_dual_mono_8(nullptr, 0, m8));
    CHECK(m16.empty());
    CHECK(m8.empty());
}

static void run_all()
{
    test_identical_channels_fold_exactly();
    test_difference_of_2_folds_to_average();
    test_difference_of_3_anywhere_rejects();
    test_8bit_fold_and_reject();
    test_empty_input();
}

TEST_MAIN()
