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
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
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
    CaptureAudioSource audio;
    ValidatedRoute validated_route;
    std::vector<CaptureWarning> warnings;
};

struct InputPeakLevels {
    double adjusted_dbfs;
    double clipping_dbfs;
};

[[noreturn]] void throw_gain_error(
    const std::string& label,
    const double minimum,
    const double maximum) {
    throw CaptureError(
        ErrorCode::validation_failed,
        label + " must be between " + std::to_string(minimum)
            + " and " + std::to_string(maximum) + " dB.");
}

[[nodiscard]] double validated_gain(
    const double value,
    const double minimum,
    const double maximum,
    const std::string& label) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        throw_gain_error(label, minimum, maximum);
    }
    return value;
}

[[nodiscard]] CapturePassOptions validated_options(const CapturePassOptions& options) {
    return {
        .playback_gain_db = validated_gain(
            options.playback_gain_db,
            constants::gain::output_minimum_db,
            constants::gain::output_maximum_db,
            "Output gain"),
        .recording_gain_db = validated_gain(
            options.recording_gain_db,
            constants::gain::input_minimum_db,
            constants::gain::input_maximum_db,
            "Input gain"),
    };
}

[[nodiscard]] CapturePassOptions validated_verification_options(
    const CapturePassOptions& raw_options,
    const double output_trim_db,
    const double input_trim_db) {
    const auto base = validated_options(raw_options);
    const auto output_trim = validated_gain(
        output_trim_db,
        constants::gain::output_minimum_db,
        constants::gain::output_maximum_db,
        "Output trim");
    const auto input_trim = validated_gain(
        input_trim_db,
        constants::gain::input_minimum_db,
        constants::gain::input_maximum_db,
        "Input trim");
    return validated_options({
        .playback_gain_db = base.playback_gain_db + output_trim,
        .recording_gain_db = base.recording_gain_db + input_trim,
    });
}

void validate_supported_sample_rate(const double sample_rate) {
    if (!std::isfinite(sample_rate)
        || sample_rate < constants::audio::minimum_supported_sample_rate
        || sample_rate > constants::audio::maximum_supported_sample_rate) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "Sample rate must be between "
                + std::to_string(constants::audio::minimum_supported_sample_rate)
                + " and "
                + std::to_string(constants::audio::maximum_supported_sample_rate)
                + " Hz.");
    }
}

void validate_capture_source(const WavFormat& format) {
    if (format.total_frames <= 0 || format.channel_count == 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The source WAV must contain at least one audio frame.");
    }
}

void throw_if_cancelled(const std::shared_ptr<CancellationToken>& cancellation) {
    if (cancellation && cancellation->is_cancelled()) {
        throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
    }
}

[[nodiscard]] bool paths_refer_to_same_file(
    const std::filesystem::path& input,
    const std::filesystem::path& output) noexcept {
    if (input == output) return true;

    std::error_code equivalent_error;
    if (std::filesystem::equivalent(input, output, equivalent_error)
        && !equivalent_error) {
        return true;
    }

    std::error_code input_error;
    std::error_code output_error;
    const auto canonical_input = std::filesystem::weakly_canonical(input, input_error);
    const auto canonical_output = std::filesystem::weakly_canonical(output, output_error);
    return !input_error && !output_error && canonical_input == canonical_output;
}

void validate_distinct_paths(const CaptureConfiguration& configuration) {
    if (paths_refer_to_same_file(configuration.input_path, configuration.output_path)) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Input and output paths must refer to different files.");
    }
}

[[nodiscard]] std::int64_t checked_padding_frames(
    const double padding_seconds,
    const double sample_rate) {
    const auto frames = padding_seconds * sample_rate;
    if (!std::isfinite(frames) || frames < 0.0
        || frames >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw CaptureError(ErrorCode::validation_failed, "Capture padding is too large.");
    }
    return static_cast<std::int64_t>(std::llround(frames));
}

[[noreturn]] void throw_invalid_backend_result(const std::string& detail) {
    throw CaptureError(
        ErrorCode::backend_failure,
        "Audio backend returned an invalid capture result: " + detail);
}

