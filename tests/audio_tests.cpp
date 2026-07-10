#include "test_framework.hpp"

#include "capture_panel/core/audio.hpp"
#include "capture_panel/core/errors.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace capture_panel;

CP_TEST_CASE("audio level converts between linear amplitude and dBFS") {
    const auto linear = linear_from_dbfs(-12.0);
    CP_REQUIRE_NEAR(linear, 0.2511886432, 1.0e-9);
    CP_REQUIRE_NEAR(dbfs_from_peak(-linear), -12.0, 1.0e-9);
    CP_REQUIRE_NEAR(dbfs_from_rms(linear), -12.0, 1.0e-9);
    CP_REQUIRE_NEAR(dbfs_from_peak(0.0), silence_dbfs, 1.0e-9);
    CP_REQUIRE_NEAR(dbfs_from_rms(0.0), silence_dbfs, 1.0e-9);
}

CP_TEST_CASE("audio peak and RMS use all interleaved samples") {
    const std::vector<float> samples{-1.0F, 0.5F, 0.25F};
    CP_REQUIRE_NEAR(peak(samples), 1.0, 1.0e-9);
    CP_REQUIRE_NEAR(rms(samples), std::sqrt(1.3125 / 3.0), 1.0e-9);
    CP_REQUIRE_NEAR(peak(std::span<const float>{}), 0.0, 1.0e-9);
    CP_REQUIRE_NEAR(rms(std::span<const float>{}), 0.0, 1.0e-9);
}

CP_TEST_CASE("audio level frame ranges clamp against interleaved buffer bounds") {
    const AudioBuffer buffer{
        48'000.0,
        2,
        {0.1F, -0.2F, 0.3F, -0.4F, 0.5F, -0.6F},
    };

    CP_REQUIRE_NEAR(peak(buffer, 1, 2), 0.6, 1.0e-6);
    CP_REQUIRE_NEAR(
        rms(buffer, 1, 2),
        std::sqrt((0.09 + 0.16 + 0.25 + 0.36) / 4.0),
        1.0e-6);
    CP_REQUIRE_NEAR(peak(buffer, -1, 2), 0.2, 1.0e-6);
    CP_REQUIRE_NEAR(peak(buffer, 99, 10), 0.0, 1.0e-9);
    CP_REQUIRE_NEAR(
        peak(buffer, std::numeric_limits<std::int64_t>::max(), 1),
        0.0,
        1.0e-9);
}

CP_TEST_CASE("audio gain is applied without clipping") {
    std::vector<float> samples{0.25F, -0.5F, 0.0F};
    apply_gain_db(samples, 6.020599913279624);
    CP_REQUIRE_NEAR(samples[0], 0.5, 1.0e-6);
    CP_REQUIRE_NEAR(samples[1], -1.0, 1.0e-6);
    CP_REQUIRE_NEAR(samples[2], 0.0, 1.0e-9);
}

CP_TEST_CASE("frame extraction preserves interleaving and zero pads") {
    const std::vector<float> samples{
        1.0F, 2.0F,
        3.0F, 4.0F,
        5.0F, 6.0F,
    };
    const auto result = extract_frames(samples, -1, 4, 2, 5);
    const std::vector<float> expected{
        0.0F, 0.0F,
        1.0F, 2.0F,
        3.0F, 4.0F,
        5.0F, 6.0F,
        0.0F, 0.0F,
    };
    CP_REQUIRE(result == expected);
}

CP_TEST_CASE("AudioBuffer frame extraction preserves sample rate and channels") {
    const AudioBuffer source{44'100.0, 1, {0.1F, 0.2F, 0.3F}};
    const auto result = extract_frames(source, 1, 2, 4);
    CP_REQUIRE_NEAR(result.sample_rate, 44'100.0, 1.0e-9);
    CP_REQUIRE(result.channel_count == 1);
    CP_REQUIRE(result.samples.size() == 4);
    CP_REQUIRE_NEAR(result.samples[0], 0.2, 1.0e-6);
    CP_REQUIRE_NEAR(result.samples[1], 0.3, 1.0e-6);
    CP_REQUIRE_NEAR(result.samples[2], 0.0, 1.0e-9);
    CP_REQUIRE_NEAR(result.samples[3], 0.0, 1.0e-9);
}

CP_TEST_CASE("frame extraction rejects invalid dimensions") {
    bool bad_channels = false;
    try {
        static_cast<void>(extract_frames(std::span<const float>{}, 0, 0, 0, 0));
    } catch (const CaptureError& error) {
        bad_channels = error.code() == ErrorCode::validation_failed;
    }
    CP_REQUIRE(bad_channels);

    bool bad_frames = false;
    try {
        static_cast<void>(extract_frames(std::span<const float>{}, 0, -1, 1, 0));
    } catch (const CaptureError& error) {
        bad_frames = error.code() == ErrorCode::validation_failed;
    }
    CP_REQUIRE(bad_frames);
}
