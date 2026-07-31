#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace capture_panel {

enum class AudioBitDepth : std::uint16_t {
    pcm16 = 16,
    pcm24 = 24,
    pcm32 = 32,
};

struct AudioBuffer {
    double sample_rate = 0.0;
    std::uint32_t channel_count = 0;
    std::vector<float> samples;

    [[nodiscard]] std::int64_t frame_count() const noexcept {
        return channel_count == 0
            ? 0
            : static_cast<std::int64_t>(samples.size() / channel_count);
    }

    [[nodiscard]] double duration_seconds() const noexcept {
        return sample_rate > 0.0
            ? static_cast<double>(frame_count()) / sample_rate
            : 0.0;
    }
};

struct WavFormat {
    double sample_rate = 0.0;
    std::uint32_t channel_count = 0;
    AudioBitDepth bit_depth = AudioBitDepth::pcm24;
    std::int64_t total_frames = 0;

    [[nodiscard]] double duration_seconds() const noexcept {
        return sample_rate > 0.0
            ? static_cast<double>(total_frames) / sample_rate
            : 0.0;
    }
};

struct WavContents {
    WavFormat format;
    AudioBuffer audio;
};

enum class ChannelDirection { input, output };

struct AudioChannel {
    std::uint32_t index = 0; // Public, one-based index.
    std::string name;
};

struct AudioDevice {
    std::string id;
    std::string name;
    std::uint32_t input_channels = 0;
    std::uint32_t output_channels = 0;
    double sample_rate = 0.0;
    bool available = true;
    std::string status;

    [[nodiscard]] bool has_input() const noexcept { return input_channels > 0; }
    [[nodiscard]] bool has_output() const noexcept { return output_channels > 0; }
};

struct CaptureRoute {
    std::string driver_id;
    std::vector<std::uint32_t> playback_channels;
    std::vector<std::uint32_t> record_channels;
};

struct CaptureConfiguration {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    CaptureRoute route;
    std::optional<AudioBitDepth> output_bit_depth;
};

struct CapturePassOptions {
    double playback_gain_db = 0.0;
    double recording_gain_db = 0.0;
};

class CancellationToken {
public:
    bool cancel() noexcept {
        auto expected = State::active;
        return state_.compare_exchange_strong(
            expected,
            State::cancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_.load(std::memory_order_acquire) == State::cancelled;
    }

    [[nodiscard]] bool begin_output_commit() noexcept {
        auto expected = State::active;
        if (state_.compare_exchange_strong(
                expected,
                State::output_committing,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
        return expected == State::output_committing;
    }

private:
    enum class State : std::uint8_t {
        active,
        cancelled,
        output_committing,
    };

    static_assert(std::atomic<State>::is_always_lock_free);
    std::atomic<State> state_{State::active};
};

enum class CaptureWarning {
    source_channel_count_mismatch,
    source_near_digital_full_scale,
    equipment_decay_may_affect_capture,
    marker_evidence_low,
    alignment_fit_error_high,
    verification_ambiguous,
};

enum class CaptureFailure {
    digital_clipping,
    verification_signal_missing,
    verification_timing_mismatch,
};

struct CaptureProgress {
    std::int64_t completed_frames = 0;
    std::int64_t total_frames = 0;
    double sample_rate = 0.0;

    [[nodiscard]] double fraction() const noexcept {
        if (total_frames <= 0) return 0.0;
        return std::clamp(
            static_cast<double>(completed_frames) / static_cast<double>(total_frames),
            0.0,
            1.0);
    }

    [[nodiscard]] int percentage() const noexcept {
        return static_cast<int>(fraction() * 100.0);
    }

    [[nodiscard]] double remaining_seconds() const noexcept {
        if (!(sample_rate > 0.0) || total_frames <= completed_frames) return 0.0;
        return static_cast<double>(total_frames - std::max<std::int64_t>(0, completed_frames))
            / sample_rate;
    }
};

enum class CaptureStage {
    sample_rate_configuration,
    recording,
    alignment,
    verification,
    output_writing,
};

struct ImpulseDetection {
    std::vector<std::int64_t> expected_positions;
    std::vector<std::int64_t> detected_positions;
};

struct PayloadAlignmentInfo {
    std::optional<std::int64_t> marker_latency_samples;
    std::optional<double> marker_latency_milliseconds;
    std::int64_t trim_start_frame = 0;
    std::int64_t trimmed_frame_count = 0;
    std::int64_t target_frame_count = 0;
};

struct CaptureInputInfo {
    std::filesystem::path path;
    WavFormat format;
};

struct CaptureOutputInfo {
    std::filesystem::path path;
    std::uintmax_t file_size = 0;
    double sample_rate = 0.0;
    std::uint32_t channel_count = 0;
    AudioBitDepth bit_depth = AudioBitDepth::pcm24;
};

struct CapturePassResult {
    CaptureInputInfo input;
    CaptureOutputInfo output;
    PayloadAlignmentInfo alignment;
    std::chrono::duration<double> elapsed{};
};

enum class VerificationReliability { reliable, ambiguous, unmeasurable };

struct VerificationSweepResult {
    std::int64_t expected_frame = 0;
    std::optional<std::int64_t> detected_frame;
    std::optional<std::int64_t> error_frames;
    double direct_score = 0.0;
    std::optional<std::int64_t> strongest_frame;
    std::optional<std::int64_t> strongest_offset_frames;
    double strongest_score = 0.0;
    std::optional<double> ambiguity_ratio;
    VerificationReliability reliability = VerificationReliability::unmeasurable;
};

struct AlignmentVerificationResult {
    std::optional<std::int64_t> start_offset_frames;
    std::optional<double> timing_fit_error_frames;
    std::optional<std::int64_t> max_timing_error_frames;
    std::optional<VerificationSweepResult> sweep;
    std::size_t ambiguous_match_count = 0;
    std::vector<CaptureWarning> warnings;
    std::vector<CaptureFailure> failures;
};

struct CaptureVerificationResult {
    CaptureInputInfo input;
    PayloadAlignmentInfo alignment;
    AlignmentVerificationResult verification;
    std::optional<ImpulseDetection> impulse_detection;
    double output_peak_dbfs = 0.0;
    double input_peak_dbfs = 0.0;
    double sample_rate = 0.0;
    std::vector<CaptureWarning> warnings;
    std::vector<CaptureFailure> failures;
    std::chrono::duration<double> elapsed{};

    [[nodiscard]] bool passed() const noexcept { return failures.empty(); }
};

enum class CaptureEventType {
    started,
    input_loaded,
    warning,
    devices_validated,
    stage_changed,
    recording_progress,
    capture_finished,
    impulse_detection,
    alignment_finished,
    verification_finished,
    output_written,
};

struct CaptureEvent {
    CaptureEventType type = CaptureEventType::started;
    std::optional<CaptureStage> stage;
    std::optional<CaptureProgress> progress;
    std::optional<CaptureWarning> warning;
    std::optional<CaptureInputInfo> input;
    std::optional<AudioDevice> device;
    std::optional<CaptureRoute> route;
    std::optional<ImpulseDetection> impulse_detection;
    std::optional<PayloadAlignmentInfo> alignment;
    std::optional<AlignmentVerificationResult> verification;
    std::optional<CaptureOutputInfo> output;
    std::optional<double> sample_rate;
    std::optional<double> elapsed_seconds;
    std::optional<std::int64_t> total_frames;
    std::optional<double> padding_seconds;
    std::optional<double> marker_to_payload_silence_seconds;
    std::string message;
};

using CaptureEventHandler = std::function<void(const CaptureEvent&)>;

} // namespace capture_panel
