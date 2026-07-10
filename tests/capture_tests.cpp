#include "test_framework.hpp"

#include "capture_panel/core/capture.hpp"
#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/wav.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace capture_panel;

class TemporaryWavPair final {
public:
    explicit TemporaryWavPair(std::string stem) {
        const auto root = std::filesystem::temp_directory_path();
        input = root / (stem + "-input.wav");
        output = root / (stem + "-output.wav");
        std::error_code ignored;
        std::filesystem::remove(input, ignored);
        std::filesystem::remove(output, ignored);
    }

    ~TemporaryWavPair() {
        std::error_code ignored;
        std::filesystem::remove(input, ignored);
        std::filesystem::remove(output, ignored);
    }

    std::filesystem::path input;
    std::filesystem::path output;
};

AudioBuffer sine_source(double sample_rate, std::int64_t frames) {
    AudioBuffer result{
        .sample_rate = sample_rate,
        .channel_count = 1,
        .samples = std::vector<float>(static_cast<std::size_t>(frames), 0.0F),
    };
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        result.samples[static_cast<std::size_t>(frame)] = static_cast<float>(
            0.25 * std::sin(2.0 * 3.14159265358979323846 * 997.0
                * static_cast<double>(frame) / sample_rate));
    }
    return result;
}

CaptureConfiguration configuration_for(const TemporaryWavPair& files) {
    return {
        .input_path = files.input,
        .output_path = files.output,
        .route = {
            .driver_id = std::string(capture_panel::fake::loopback_device_id),
            .playback_channels = {1},
            .record_channels = {1},
        },
        .output_bit_depth = AudioBitDepth::pcm24,
    };
}

} // namespace

CP_TEST_CASE("CaptureService runs the complete fake loopback capture pipeline") {
    TemporaryWavPair files("capture-panel-full-pipeline");
    const auto source = sine_source(44'100.0, 4'410);
    write_wav(files.input, source, AudioBitDepth::pcm24);

    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>(
        capture_panel::fake::FakeBackendOptions{.latency_frames = 96});
    std::vector<CaptureEvent> events;
    CaptureService service(backend, backend, [&](const CaptureEvent& event) {
        events.push_back(event);
    });

    const auto result = service.capture(configuration_for(files));
    const auto output = read_wav(files.output);

    CP_REQUIRE(result.output.channel_count == 1);
    CP_REQUIRE(result.alignment.marker_latency_samples.has_value());
    CP_REQUIRE(result.alignment.marker_latency_samples.value() == 96);
    CP_REQUIRE(output.audio.frame_count() == source.frame_count());
    CP_REQUIRE(output.audio.channel_count == source.channel_count);
    CP_REQUIRE(output.audio.samples.size() == source.samples.size());
    for (std::size_t index = 0; index < source.samples.size(); index += 137) {
        CP_REQUIRE_NEAR(output.audio.samples[index], source.samples[index], 0.00001);
    }
    const auto event_of_type = [&](CaptureEventType type) {
        return std::find_if(events.begin(), events.end(), [type](const CaptureEvent& event) {
            return event.type == type;
        });
    };
    const auto input_event = event_of_type(CaptureEventType::input_loaded);
    CP_REQUIRE(input_event != events.end());
    CP_REQUIRE(input_event->input.has_value());
    CP_REQUIRE(input_event->input->format.total_frames == source.frame_count());
    const auto device_event = event_of_type(CaptureEventType::devices_validated);
    CP_REQUIRE(device_event != events.end());
    CP_REQUIRE(device_event->device.has_value());
    CP_REQUIRE(device_event->route.has_value());
    const auto stage_event = event_of_type(CaptureEventType::stage_changed);
    CP_REQUIRE(stage_event != events.end());
    CP_REQUIRE(stage_event->stage == CaptureStage::sample_rate_configuration);
    CP_REQUIRE(stage_event->sample_rate.has_value());
    CP_REQUIRE_NEAR(*stage_event->sample_rate, source.sample_rate, 0.01);
    const auto marker_event = event_of_type(CaptureEventType::impulse_detection);
    CP_REQUIRE(marker_event != events.end());
    CP_REQUIRE(marker_event->impulse_detection.has_value());
    const auto alignment_event = event_of_type(CaptureEventType::alignment_finished);
    CP_REQUIRE(alignment_event != events.end());
    CP_REQUIRE(alignment_event->alignment.has_value());
    const auto output_event = event_of_type(CaptureEventType::output_written);
    CP_REQUIRE(output_event != events.end());
    CP_REQUIRE(output_event->output.has_value());

    // The fake device sample rate is restored after the pass.
    CP_REQUIRE_NEAR(backend->device(std::string(capture_panel::fake::loopback_device_id)).sample_rate,
                    48'000.0,
                    0.01);
}

CP_TEST_CASE("CaptureProgress reports percentage and remaining duration") {
    const CaptureProgress progress{
        .completed_frames = 250,
        .total_frames = 1'000,
        .sample_rate = 100.0,
    };
    CP_REQUIRE(progress.percentage() == 25);
    CP_REQUIRE_NEAR(progress.remaining_seconds(), 7.5, 0.000001);
}

CP_TEST_CASE("CaptureService setup verification passes through the fake backend") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>(
        capture_panel::fake::FakeBackendOptions{
            .sample_rate = 48'000.0,
            .latency_frames = 128,
            .loopback_gain_db = -6.0,
        });
    CaptureService service(backend, backend);

    const auto result = service.verify_setup({
        .driver_id = std::string(capture_panel::fake::loopback_device_id),
        .playback_channels = {1},
        .record_channels = {1},
    });

    CP_REQUIRE(result.passed());
    CP_REQUIRE(result.alignment.marker_latency_samples.has_value());
    CP_REQUIRE(result.alignment.marker_latency_samples.value() == 128);
    CP_REQUIRE(result.verification.sweep.has_value());
    CP_REQUIRE(result.failures.empty());
}

CP_TEST_CASE("CaptureService propagates cancellation from the fake backend") {
    TemporaryWavPair files("capture-panel-cancelled");
    write_wav(files.input, sine_source(48'000.0, 1'000), AudioBitDepth::pcm24);

    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    CaptureService service(backend, backend);
    auto cancellation = std::make_shared<CancellationToken>();
    cancellation->cancel();

    bool cancelled = false;
    try {
        static_cast<void>(service.capture(configuration_for(files), {}, cancellation));
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }
    CP_REQUIRE(cancelled);
    CP_REQUIRE(!std::filesystem::exists(files.output));
}
