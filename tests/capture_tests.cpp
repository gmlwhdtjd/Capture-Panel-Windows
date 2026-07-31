#include "test_framework.hpp"

#include "capture_panel/core/capture.hpp"
#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/wav.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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

class PartialSampleRateFailureBackend final
    : public IAudioDeviceProvider,
      public IAudioCaptureBackend {
public:
    [[nodiscard]] std::vector<AudioDevice> devices() const override {
        return {device("partial:sample-rate")};
    }

    [[nodiscard]] AudioDevice device(const std::string& id) const override {
        validate_id(id);
        return {
            .id = "partial:sample-rate",
            .name = "Partial sample-rate backend",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = sample_rate_,
            .available = true,
        };
    }

    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string& id,
        ChannelDirection) const override {
        validate_id(id);
        return {{.index = 1, .name = "Channel 1"}};
    }

    void set_sample_rate(const std::string& id, double sample_rate) override {
        validate_id(id);
        configured_rates.push_back(sample_rate);
        sample_rate_ = sample_rate;
        if (std::abs(sample_rate - 44'100.0) <= 0.5) {
            throw CaptureError(
                ErrorCode::backend_failure,
                "The device changed rate before reporting failure.");
        }
    }

    RawAudioCaptureResult capture(const RawAudioCaptureRequest&) override {
        capture_called = true;
        return {};
    }

    double sample_rate_ = 48'000.0;
    std::vector<double> configured_rates;
    bool capture_called = false;

private:
    static void validate_id(const std::string& id) {
        if (id != "partial:sample-rate") {
            throw CaptureError(ErrorCode::device_not_found, "Audio driver not found: " + id);
        }
    }
};

enum class RawResultDefect {
    wrong_sample_rate,
    wrong_channel_count,
    partial_frame,
    negative_pre_pad,
    too_short,
    non_finite_sample,
};

class ContractViolatingBackend final
    : public IAudioDeviceProvider,
      public IAudioCaptureBackend {
public:
    explicit ContractViolatingBackend(const RawResultDefect defect) : defect_(defect) {}

    [[nodiscard]] std::vector<AudioDevice> devices() const override {
        return {device("malformed:capture")};
    }

    [[nodiscard]] AudioDevice device(const std::string& id) const override {
        validate_id(id);
        return {
            .id = "malformed:capture",
            .name = "Malformed capture backend",
            .input_channels = 2,
            .output_channels = 1,
            .sample_rate = sample_rate_,
            .available = true,
        };
    }

    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string& id,
        const ChannelDirection direction) const override {
        validate_id(id);
        const auto count = direction == ChannelDirection::input ? 2U : 1U;
        std::vector<AudioChannel> result;
        for (std::uint32_t index = 1; index <= count; ++index) {
            result.push_back({.index = index, .name = "Channel " + std::to_string(index)});
        }
        return result;
    }

    void set_sample_rate(const std::string& id, const double sample_rate) override {
        validate_id(id);
        sample_rate_ = sample_rate;
    }

    RawAudioCaptureResult capture(const RawAudioCaptureRequest& request) override {
        const auto pre_pad = static_cast<std::int64_t>(std::llround(
            request.padding_seconds * request.playback_plan.sample_rate()));
        const auto required_frames = pre_pad
            + request.playback_plan.playback_frame_count + pre_pad;
        const auto channel_count = static_cast<std::uint32_t>(
            request.route.record_channels.size());
        AudioBuffer recorded{
            .sample_rate = request.playback_plan.sample_rate(),
            .channel_count = channel_count,
            .samples = std::vector<float>(
                static_cast<std::size_t>(required_frames) * channel_count,
                0.0F),
        };
        auto result_pre_pad = pre_pad;

        switch (defect_) {
        case RawResultDefect::wrong_sample_rate:
            recorded.sample_rate += 1'000.0;
            break;
        case RawResultDefect::wrong_channel_count:
            recorded.channel_count = 1;
            recorded.samples.resize(static_cast<std::size_t>(required_frames));
            break;
        case RawResultDefect::partial_frame:
            throw CaptureError(
                ErrorCode::backend_failure,
                "A backend cannot publish a partial frame through the streaming contract.");
        case RawResultDefect::negative_pre_pad:
            result_pre_pad = -1;
            break;
        case RawResultDefect::too_short:
            recorded.samples.resize(recorded.samples.size() - channel_count);
            break;
        case RawResultDefect::non_finite_sample:
            recorded.samples.front() = std::numeric_limits<float>::quiet_NaN();
            break;
        }
        return {
            .recorded = Float32AudioAsset::from_memory(std::move(recorded)),
            .pre_pad_frames = result_pre_pad,
        };
    }

