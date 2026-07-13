#include "test_framework.hpp"

#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/wav.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace capture_panel;

namespace {

[[nodiscard]] CapturePassPlaybackPlan direct_playback(AudioBuffer audio) {
    const auto frame_count = audio.frame_count();
    return {
        .source = CaptureAudioSource::from_memory(std::move(audio)),
        .playback_frame_count = frame_count,
        .marker_frames = {},
        .source_start_frame = 0,
        .playback_gain = 1.0F,
    };
}

[[nodiscard]] AudioBuffer materialize(const Float32AudioAsset& asset) {
    return AlignedCapturePayload{
        .asset = asset,
        .start_frame = 0,
        .frame_count = asset.frame_count(),
        .gain = 1.0F,
    }.materialize();
}

class TemporaryWav final {
public:
    TemporaryWav() {
        static std::atomic_uint64_t sequence{0};
        path = std::filesystem::temp_directory_path()
            / ("capture-panel-fake-source-"
               + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed))
               + ".wav");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    ~TemporaryWav() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::filesystem::path path;
};

} // namespace

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
        .playback_plan = direct_playback({
            .sample_rate = 10.0,
            .channel_count = 2,
            .samples = {1.0F, -1.0F, 0.5F, -0.5F},
        }),
        .padding_seconds = 0.3,
        .progress = [&](std::int64_t completed, std::int64_t total) {
            progress.push_back({completed, total, 10.0});
        },
    });
    const auto recorded = materialize(result.recorded);

    CP_REQUIRE(result.pre_pad_frames == 3);
    CP_REQUIRE(result.recorded.frame_count() == 8);
    CP_REQUIRE(result.recorded.channel_count() == 3);
    // Playback begins at pre-pad (3) + configured latency (2).
    CP_REQUIRE_NEAR(recorded.samples[5 * 3], 0.5F, 0.000001F);
    CP_REQUIRE_NEAR(recorded.samples[5 * 3 + 1], -0.5F, 0.000001F);
    CP_REQUIRE_NEAR(recorded.samples[5 * 3 + 2], 0.0F, 0.000001F);
    CP_REQUIRE_NEAR(recorded.samples[6 * 3], 0.25F, 0.000001F);
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
            .playback_plan = direct_playback({
                .sample_rate = 48'000.0,
                .channel_count = 1,
                .samples = {0.0F},
            }),
        });
    } catch (const CaptureError& error) {
        invalid_channel = error.code() == ErrorCode::validation_failed;
    }
    CP_REQUIRE(invalid_channel);
}

CP_TEST_CASE("fake backend rejects duplicate playback and record channels") {
    fake::FakeAudioBackend backend;
    for (const auto duplicate_playback : {true, false}) {
        bool failed = false;
        try {
            static_cast<void>(backend.capture({
                .route = {
                    .driver_id = "fake:loopback",
                    .playback_channels = duplicate_playback
                        ? std::vector<std::uint32_t>{1, 1}
                        : std::vector<std::uint32_t>{1},
                    .record_channels = duplicate_playback
                        ? std::vector<std::uint32_t>{1}
                        : std::vector<std::uint32_t>{1, 1},
                },
                .playback_plan = direct_playback({
                    .sample_rate = 48'000.0,
                    .channel_count = 1,
                    .samples = {0.0F},
                }),
            }));
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::validation_failed;
        }
        CP_REQUIRE(failed);
    }
}

