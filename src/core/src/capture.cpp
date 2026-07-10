#include "capture_panel/core/capture.hpp"

#include "capture_panel/core/alignment.hpp"
#include "capture_panel/core/audio.hpp"
#include "capture_panel/core/channels.hpp"
#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/diagnostics.hpp"
#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/verification.hpp"
#include "capture_panel/core/wav.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace capture_panel {
namespace {

struct ValidatedRoute {
    AudioDevice device;
    CaptureRoute route;
};

struct PassSource {
    CaptureInputInfo input;
    AudioBuffer audio;
    ValidatedRoute validated_route;
    std::vector<CaptureWarning> warnings;
};

struct PassExecution {
    CaptureInputInfo input;
    RawAudioCaptureResult raw;
    PayloadAlignment alignment;
    std::chrono::duration<double> elapsed{};
};

struct InputPeakLevels {
    double adjusted_dbfs;
    double clipping_dbfs;
};

[[nodiscard]] double finite_or_zero(double value) noexcept {
    return std::isfinite(value) ? value : 0.0;
}

[[nodiscard]] CapturePassOptions sanitized_options(const CapturePassOptions& options) noexcept {
    return {
        .playback_gain_db = finite_or_zero(options.playback_gain_db),
        .recording_gain_db = finite_or_zero(options.recording_gain_db),
    };
}

[[nodiscard]] InputPeakLevels input_peak_levels(
    const std::span<const float> recorded_samples,
    const double recording_gain_db) noexcept {
    const auto raw_peak = peak(recorded_samples);
    auto recording_gain = linear_from_dbfs(recording_gain_db);
    if (!std::isfinite(recording_gain)) recording_gain = 1.0;

    const auto raw_dbfs = dbfs_from_peak(raw_peak);
    const auto adjusted_dbfs = dbfs_from_peak(raw_peak * recording_gain);
    return {
        .adjusted_dbfs = adjusted_dbfs,
        .clipping_dbfs = std::max(raw_dbfs, adjusted_dbfs),
    };
}

void append_unique(std::vector<CaptureWarning>& destination, CaptureWarning warning) {
    if (std::find(destination.begin(), destination.end(), warning) == destination.end()) {
        destination.push_back(warning);
    }
}

class SampleRateRestore final {
public:
    SampleRateRestore(
        IAudioDeviceProvider& provider,
        std::string device_id,
        double original_sample_rate,
        double configured_sample_rate)
        : provider_(&provider),
          device_id_(std::move(device_id)),
          original_sample_rate_(original_sample_rate),
          should_restore_(
              std::isfinite(original_sample_rate)
              && original_sample_rate > 0.0
              && std::isfinite(configured_sample_rate)
              && configured_sample_rate > 0.0
              && std::abs(original_sample_rate - configured_sample_rate) > 0.5) {}

    SampleRateRestore(const SampleRateRestore&) = delete;
    SampleRateRestore& operator=(const SampleRateRestore&) = delete;

    SampleRateRestore(SampleRateRestore&& other) noexcept
        : provider_(std::exchange(other.provider_, nullptr)),
          device_id_(std::move(other.device_id_)),
          original_sample_rate_(other.original_sample_rate_),
          should_restore_(other.should_restore_) {}

    ~SampleRateRestore() {
        restore();
    }

    void restore() noexcept {
        if (provider_ == nullptr || !should_restore_) return;
        try {
            provider_->set_sample_rate(device_id_, original_sample_rate_);
        } catch (...) {
            // A completed capture must not be replaced by a best-effort restore error.
        }
        provider_ = nullptr;
    }

private:
    IAudioDeviceProvider* provider_ = nullptr;
    std::string device_id_;
    double original_sample_rate_ = 0.0;
    bool should_restore_ = false;
};

[[nodiscard]] ValidatedRoute validate_route(
    IAudioDeviceProvider& provider,
    const CaptureRoute& route) {
    const auto device = provider.device(route.driver_id);
    if (!device.available) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "Audio driver is unavailable: " + device.name
                + (device.status.empty() ? std::string{} : " (" + device.status + ')'));
    }

    validate_playback_channels(route.playback_channels, device.output_channels);
    validate_record_channels(route.record_channels, device.input_channels);
    return {.device = device, .route = route};
}

