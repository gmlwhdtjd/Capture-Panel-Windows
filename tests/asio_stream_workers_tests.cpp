#include "asio_stream_workers.hpp"
#include "capture_panel/core/audio.hpp"
#include "capture_panel/core/constants.hpp"
#include "test_framework.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>

namespace capture_panel::test {
namespace {

[[nodiscard]] CapturePassPlaybackPlan short_playback_plan() {
    return {
        .source = CaptureAudioSource::from_memory({
            .sample_rate = 10.0,
            .channel_count = 1,
            .samples = {0.2F, -0.4F},
        }),
        .playback_frame_count = 3,
        .marker_frames = {0},
        .source_start_frame = 1,
        .playback_gain = 0.5F,
    };
}

} // namespace

CP_TEST_CASE("ASIO stream worker produces markers and gained source in order") {
    asio::AsioStreamWorkers workers(
        short_playback_plan(),
        1,
        3,
        1,
        std::nullopt);
    workers.start();
    workers.wait_until_playback_ready(3, std::chrono::seconds(2), nullptr);

    std::array<float, 3> samples{};
    CP_REQUIRE(workers.playback_ring().read_exact(samples));
    const auto marker = static_cast<float>(linear_from_dbfs(
        constants::alignment::impulse_level_dbfs)) * 0.5F;
    CP_REQUIRE(std::abs(samples[0] - marker) < 0.000001F);
    CP_REQUIRE(std::abs(samples[1] - 0.1F) < 0.000001F);
    CP_REQUIRE(std::abs(samples[2] + 0.2F) < 0.000001F);

    workers.request_stop();
    workers.join();
}

CP_TEST_CASE("ASIO stream writer drains the final block before asset handoff") {
    std::filesystem::path temporary_path;
    Float32AudioAsset asset;
    {
        asio::AsioStreamWorkers workers(
            short_playback_plan(),
            1,
            3,
            1,
            std::nullopt);
        workers.start();
        workers.wait_until_playback_ready(3, std::chrono::seconds(2), nullptr);

        const std::array recorded{0.25F, -0.75F, 0.5F};
        CP_REQUIRE(workers.recording_ring().write_exact(recorded));
        workers.mark_recording_producer_finished();
        workers.join();
        CP_REQUIRE(workers.writer_finished());

        asset = workers.take_recorded_asset();
        CP_REQUIRE(asset.path().has_value());
        temporary_path = *asset.path();
        CP_REQUIRE(std::filesystem::exists(temporary_path));
        CP_REQUIRE(asset.raw_peak().has_value());
        CP_REQUIRE(std::abs(*asset.raw_peak() - 0.75F) < 0.000001F);
    }

    auto reader = asset.make_reader();
    std::array<float, 3> samples{};
    CP_REQUIRE(reader->read_frames(samples) == 3);
    CP_REQUIRE(samples[0] == 0.25F);
    CP_REQUIRE(samples[1] == -0.75F);
    CP_REQUIRE(samples[2] == 0.5F);
    reader.reset();
    asset = {};
    CP_REQUIRE(!std::filesystem::exists(temporary_path));
}

} // namespace capture_panel::test