CP_TEST_CASE("fake backend tolerates source and route channel count mismatch") {
    fake::FakeAudioBackend backend({.sample_rate = 10.0});
    const auto result = backend.capture({
        .route = {
            .driver_id = "fake:loopback",
            .playback_channels = {1},
            .record_channels = {1, 2},
        },
        .playback_plan = direct_playback({
            .sample_rate = 10.0,
            .channel_count = 2,
            .samples = {0.25F, 0.75F},
        }),
    });
    const auto recorded = materialize(result.recorded);

    CP_REQUIRE(result.recorded.channel_count() == 2);
    CP_REQUIRE_NEAR(recorded.samples[0], 0.25F, 0.000001F);
    CP_REQUIRE_NEAR(recorded.samples[1], 0.0F, 0.000001F);
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
            .playback_plan = direct_playback({
                .sample_rate = 10.0,
                .channel_count = 1,
                .samples = {1.0F, 1.0F, 1.0F},
            }),
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

CP_TEST_CASE("fake backend handles maximum latency without signed underflow") {
    fake::FakeAudioBackend backend({
        .sample_rate = 10.0,
        .latency_frames = std::numeric_limits<std::int64_t>::max(),
        .progress_block_frames = 1,
    });
    const auto result = backend.capture({
        .route = {
            .driver_id = "fake:loopback",
            .playback_channels = {1},
            .record_channels = {1},
        },
        .playback_plan = direct_playback({10.0, 1, {0.25F, 0.5F, 0.75F}}),
    });
    const auto recorded = materialize(result.recorded);
    CP_REQUIRE(recorded.frame_count() == 3);
    CP_REQUIRE(std::all_of(
        recorded.samples.begin(), recorded.samples.end(),
        [](const float sample) { return sample == 0.0F; }));
}

CP_TEST_CASE("fake backend drains and identity-checks source hidden beyond input latency") {
    TemporaryWav file;
    write_wav(file.path, std::vector<float>{0.25F, 0.5F, 0.75F}, 10.0, 1);
    const auto format = read_wav_format(file.path);
    const auto original_time = std::filesystem::last_write_time(file.path);
    fake::FakeAudioBackend backend({
        .sample_rate = 10.0,
        .latency_frames = std::numeric_limits<std::int64_t>::max(),
        .progress_block_frames = 1,
    });
    bool failed = false;
    try {
        static_cast<void>(backend.capture({
            .route = {
                .driver_id = "fake:loopback",
                .playback_channels = {1},
                .record_channels = {1},
            },
            .playback_plan = {
                .source = CaptureAudioSource::from_wav(file.path, format),
                .playback_frame_count = format.total_frames,
                .source_start_frame = 0,
                .playback_gain = 1.0F,
            },
            .progress = [&](const std::int64_t completed, const std::int64_t total) {
                if (completed == total) {
                    std::filesystem::last_write_time(
                        file.path, original_time + std::chrono::seconds(2));
                }
            },
        }));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::source_stream_failure;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("fake backend rejects a combined gain that overflows Float32") {
    fake::FakeAudioBackend backend({
        .sample_rate = 10.0,
        .loopback_gain_db = std::numeric_limits<double>::max(),
    });
    bool failed = false;
    try {
        static_cast<void>(backend.capture({
            .route = {
                .driver_id = "fake:loopback",
                .playback_channels = {1},
                .record_channels = {1},
            },
            .playback_plan = direct_playback({10.0, 1, {0.25F}}),
        }));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::validation_failed;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("fake backend temporary recording is owned and cleaned by the asset") {
    std::optional<std::filesystem::path> scratch_path;
    {
        fake::FakeAudioBackend backend({.sample_rate = 10.0});
        const auto result = backend.capture({
            .route = {
                .driver_id = "fake:loopback",
                .playback_channels = {1},
                .record_channels = {1},
            },
            .playback_plan = direct_playback({10.0, 1, {0.25F}}),
        });
        scratch_path = result.recorded.path();
        CP_REQUIRE(scratch_path.has_value());
        CP_REQUIRE(std::filesystem::exists(*scratch_path));
    }
    CP_REQUIRE(scratch_path.has_value());
    CP_REQUIRE(!std::filesystem::exists(*scratch_path));
}

CP_TEST_CASE("fake backend falls back to a short scratch name for long output components") {
    fake::FakeAudioBackend backend({.sample_rate = 10.0});
    const auto long_name = std::string(230, 'x') + ".capture-panel.tmp.";
    const auto prefix = std::filesystem::temp_directory_path() / long_name;
    const auto result = backend.capture({
        .route = {
            .driver_id = "fake:loopback",
            .playback_channels = {1},
            .record_channels = {1},
        },
        .playback_plan = direct_playback({10.0, 1, {0.25F}}),
        .scratch_file_prefix = prefix,
    });
    CP_REQUIRE(result.recorded.path().has_value());
    CP_REQUIRE(result.recorded.path()->filename().native().size() <= 255);
    CP_REQUIRE(result.recorded.path()->filename().string().starts_with(
        ".capture-panel.tmp."));
}