[[nodiscard]] std::string device_event_message(const ValidatedRoute& route) {
    std::ostringstream message;
    message << route.device.name << " (" << route.device.id << "), outputs ";
    for (std::size_t index = 0; index < route.route.playback_channels.size(); ++index) {
        if (index > 0) message << ',';
        message << route.route.playback_channels[index];
    }
    message << ", inputs ";
    for (std::size_t index = 0; index < route.route.record_channels.size(); ++index) {
        if (index > 0) message << ',';
        message << route.route.record_channels[index];
    }
    return message.str();
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::string warning_event_message(
    CaptureWarning warning,
    const PassSource* source = nullptr) {
    if (warning == CaptureWarning::source_channel_count_mismatch && source != nullptr) {
        std::ostringstream message;
        message << "The source has " << source->audio.channel_count
                << " channel(s), but the playback route has "
                << source->validated_route.route.playback_channels.size()
                << "; channels are mapped in order.";
        return message.str();
    }
    return std::string(warning_message(warning));
}

} // namespace

CaptureService::CaptureService(
    std::shared_ptr<IAudioDeviceProvider> device_provider,
    std::shared_ptr<IAudioCaptureBackend> capture_backend,
    CaptureEventHandler event_handler)
    : device_provider_(std::move(device_provider)),
      capture_backend_(std::move(capture_backend)),
      event_handler_(std::move(event_handler)) {
    if (!device_provider_ || !capture_backend_) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "CaptureService requires a device provider and a capture backend.");
    }
}