[[nodiscard]] float validate_raw_capture_result(
    const RawAudioCaptureResult& raw,
    const CapturePassPlaybackPlan& playback,
    const CaptureRoute& route,
    const double padding_seconds,
    const std::shared_ptr<CancellationToken>& cancellation) {
    if (!raw.recorded.valid()
        || !std::isfinite(raw.recorded.sample_rate())
        || std::abs(raw.recorded.sample_rate() - playback.sample_rate()) > 0.5) {
        throw_invalid_backend_result("recorded sample rate does not match playback");
    }
    if (route.record_channels.size() > std::numeric_limits<std::uint32_t>::max()
        || raw.recorded.channel_count()
            != static_cast<std::uint32_t>(route.record_channels.size())) {
        throw_invalid_backend_result("recorded channel count does not match the route");
    }
    if (raw.pre_pad_frames < 0) {
        throw_invalid_backend_result("pre-pad frame count is negative");
    }
    const auto expected_pre_pad = checked_padding_frames(
        padding_seconds, playback.sample_rate());
    if (raw.pre_pad_frames != expected_pre_pad) {
        throw_invalid_backend_result("pre-pad frame count does not match the request");
    }
    const auto playback_frames = playback.playback_frame_count;
    if (playback_frames < 0
        || raw.pre_pad_frames
            > (std::numeric_limits<std::int64_t>::max() - playback_frames) / 2) {
        throw_invalid_backend_result("required capture length overflows");
    }
    const auto required_frames = raw.pre_pad_frames + playback_frames + raw.pre_pad_frames;
    if (raw.recorded.frame_count() < required_frames) {
        throw_invalid_backend_result("recording ended before all requested frames were captured");
    }

    if (raw.recorded.path().has_value()) {
        const auto channels = raw.recorded.channel_count();
        const auto frames = static_cast<std::uint64_t>(raw.recorded.frame_count());
        if (frames > std::numeric_limits<std::uintmax_t>::max()
                / channels / sizeof(float)) {
            throw_invalid_backend_result("recorded file dimensions overflow");
        }
        const auto expected_bytes = static_cast<std::uintmax_t>(frames)
            * channels * sizeof(float);
        std::error_code size_error;
        const auto actual_bytes = std::filesystem::file_size(
            *raw.recorded.path(), size_error);
        if (size_error || actual_bytes != expected_bytes) {
            throw_invalid_backend_result(
                "recorded file size does not match its declared frame count");
        }
        // File-backed capture workers calculate this peak while draining the
        // recording ring. Re-reading multi-gigabyte scratch audio here would
        // double disk I/O and delay alignment without strengthening the ASIO
        // converter contract; exact size and finite peak metadata are checked
        // at this boundary.
        if (raw.recorded.raw_peak().has_value()) {
            return *raw.recorded.raw_peak();
        }
    }
    return raw.recorded.validated_peak_level(default_audio_chunk_frames, cancellation);
}

[[nodiscard]] InputPeakLevels input_peak_levels(
    const float raw_peak,
    const double recording_gain_db) noexcept {
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
        message << "The source has " << source->input.format.channel_count
                << " channel(s), but the playback route has "
                << source->validated_route.route.playback_channels.size()
                << "; channels are mapped in order.";
        return message.str();
    }
    return std::string(warning_message(warning));
}