private:
    static void validate_id(const std::string& id) {
        if (id != "malformed:capture") {
            throw CaptureError(ErrorCode::device_not_found, "Audio driver not found: " + id);
        }
    }

    RawResultDefect defect_;
    double sample_rate_ = 48'000.0;
};

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

CP_TEST_CASE("CaptureService setup verification reports the input peak after input trim") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    CaptureService service(backend, backend);

    const auto result = service.verify_setup(
        {
            .driver_id = std::string(capture_panel::fake::loopback_device_id),
            .playback_channels = {1},
            .record_channels = {1},
        },
        std::nullopt,
        0.0,
        -6.0);

    CP_REQUIRE(result.passed());
    CP_REQUIRE_NEAR(result.output_peak_dbfs, -12.0, 0.05);
    CP_REQUIRE_NEAR(result.input_peak_dbfs, -18.0, 0.05);
}

CP_TEST_CASE("CaptureService setup verification keeps raw clipping after negative input trim") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>(
        capture_panel::fake::FakeBackendOptions{
            .loopback_gain_db = 20.0 * std::log10(4.0),
        });
    CaptureService service(backend, backend);

    const auto result = service.verify_setup(
        {
            .driver_id = std::string(capture_panel::fake::loopback_device_id),
            .playback_channels = {1},
            .record_channels = {1},
        },
        std::nullopt,
        0.0,
        -12.0);

    CP_REQUIRE(!result.passed());
    CP_REQUIRE_NEAR(result.input_peak_dbfs, -12.0, 0.05);
    CP_REQUIRE(std::find(
        result.failures.begin(),
        result.failures.end(),
        CaptureFailure::digital_clipping) != result.failures.end());
}

CP_TEST_CASE("CaptureService setup verification detects clipping introduced by positive input trim") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    CaptureService service(backend, backend);

    const auto result = service.verify_setup(
        {
            .driver_id = std::string(capture_panel::fake::loopback_device_id),
            .playback_channels = {1},
            .record_channels = {1},
        },
        std::nullopt,
        0.0,
        12.0);

    CP_REQUIRE(!result.passed());
    CP_REQUIRE_NEAR(result.input_peak_dbfs, 0.0, 0.05);
    CP_REQUIRE(std::find(
        result.failures.begin(),
        result.failures.end(),
        CaptureFailure::digital_clipping) != result.failures.end());
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

CP_TEST_CASE("CaptureService observes cancellation when alignment starts") {
    TemporaryWavPair files("capture-panel-cancel-during-alignment");
    write_wav(files.input, sine_source(48'000.0, 1'000), AudioBitDepth::pcm24);

    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    auto cancellation = std::make_shared<CancellationToken>();
    bool alignment_started = false;
    bool output_writing_started = false;
    CaptureService service(backend, backend, [&](const CaptureEvent& event) {
        if (event.type != CaptureEventType::stage_changed || !event.stage) return;
        if (*event.stage == CaptureStage::alignment) {
            alignment_started = true;
            static_cast<void>(cancellation->cancel());
        } else if (*event.stage == CaptureStage::output_writing) {
            output_writing_started = true;
        }
    });

    bool cancelled = false;
    try {
        static_cast<void>(service.capture(configuration_for(files), {}, cancellation));
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }

    CP_REQUIRE(cancelled);
    CP_REQUIRE(alignment_started);
    CP_REQUIRE(!output_writing_started);
    CP_REQUIRE(!std::filesystem::exists(files.output));
}