CapturePassResult CaptureService::capture(
    const CaptureConfiguration& configuration,
    const CapturePassOptions& raw_options,
    std::shared_ptr<CancellationToken> cancellation) {
    const auto emit = [this](CaptureEvent event) {
        if (event_handler_) event_handler_(event);
    };

    emit({.type = CaptureEventType::started, .message = path_utf8(configuration.input_path)});
    const auto wav = read_wav(configuration.input_path);
    auto source = PassSource{
        .input = {.path = configuration.input_path, .format = wav.format},
        .audio = wav.audio,
        .validated_route = validate_route(*device_provider_, configuration.route),
        .warnings = {},
    };

    if (source.audio.channel_count != configuration.route.playback_channels.size()) {
        source.warnings.push_back(CaptureWarning::source_channel_count_mismatch);
    }
    if (peak(source.audio.samples) >= 0.999) {
        source.warnings.push_back(CaptureWarning::source_near_digital_full_scale);
    }

    emit({
        .type = CaptureEventType::input_loaded,
        .input = source.input,
        .message = path_utf8(configuration.input_path),
    });
    for (const auto warning : source.warnings) {
        emit({
            .type = CaptureEventType::warning,
            .warning = warning,
            .message = warning_event_message(warning, &source),
        });
    }
    emit({
        .type = CaptureEventType::devices_validated,
        .device = source.validated_route.device,
        .route = source.validated_route.route,
        .message = device_event_message(source.validated_route),
    });

    const auto options = sanitized_options(raw_options);
    emit({
        .type = CaptureEventType::stage_changed,
        .stage = CaptureStage::sample_rate_configuration,
        .sample_rate = source.input.format.sample_rate,
    });
    SampleRateRestore restore(
        *device_provider_,
        source.validated_route.device.id,
        source.validated_route.device.sample_rate,
        source.input.format.sample_rate);
    device_provider_->set_sample_rate(
        source.validated_route.device.id,
        source.input.format.sample_rate);

    const auto playback_gain = static_cast<float>(linear_from_dbfs(options.playback_gain_db));
    const auto plan = make_alignment_playback_plan(source.audio, playback_gain);
    emit({
        .type = CaptureEventType::stage_changed,
        .stage = CaptureStage::recording,
        .total_frames = plan.playback_frame_count,
        .padding_seconds = constants::alignment::padding_seconds,
        .marker_to_payload_silence_seconds =
            constants::alignment::marker_to_payload_silence_seconds,
    });

    const auto started = std::chrono::steady_clock::now();
    auto raw = capture_backend_->capture({
        .route = configuration.route,
        .playback = plan.audio,
        .padding_seconds = constants::alignment::padding_seconds,
        .cancellation = std::move(cancellation),
        .progress = [emit, sample_rate = source.input.format.sample_rate](
                        std::int64_t completed,
                        std::int64_t total) {
            emit({
                .type = CaptureEventType::recording_progress,
                .progress = CaptureProgress{
                    .completed_frames = completed,
                    .total_frames = total,
                    .sample_rate = sample_rate,
                },
            });
        },
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    restore.restore();
    emit({
        .type = CaptureEventType::capture_finished,
        .elapsed_seconds = std::chrono::duration<double>(elapsed).count(),
        .message = std::to_string(std::chrono::duration<double>(elapsed).count()),
    });

    emit({.type = CaptureEventType::stage_changed, .stage = CaptureStage::alignment});
    apply_gain_db(raw.recorded.samples, options.recording_gain_db);
    auto alignment = align_payload(raw.recorded, {
        .expected_marker_frames = [&] {
            auto frames = plan.marker_frames;
            for (auto& frame : frames) frame += raw.pre_pad_frames;
            return frames;
        }(),
        .source_start_frame = raw.pre_pad_frames + plan.source_start_frame,
        .source_frame_count = source.input.format.total_frames,
    });

    if (alignment.impulse_detection) {
        emit({
            .type = CaptureEventType::impulse_detection,
            .impulse_detection = *alignment.impulse_detection,
        });
    }
    for (const auto warning : alignment.warnings) {
        emit({
            .type = CaptureEventType::warning,
            .warning = warning,
            .message = warning_event_message(warning),
        });
    }
    emit({
        .type = CaptureEventType::alignment_finished,
        .alignment = alignment.info,
    });

    emit({.type = CaptureEventType::stage_changed, .stage = CaptureStage::output_writing});
    const auto bit_depth = configuration.output_bit_depth.value_or(source.input.format.bit_depth);
    write_wav(configuration.output_path, alignment.audio, bit_depth);
    CaptureOutputInfo output{
        .path = configuration.output_path,
        .file_size = std::filesystem::file_size(configuration.output_path),
        .sample_rate = alignment.audio.sample_rate,
        .channel_count = alignment.audio.channel_count,
        .bit_depth = bit_depth,
    };
    emit({
        .type = CaptureEventType::output_written,
        .output = output,
        .message = path_utf8(configuration.output_path),
    });

    return {
        .input = source.input,
        .output = std::move(output),
        .alignment = alignment.info,
        .elapsed = std::chrono::duration<double>(elapsed),
    };
}

CaptureVerificationResult CaptureService::verify_setup(
    const CaptureRoute& route,
    std::optional<double> requested_sample_rate,
    double output_trim_db,
    double input_trim_db,
    const CapturePassOptions& raw_options,
    std::shared_ptr<CancellationToken> cancellation) {
    const auto emit = [this](CaptureEvent event) {
        if (event_handler_) event_handler_(event);
    };
    const auto validated = validate_route(*device_provider_, route);
    const auto sample_rate = requested_sample_rate.value_or(
        validated.device.sample_rate > 0.0
            ? validated.device.sample_rate
            : constants::audio::fallback_sample_rate);
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        throw CaptureError(ErrorCode::unsupported_sample_rate, "Sample rate must be positive.");
    }

    emit({
        .type = CaptureEventType::devices_validated,
        .device = validated.device,
        .route = validated.route,
        .message = device_event_message(validated),
    });
    const auto signal = make_verification_signal(
        sample_rate,
        static_cast<std::uint32_t>(std::max<std::size_t>(1, route.playback_channels.size())));
    const CaptureInputInfo input{
        .path = "alignment-verification-signal.wav",
        .format = signal.format,
    };
    emit({
        .type = CaptureEventType::input_loaded,
        .input = input,
        .message = path_utf8(input.path),
    });

    const auto options = sanitized_options({
        .playback_gain_db = raw_options.playback_gain_db + finite_or_zero(output_trim_db),
        .recording_gain_db = raw_options.recording_gain_db + finite_or_zero(input_trim_db),
    });

    emit({
        .type = CaptureEventType::stage_changed,
        .stage = CaptureStage::sample_rate_configuration,
        .sample_rate = sample_rate,
    });
    SampleRateRestore restore(
        *device_provider_,
        validated.device.id,
        validated.device.sample_rate,
        sample_rate);
    device_provider_->set_sample_rate(validated.device.id, sample_rate);

    const auto playback_gain = static_cast<float>(linear_from_dbfs(options.playback_gain_db));
    const auto plan = make_alignment_playback_plan(signal.audio, playback_gain);
    emit({
        .type = CaptureEventType::stage_changed,
        .stage = CaptureStage::recording,
        .total_frames = plan.playback_frame_count,
        .padding_seconds = constants::alignment::padding_seconds,
        .marker_to_payload_silence_seconds =
            constants::alignment::marker_to_payload_silence_seconds,
    });
    const auto started = std::chrono::steady_clock::now();
    auto raw = capture_backend_->capture({
        .route = route,
        .playback = plan.audio,
        .padding_seconds = constants::alignment::padding_seconds,
        .cancellation = std::move(cancellation),
        .progress = [emit, sample_rate](std::int64_t completed, std::int64_t total) {
            emit({
                .type = CaptureEventType::recording_progress,
                .progress = CaptureProgress{
                    .completed_frames = completed,
                    .total_frames = total,
                    .sample_rate = sample_rate,
                },
            });
        },
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    restore.restore();
    emit({
        .type = CaptureEventType::capture_finished,
        .elapsed_seconds = std::chrono::duration<double>(elapsed).count(),
    });

    const auto input_peaks = input_peak_levels(
        raw.recorded.samples,
        options.recording_gain_db);
    emit({.type = CaptureEventType::stage_changed, .stage = CaptureStage::alignment});
    apply_gain_db(raw.recorded.samples, options.recording_gain_db);
    auto alignment = align_payload(raw.recorded, {
        .expected_marker_frames = [&] {
            auto frames = plan.marker_frames;
            for (auto& frame : frames) frame += raw.pre_pad_frames;
            return frames;
        }(),
        .source_start_frame = raw.pre_pad_frames + plan.source_start_frame,
        .source_frame_count = signal.audio.frame_count(),
    });
    if (alignment.impulse_detection) {
        emit({
            .type = CaptureEventType::impulse_detection,
            .impulse_detection = *alignment.impulse_detection,
        });
    }
    for (const auto warning : alignment.warnings) {
        emit({
            .type = CaptureEventType::warning,
            .warning = warning,
            .message = warning_event_message(warning),
        });
    }
    emit({
        .type = CaptureEventType::alignment_finished,
        .alignment = alignment.info,
    });

    emit({.type = CaptureEventType::stage_changed, .stage = CaptureStage::verification});
    auto verification = evaluate_verification(
        alignment.audio,
        signal,
        alignment.info,
        input_peaks.clipping_dbfs);
    for (const auto warning : verification.warnings) {
        emit({
            .type = CaptureEventType::warning,
            .warning = warning,
            .message = warning_event_message(warning),
        });
    }
    emit({
        .type = CaptureEventType::verification_finished,
        .verification = verification,
    });

    auto warnings = alignment.warnings;
    for (const auto warning : verification.warnings) append_unique(warnings, warning);
    auto failures = verification.failures;

    const auto marker_peak = std::abs(
        linear_from_dbfs(constants::alignment::impulse_level_dbfs)
        * static_cast<double>(playback_gain));
    const auto payload_peak = peak(signal.audio.samples) * std::abs(playback_gain);

    return {
        .input = input,
        .alignment = alignment.info,
        .verification = std::move(verification),
        .impulse_detection = std::move(alignment.impulse_detection),
        .output_peak_dbfs = dbfs_from_peak(std::max(marker_peak, payload_peak)),
        .input_peak_dbfs = input_peaks.adjusted_dbfs,
        .sample_rate = sample_rate,
        .warnings = std::move(warnings),
        .failures = std::move(failures),
        .elapsed = std::chrono::duration<double>(elapsed),
    };
}

} // namespace capture_panel
