#include "test_framework.hpp"

#include "capture_panel/core/errors.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#include <memory>
#include <vector>

using namespace capture_panel;

CP_TEST_CASE("fake backend exposes one 8x8 48 kHz loopback device") {
    const fake::FakeAudioBackend backend;
    const auto devices = backend.devices();

    CP_REQUIRE(devices.size() == 1);
    CP_REQUIRE(devices[0].id == "fake:loopback");
    CP_REQUIRE(devices[0].input_channels == 8);
    CP_REQUIRE(devices[0].output_channels == 8);
    CP_REQUIRE_NEAR(devices[0].sample_rate, 48'000.0, 0.001);
    CP_REQUIRE(backend.channels("fake:loopback", ChannelDirection::input).size() == 8);
    CP_REQUIRE(backend.channels("fake:loopback", ChannelDirection::output)[7].index == 8);
}

CP_TEST_CASE("fake backend loops playback through padding latency gain and route channel count") {
    fake::FakeAudioBackend backend({
        .sample_rate = 10.0,
        .input_channels = 8,
        .output_channels = 8,
        .latency_frames = 2,
        .loopback_gain_db = -6.020599913279624,
        .progress_block_frames = 2,
    });
    std::vector<CaptureProgress> progress;

    const auto result = backend.capture({
        .route = {
            .driver_id = "fake:loopback",
            .playback_channels = {1, 2},
            .record_channels = {7, 8, 6},
        },
        .playback = {
            .sample_rate = 10.0,
            .channel_count = 2,
            .samples = {1.0F, -1.0F, 0.5F, -0.5F},
        },
        .padding_seconds = 0.3,
        .progress = [&](std::int64_t completed, std::int64_t total) {
            progress.push_back({completed, total, 10.0});
        },
    });

    CP_REQUIRE(result.pre_pad_frames == 3);
    CP_REQUIRE(result.recorded.frame_count() == 8);
    CP_REQUIRE(result.recorded.channel_count == 3);
    // Playback begins at pre-pad (3) + configured latency (2).
    CP_REQUIRE_NEAR(result.recorded.samples[5 * 3], 0.5F, 0.000001F);
    CP_REQUIRE_NEAR(result.recorded.samples[5 * 3 + 1], -0.5F, 0.000001F);
    CP_REQUIRE_NEAR(result.recorded.samples[5 * 3 + 2], 0.0F, 0.000001F);
    CP_REQUIRE_NEAR(result.recorded.samples[6 * 3], 0.25F, 0.000001F);
    CP_REQUIRE(!progress.empty());
    CP_REQUIRE(progress.front().completed_frames == 0);
    CP_REQUIRE(progress.back().completed_frames == 8);
    CP_REQUIRE(progress.back().total_frames == 8);
}

CP_TEST_CASE("fake backend rejects invalid routes") {
    fake::FakeAudioBackend backend;
    bool invalid_driver = false;
    try {
        (void)backend.device("missing");
    } catch (const CaptureError& error) {
        invalid_driver = error.code() == ErrorCode::device_not_found;
    }
    CP_REQUIRE(invalid_driver);

    bool invalid_channel = false;
    try {
        (void)backend.capture({
            .route = {
                .driver_id = "fake:loopback",
                .playback_channels = {9},
                .record_channels = {1},
            },
            .playback = {
                .sample_rate = 48'000.0,
                .channel_count = 1,
                .samples = {0.0F},
            },
        });
    } catch (const CaptureError& error) {
        invalid_channel = error.code() == ErrorCode::invalid_channel_specification;
    }
    CP_REQUIRE(invalid_channel);
}

CP_TEST_CASE("fake backend tolerates source and route channel count mismatch") {
    fake::FakeAudioBackend backend({.sample_rate = 10.0});
    const auto result = backend.capture({
        .route = {
            .driver_id = "fake:loopback",
            .playback_channels = {1},
            .record_channels = {1, 2},
        },
        .playback = {
            .sample_rate = 10.0,
            .channel_count = 2,
            .samples = {0.25F, 0.75F},
        },
    });

    CP_REQUIRE(result.recorded.channel_count == 2);
    CP_REQUIRE_NEAR(result.recorded.samples[0], 0.25F, 0.000001F);
    CP_REQUIRE_NEAR(result.recorded.samples[1], 0.0F, 0.000001F);
}

CP_TEST_CASE("fake backend observes cancellation during progress") {
    fake::FakeAudioBackend backend({
        .sample_rate = 10.0,
        .input_channels = 8,
        .output_channels = 8,
        .latency_frames = 0,
        .loopback_gain_db = 0.0,
        .progress_block_frames = 1,
    });
    const auto token = std::make_shared<CancellationToken>();
    bool cancelled = false;

    try {
        (void)backend.capture({
            .route = {
                .driver_id = "fake:loopback",
                .playback_channels = {1},
                .record_channels = {1},
            },
            .playback = {
                .sample_rate = 10.0,
                .channel_count = 1,
                .samples = {1.0F, 1.0F, 1.0F},
            },
            .cancellation = token,
            .progress = [&](std::int64_t completed, std::int64_t) {
                if (completed == 1) token->cancel();
            },
        });
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }

    CP_REQUIRE(cancelled);
}