CP_TEST_CASE("CaptureService cancellation before rate configuration has no device side effect") {
    TemporaryWavPair files("capture-panel-cancel-before-rate");
    write_wav(files.input, sine_source(44'100.0, 441), AudioBitDepth::pcm24);

    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    auto cancellation = std::make_shared<CancellationToken>();
    CaptureService service(backend, backend, [&](const CaptureEvent& event) {
        if (event.type == CaptureEventType::input_loaded) cancellation->cancel();
    });

    bool cancelled = false;
    try {
        static_cast<void>(service.capture(configuration_for(files), {}, cancellation));
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }
    CP_REQUIRE(cancelled);
    CP_REQUIRE_NEAR(
        backend->device(std::string(capture_panel::fake::loopback_device_id)).sample_rate,
        48'000.0,
        0.5);
    CP_REQUIRE(!std::filesystem::exists(files.output));
}

CP_TEST_CASE("CaptureService setup cancellation before rate configuration has no device side effect") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    auto cancellation = std::make_shared<CancellationToken>();
    CaptureService service(backend, backend, [&](const CaptureEvent& event) {
        if (event.type == CaptureEventType::devices_validated) cancellation->cancel();
    });

    bool cancelled = false;
    try {
        static_cast<void>(service.verify_setup(
            {
                .driver_id = std::string(capture_panel::fake::loopback_device_id),
                .playback_channels = {1},
                .record_channels = {1},
            },
            44'100.0,
            0.0,
            0.0,
            {},
            cancellation));
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }
    CP_REQUIRE(cancelled);
    CP_REQUIRE_NEAR(
        backend->device(std::string(capture_panel::fake::loopback_device_id)).sample_rate,
        48'000.0,
        0.5);
}

CP_TEST_CASE("CaptureService setup observes cancellation when alignment starts") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    auto cancellation = std::make_shared<CancellationToken>();
    bool alignment_started = false;
    bool verification_started = false;
    CaptureService service(backend, backend, [&](const CaptureEvent& event) {
        if (event.type != CaptureEventType::stage_changed || !event.stage) return;
        if (*event.stage == CaptureStage::alignment) {
            alignment_started = true;
            static_cast<void>(cancellation->cancel());
        } else if (*event.stage == CaptureStage::verification) {
            verification_started = true;
        }
    });

    bool cancelled = false;
    try {
        static_cast<void>(service.verify_setup(
            {
                .driver_id = std::string(capture_panel::fake::loopback_device_id),
                .playback_channels = {1},
                .record_channels = {1},
            },
            std::nullopt,
            0.0,
            0.0,
            {},
            cancellation));
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }

    CP_REQUIRE(cancelled);
    CP_REQUIRE(alignment_started);
    CP_REQUIRE(!verification_started);
}

