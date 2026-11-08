// test_mixer_groups.cpp - group tagging and gain math for the mixer's
// SAMPLE / CHIP group volumes. Build with build_mixer_groups.bat.
#include "../mixer_groups.h"
#include "microtest.h"
#include <cmath>

static bool near(float a, float b, float tol = 0.002f) { return std::fabs(a - b) <= tol; }

static void test_group_for_start_tags_by_start_function()
{
    // A stream on a chip channel is CHIP; a sample anywhere below the
    // reserved range is SAMPLE regardless of the channel number.
    CHECK_EQ(mixer_group_for_start(9,  /*is_stream=*/true),  (int)MIXER_GROUP_CHIP);
    CHECK_EQ(mixer_group_for_start(12, /*is_stream=*/true),  (int)MIXER_GROUP_CHIP);
    CHECK_EQ(mixer_group_for_start(0,  /*is_stream=*/false), (int)MIXER_GROUP_SAMPLE);
    CHECK_EQ(mixer_group_for_start(8,  /*is_stream=*/false), (int)MIXER_GROUP_SAMPLE);
    CHECK_EQ(mixer_group_for_start(12, /*is_stream=*/false), (int)MIXER_GROUP_SAMPLE);
}

static void test_group_for_start_reserved_channels_are_untouched()
{
    // 17..19 are the ambient loops; setup_ambient already scales them by
    // noisevol, so they must not also pick up a group gain.
    for (int ch = MIXER_FIRST_RESERVED_CHANNEL; ch < 20; ++ch) {
        CHECK_EQ(mixer_group_for_start(ch, true),  (int)MIXER_GROUP_NONE);
        CHECK_EQ(mixer_group_for_start(ch, false), (int)MIXER_GROUP_NONE);
    }
}

static void test_gain_full_group_is_identity()
{
    // Group byte 255 must reproduce today's behaviour exactly.
    CHECK(near(mixer_group_channel_gain(255, 255), 1.0f, 0.0001f));
    CHECK(near(mixer_group_channel_gain(128, 255), VolumeByteToLinear(128), 0.0001f));
    CHECK(near(mixer_group_channel_gain(5,   255), VolumeByteToLinear(5),   0.0001f));
}

static void test_gain_group_multiplies_through_curve()
{
    // Group byte 128 sits at ~-6.29 dB on the two-segment perceptual curve
    // (VolumeNormalizedToLinear), not exactly -6 dB: a full-scale channel
    // lands at ~0.4847, and a channel at that same -6.29 dB lands at
    // ~0.2349 (that value squared, i.e. ~-12.58 dB).
    CHECK(near(mixer_group_channel_gain(255, 128), 0.4847f, 0.0005f));
    CHECK(near(mixer_group_channel_gain(128, 128), 0.2349f, 0.0005f));
    // Commutative: the two bytes are interchangeable.
    CHECK(near(mixer_group_channel_gain(200, 100), mixer_group_channel_gain(100, 200), 0.0001f));
}

static void test_gain_group_zero_is_effectively_silent()
{
    // Curve bottom is -80 dB (1e-4), not a hard zero.
    CHECK(mixer_group_channel_gain(255, 0) < 0.0002f);
    CHECK(mixer_group_channel_gain(255, 0) > 0.0f);
}

static void test_gain_clamps_out_of_range_bytes()
{
    CHECK(near(mixer_group_channel_gain(999, 999), 1.0f, 0.0001f));
    CHECK(near(mixer_group_channel_gain(-5, 255), mixer_group_channel_gain(0, 255), 0.0001f));
}

static void test_trim_255_is_identity()
{
    CHECK(near(mixer_group_channel_gain(200, 128, 255), mixer_group_channel_gain(200, 128), 0.0001f));
    CHECK(near(mixer_group_channel_gain(255, 255, 255), 1.0f, 0.0001f));
}

static void test_trim_multiplies_through_curve()
{
    // 170 on the curve is -4.21 dB = 0.6157 linear (the Asteroids Deluxe /
    // Battlezone sample trim).
    CHECK(near(mixer_group_channel_gain(255, 255, 170), 0.6157f, 0.005f));
    CHECK(near(mixer_group_channel_gain(128, 255, 170), VolumeByteToLinear(128) * 0.6157f, 0.005f));
}

static void test_trim_from_driver_treats_zero_as_none()
{
    CHECK_EQ(mixer_trim_from_driver(0),   255);   // omitted macro -> value-initialised 0
    CHECK_EQ(mixer_trim_from_driver(-3),  255);
    CHECK_EQ(mixer_trim_from_driver(170), 170);
    CHECK_EQ(mixer_trim_from_driver(255), 255);
    CHECK_EQ(mixer_trim_from_driver(999), 255);
}

static void run_all()
{
    test_group_for_start_tags_by_start_function();
    test_group_for_start_reserved_channels_are_untouched();
    test_gain_full_group_is_identity();
    test_gain_group_multiplies_through_curve();
    test_gain_group_zero_is_effectively_silent();
    test_gain_clamps_out_of_range_bytes();
    test_trim_255_is_identity();
    test_trim_multiplies_through_curve();
    test_trim_from_driver_treats_zero_as_none();
}

TEST_MAIN()