[[nodiscard]] std::filesystem::path scratch_file_prefix(
    const std::filesystem::path& output_path) {
    auto prefix = output_path;
    prefix += ".capture-panel.tmp.";
    return prefix;
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
    validate_distinct_paths(configuration);
    throw_if_cancelled(cancellation);
    const auto options = validated_options(raw_options);
    const auto emit = [this](CaptureEvent event) {
        if (event_handler_) event_handler_(event);
    };

    emit({.type = CaptureEventType::started, .message = path_utf8(configuration.input_path)});
    const auto format = read_wav_format(configuration.input_path);
    validate_capture_source(format);
    validate_supported_sample_rate(format.sample_rate);
    if (configuration.route.record_channels.size()
        > std::numeric_limits<std::uint32_t>::max()) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The recording route has too many channels for WAV output.");
    }
    const auto bit_depth = configuration.output_bit_depth.value_or(format.bit_depth);
    validate_wav_output_capacity(
        format.total_frames,
        format.sample_rate,
        static_cast<std::uint32_t>(configuration.route.record_channels.size()),
        bit_depth);
    auto captured_source = CaptureAudioSource::from_wav(configuration.input_path, format);
    const auto source_peak = captured_source.validated_peak_level(
        default_audio_chunk_frames, cancellation);
    throw_if_cancelled(cancellation);
    auto source = PassSource{
        .input = {.path = configuration.input_path, .format = format},
        .audio = std::move(captured_source),
        .validated_route = validate_route(*device_provider_, configuration.route),
        .warnings = {},
    };
    source.audio.validate_identity();

    if (source.input.format.channel_count != configuration.route.playback_channels.size()) {
        source.warnings.push_back(CaptureWarning::source_channel_count_mismatch);
    }
    if (source_peak >= 0.999F) {
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

    emit({
        .type = CaptureEventType::stage_changed,
        .stage = CaptureStage::sample_rate_configuration,
        .sample_rate = source.input.format.sample_rate,
    });
    throw_if_cancelled(cancellation);
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
    source.audio.validate_identity();
    RawAudioCaptureResult raw;
    try {
        raw = capture_backend_->capture({
            .route = configuration.route,
            .playback_plan = plan,
            .padding_seconds = constants::alignment::padding_seconds,
            .scratch_file_prefix = scratch_file_prefix(configuration.output_path),
            .cancellation = cancellation,
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
    } catch (...) {
        // Cancellation is user intent and takes precedence over a concurrent
        // driver/stream failure observed while the backend is unwinding.
        throw_if_cancelled(cancellation);
        throw;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    restore.restore();
    throw_if_cancelled(cancellation);
    source.audio.validate_identity();
    static_cast<void>(validate_raw_capture_result(
        raw,
        plan,
        configuration.route,
        constants::alignment::padding_seconds,
        cancellation));
    emit({
        .type = CaptureEventType::capture_finished,
        .elapsed_seconds = std::chrono::duration<double>(elapsed).count(),
        .message = std::to_string(std::chrono::duration<double>(elapsed).count()),
    });

    emit({.type = CaptureEventType::stage_changed, .stage = CaptureStage::alignment});
    auto alignment = align_payload(
        raw.recorded,
        static_cast<float>(linear_from_dbfs(options.recording_gain_db)),
        {
            .expected_marker_frames = [&] {
                auto frames = plan.marker_frames;
                for (auto& frame : frames) frame += raw.pre_pad_frames;
                return frames;
            }(),
            .source_start_frame = raw.pre_pad_frames + plan.source_start_frame,
            .source_frame_count = source.input.format.total_frames,
        },
        cancellation);

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
    throw_if_cancelled(cancellation);
    source.audio.validate_identity();
    auto aligned_reader = alignment.payload.make_reader();
    write_wav(
        configuration.output_path,
        *aligned_reader,
        alignment.payload.frame_count,
        source.input.format.sample_rate,
        alignment.payload.channel_count(),
        bit_depth,
        cancellation);
    CaptureOutputInfo output{
        .path = configuration.output_path,
        .file_size = std::filesystem::file_size(configuration.output_path),
        .sample_rate = source.input.format.sample_rate,
        .channel_count = alignment.payload.channel_count(),
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
    throw_if_cancelled(cancellation);
    const auto validated = validate_route(*device_provider_, route);
    const auto sample_rate = requested_sample_rate.value_or(
        validated.device.sample_rate > 0.0
            ? validated.device.sample_rate
            : constants::audio::fallback_sample_rate);
    validate_supported_sample_rate(sample_rate);
    throw_if_cancelled(cancellation);
    const auto options = validated_verification_options(
        raw_options, output_trim_db, input_trim_db);

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

    emit({
        .type = CaptureEventType::stage_changed,
        .stage = CaptureStage::sample_rate_configuration,
        .sample_rate = sample_rate,
    });
    throw_if_cancelled(cancellation);
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
    RawAudioCaptureResult raw;
    try {
        raw = capture_backend_->capture({
            .route = route,
            .playback_plan = plan,
            .padding_seconds = constants::alignment::padding_seconds,
            .scratch_file_prefix = std::nullopt,
            .cancellation = cancellation,
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
    } catch (...) {
        throw_if_cancelled(cancellation);
        throw;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    restore.restore();
    throw_if_cancelled(cancellation);
    const auto raw_peak = validate_raw_capture_result(
        raw,
        plan,
        route,
        constants::alignment::padding_seconds,
        cancellation);
    emit({
        .type = CaptureEventType::capture_finished,
        .elapsed_seconds = std::chrono::duration<double>(elapsed).count(),
    });

    const auto input_peaks = input_peak_levels(
        raw_peak,
        options.recording_gain_db);
    emit({.type = CaptureEventType::stage_changed, .stage = CaptureStage::alignment});
    auto alignment = align_payload(
        raw.recorded,
        static_cast<float>(linear_from_dbfs(options.recording_gain_db)),
        {
            .expected_marker_frames = [&] {
                auto frames = plan.marker_frames;
                for (auto& frame : frames) frame += raw.pre_pad_frames;
                return frames;
            }(),
            .source_start_frame = raw.pre_pad_frames + plan.source_start_frame,
            .source_frame_count = signal.audio.frame_count(),
        },
        cancellation);
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
    throw_if_cancelled(cancellation);
    auto verification = evaluate_verification(
        alignment.payload.materialize(),
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