CP_TEST_CASE("CaptureService rejects a zero-frame source before capture") {
    TemporaryWavPair files("capture-panel-empty-source");
    write_wav(
        files.input,
        AudioBuffer{.sample_rate = 48'000.0, .channel_count = 1, .samples = {}},
        AudioBitDepth::pcm24);

    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    CaptureService service(backend, backend);
    bool failed = false;
    try {
        static_cast<void>(service.capture(configuration_for(files)));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::validation_failed;
    }

    CP_REQUIRE(failed);
    CP_REQUIRE(!std::filesystem::exists(files.output));
}

CP_TEST_CASE("CaptureService rejects duplicate routes before changing the sample rate") {
    TemporaryWavPair files("capture-panel-duplicate-route");
    write_wav(files.input, sine_source(44'100.0, 441), AudioBitDepth::pcm24);

    for (const auto duplicate_playback : {true, false}) {
        auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
        CaptureService service(backend, backend);
        auto configuration = configuration_for(files);
        configuration.route.playback_channels = duplicate_playback
            ? std::vector<std::uint32_t>{1, 1}
            : std::vector<std::uint32_t>{1};
        configuration.route.record_channels = duplicate_playback
            ? std::vector<std::uint32_t>{1}
            : std::vector<std::uint32_t>{1, 1};

        bool failed = false;
        try {
            static_cast<void>(service.capture(configuration));
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::validation_failed;
        }
        CP_REQUIRE(failed);
        CP_REQUIRE_NEAR(
            backend->device(std::string(capture_panel::fake::loopback_device_id)).sample_rate,
            48'000.0,
            0.01);
        CP_REQUIRE(!std::filesystem::exists(files.output));
    }
}

CP_TEST_CASE("CaptureService rejects every malformed backend capture contract") {
    const CaptureRoute route{
        .driver_id = "malformed:capture",
        .playback_channels = {1},
        .record_channels = {1, 2},
    };
    for (const auto defect : {
             RawResultDefect::wrong_sample_rate,
             RawResultDefect::wrong_channel_count,
             RawResultDefect::partial_frame,
             RawResultDefect::negative_pre_pad,
             RawResultDefect::too_short,
             RawResultDefect::non_finite_sample}) {
        auto backend = std::make_shared<ContractViolatingBackend>(defect);
        CaptureService service(backend, backend);
        bool failed = false;
        try {
            static_cast<void>(service.verify_setup(route));
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::backend_failure;
        }
        CP_REQUIRE(failed);
    }
}

CP_TEST_CASE("CaptureService rejects gains outside the desktop control ranges") {
    auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
    CaptureService service(backend, backend);
    const CaptureRoute route{
        .driver_id = std::string(capture_panel::fake::loopback_device_id),
        .playback_channels = {1},
        .record_channels = {1},
    };
    for (const auto [output_gain, input_gain] : std::vector<std::pair<double, double>>{
             {-24.01, 0.0},
             {0.01, 0.0},
             {0.0, -18.01},
             {0.0, 12.01},
             {std::numeric_limits<double>::infinity(), 0.0},
             {0.0, std::numeric_limits<double>::quiet_NaN()}}) {
        bool failed = false;
        try {
            static_cast<void>(service.verify_setup(
                route, std::nullopt, output_gain, input_gain));
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::validation_failed;
        }
        CP_REQUIRE(failed);
    }

    TemporaryWavPair files("capture-panel-invalid-gain");
    bool capture_failed = false;
    try {
        static_cast<void>(service.capture(
            configuration_for(files),
            CapturePassOptions{.playback_gain_db = 0.5}));
    } catch (const CaptureError& error) {
        capture_failed = error.code() == ErrorCode::validation_failed;
    }
    CP_REQUIRE(capture_failed);
}

CP_TEST_CASE("CaptureService rejects same-file paths hard links and case aliases") {
    enum class AliasKind { same_path, hard_link, case_variant };
    for (const auto alias_kind : {
             AliasKind::same_path,
             AliasKind::hard_link,
             AliasKind::case_variant}) {
        TemporaryWavPair files(
            alias_kind == AliasKind::hard_link
                ? "capture-panel-hard-link-input"
                : alias_kind == AliasKind::case_variant
                    ? "capture-panel-case-input"
                    : "capture-panel-same-input");
        const auto source = sine_source(48'000.0, 64);
        write_wav(files.input, source, AudioBitDepth::pcm24);
        if (alias_kind == AliasKind::hard_link) {
            std::error_code link_error;
            std::filesystem::create_hard_link(files.input, files.output, link_error);
            CP_REQUIRE(!link_error);
        }

        auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
        CaptureService service(backend, backend);
        auto configuration = configuration_for(files);
        if (alias_kind == AliasKind::same_path) {
            configuration.output_path = configuration.input_path;
        } else if (alias_kind == AliasKind::case_variant) {
            auto case_alias = configuration.input_path.native();
            std::transform(
                case_alias.begin(), case_alias.end(), case_alias.begin(),
                [](const auto character) {
                    return static_cast<std::filesystem::path::value_type>(
                        std::towupper(static_cast<std::wint_t>(character)));
                });
            configuration.output_path = std::filesystem::path(case_alias);
        }

        bool failed = false;
        try {
            static_cast<void>(service.capture(configuration));
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::validation_failed;
        }
        CP_REQUIRE(failed);

        const auto preserved = read_wav(files.input);
        CP_REQUIRE(preserved.audio.samples.size() == source.samples.size());
        CP_REQUIRE_NEAR(preserved.audio.samples[7], source.samples[7], 0.00001);
    }
}

CP_TEST_CASE("CaptureService restores a partially changed rate when configuration fails") {
    TemporaryWavPair files("capture-panel-partial-rate-failure");
    write_wav(files.input, sine_source(44'100.0, 441), AudioBitDepth::pcm24);

    auto backend = std::make_shared<PartialSampleRateFailureBackend>();
    CaptureService service(backend, backend);
    bool failed = false;
    try {
        static_cast<void>(service.capture({
            .input_path = files.input,
            .output_path = files.output,
            .route = {
                .driver_id = "partial:sample-rate",
                .playback_channels = {1},
                .record_channels = {1},
            },
        }));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::backend_failure;
    }

    CP_REQUIRE(failed);
    CP_REQUIRE(!backend->capture_called);
    CP_REQUIRE(backend->configured_rates.size() == 2);
    CP_REQUIRE_NEAR(backend->configured_rates[0], 44'100.0, 0.01);
    CP_REQUIRE_NEAR(backend->configured_rates[1], 48'000.0, 0.01);
    CP_REQUIRE_NEAR(backend->sample_rate_, 48'000.0, 0.01);
}

CP_TEST_CASE("CaptureService setup verification restores rate after configuration failure") {
    auto backend = std::make_shared<PartialSampleRateFailureBackend>();
    CaptureService service(backend, backend);
    bool failed = false;
    try {
        static_cast<void>(service.verify_setup(
            {
                .driver_id = "partial:sample-rate",
                .playback_channels = {1},
                .record_channels = {1},
            },
            44'100.0));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::backend_failure;
    }

    CP_REQUIRE(failed);
    CP_REQUIRE(!backend->capture_called);
    CP_REQUIRE(backend->configured_rates.size() == 2);
    CP_REQUIRE_NEAR(backend->sample_rate_, 48'000.0, 0.01);
}
