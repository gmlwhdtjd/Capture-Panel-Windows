#include "capture_panel/core/verification.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace capture_panel {
namespace {

using Frame = std::int64_t;

struct CorrelationMatch {
    Frame frame = 0;
    double score = 0.0;
};

[[nodiscard]] double linear_from_dbfs(double dbfs) {
    return std::pow(10.0, dbfs / 20.0);
}

[[nodiscard]] Frame frame_count(double seconds, double sample_rate) {
    return std::max<Frame>(1, static_cast<Frame>(std::llround(seconds * sample_rate)));
}

[[nodiscard]] double fade_envelope(
    Frame offset,
    Frame sweep_frame_count,
    Frame fade_frames) {
    const auto fade_in = offset < fade_frames
        ? static_cast<double>(offset) / static_cast<double>(std::max<Frame>(1, fade_frames))
        : 1.0;
    const auto fade_out_offset = sweep_frame_count - 1 - offset;
    const auto fade_out = fade_out_offset < fade_frames
        ? static_cast<double>(std::max<Frame>(0, fade_out_offset))
            / static_cast<double>(std::max<Frame>(1, fade_frames))
        : 1.0;
    return std::min(1.0, std::max(0.0, std::min(fade_in, fade_out)));
}

void write_log_sweep(
    std::vector<float>& samples,
    AudioFrameRange frame_range,
    std::uint32_t channel_count,
    double sample_rate,
    double amplitude) {
    const auto start_frequency = constants::verification_signal::start_frequency;
    const auto end_frequency = std::min(
        constants::verification_signal::maximum_end_frequency,
        sample_rate * constants::verification_signal::nyquist_end_frequency_ratio);
    const auto sweep_frame_count = std::max<Frame>(1, frame_range.size());
    const auto duration = static_cast<double>(sweep_frame_count) / sample_rate;
    const auto log_ratio = std::log(end_frequency / start_frequency);
    const auto fade_frames = std::min(
        sweep_frame_count / 2,
        std::max<Frame>(
            1,
            frame_count(constants::verification_signal::fade_seconds, sample_rate)));

    for (Frame offset = 0; offset < sweep_frame_count; ++offset) {
        const auto frame = frame_range.lower_bound + offset;
        const auto time = static_cast<double>(offset) / sample_rate;
        const auto phase = 2.0 * std::acos(-1.0) * start_frequency * duration / log_ratio
            * (std::exp((time / duration) * log_ratio) - 1.0);
        const auto envelope = fade_envelope(offset, sweep_frame_count, fade_frames);
        const auto value = static_cast<float>(amplitude * envelope * std::sin(phase));
        for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
            const auto index = frame * static_cast<Frame>(channel_count)
                + static_cast<Frame>(channel);
            if (index < 0 || index >= static_cast<Frame>(samples.size())) continue;
            samples[static_cast<std::size_t>(index)] = value;
        }
    }
}

[[nodiscard]] std::vector<double> mono_samples(
    const std::vector<float>& samples,
    std::uint32_t channel_count,
    AudioFrameRange frame_range) {
    if (channel_count == 0 || frame_range.empty()) return {};
    std::vector<double> mono;
    mono.reserve(static_cast<std::size_t>(frame_range.size()));
    for (auto frame = frame_range.lower_bound; frame < frame_range.upper_bound; ++frame) {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
            const auto index = frame * static_cast<Frame>(channel_count)
                + static_cast<Frame>(channel);
            if (index < 0 || index >= static_cast<Frame>(samples.size())) continue;
            sum += static_cast<double>(samples[static_cast<std::size_t>(index)]);
            ++count;
        }
        mono.push_back(count > 0 ? sum / static_cast<double>(count) : 0.0);
    }
    return mono;
}

[[nodiscard]] double normalized_correlation(
    const std::vector<double>& recorded,
    const std::vector<double>& reference,
    Frame start_frame,
    double reference_energy) {
    if (start_frame < 0
        || start_frame + static_cast<Frame>(reference.size())
            > static_cast<Frame>(recorded.size())) {
        return 0.0;
    }

    double dot = 0.0;
    double recorded_energy = 0.0;
    for (std::size_t index = 0; index < reference.size(); ++index) {
        const auto recorded_value =
            recorded[static_cast<std::size_t>(start_frame) + index];
        const auto reference_value = reference[index];
        dot += recorded_value * reference_value;
        recorded_energy += recorded_value * recorded_value;
    }
    if (recorded_energy <= constants::verification_sweep::recorded_energy_floor) {
        return 0.0;
    }
    return std::abs(dot) / std::sqrt(reference_energy * recorded_energy);
}

[[nodiscard]] std::optional<CorrelationMatch> best_correlation_match(
    const std::vector<double>& recorded,
    const std::vector<double>& reference,
    AudioFrameRange search_range,
    Frame step) {
    if (search_range.empty()) return std::nullopt;
    const auto reference_energy = std::accumulate(
        reference.begin(), reference.end(), 0.0,
        [](double sum, double value) { return sum + value * value; });
    if (reference_energy <= 0.0) return std::nullopt;

    std::optional<CorrelationMatch> best;
    for (auto frame = search_range.lower_bound; frame < search_range.upper_bound;
         frame += std::max<Frame>(1, step)) {
        const auto score = normalized_correlation(
            recorded, reference, frame, reference_energy);
        if (!best.has_value() || score > best->score) {
            best = CorrelationMatch{.frame = frame, .score = score};
        }
    }
    return best;
}

[[nodiscard]] Frame separate_peak_tolerance_frames(double sample_rate) {
    return std::max<Frame>(
        static_cast<Frame>(constants::verification_sweep::minimum_separate_peak_frames),
        static_cast<Frame>(std::llround(
            sample_rate * constants::verification_sweep::separate_peak_seconds)));
}

[[nodiscard]] VerificationSweepResult unmeasurable_sweep(
    const VerificationSignal& signal) {
    return VerificationSweepResult{
        .expected_frame = signal.sweep_frame_range.lower_bound,
        .detected_frame = std::nullopt,
        .error_frames = std::nullopt,
        .direct_score = 0.0,
        .strongest_frame = std::nullopt,
        .strongest_offset_frames = std::nullopt,
        .strongest_score = 0.0,
        .ambiguity_ratio = std::nullopt,
        .reliability = VerificationReliability::unmeasurable,
    };
}

[[nodiscard]] std::optional<VerificationSweepResult> match_sweep(
    const AudioBuffer& aligned,
    Frame aligned_frame_count,
    const VerificationSignal& signal) {
    const auto recorded = mono_samples(
        aligned.samples,
        aligned.channel_count,
        AudioFrameRange{.lower_bound = 0, .upper_bound = aligned_frame_count});
    const auto reference = mono_samples(
        signal.audio.samples,
        signal.channel_count,
        signal.sweep_frame_range);
    if (recorded.empty() || reference.empty() || recorded.size() < reference.size()) {
        return std::nullopt;
    }

    const auto expected_frame = signal.sweep_frame_range.lower_bound;
    const auto search_radius = std::max<Frame>(
        static_cast<Frame>(constants::verification_sweep::minimum_search_radius_frames),
        static_cast<Frame>(std::llround(
            signal.format.sample_rate * constants::verification_sweep::search_radius_seconds)));
    const auto max_start_frame = static_cast<Frame>(recorded.size() - reference.size());
    const auto direct_range = AudioFrameRange{
        .lower_bound = std::max<Frame>(0, expected_frame - search_radius),
        .upper_bound = std::min(
            max_start_frame + 1, expected_frame + search_radius + 1),
    };
    if (direct_range.empty()) return std::nullopt;

    const auto direct = best_correlation_match(
        recorded, reference, direct_range, 1);
    if (!direct.has_value()
        || direct->score < constants::verification_sweep::minimum_direct_score) {
        return VerificationSweepResult{
            .expected_frame = expected_frame,
            .detected_frame = direct.has_value()
                ? std::optional<Frame>(direct->frame)
                : std::nullopt,
            .error_frames = direct.has_value()
                ? std::optional<Frame>(direct->frame - expected_frame)
                : std::nullopt,
            .direct_score = direct.has_value() ? direct->score : 0.0,
            .strongest_frame = std::nullopt,
            .strongest_offset_frames = std::nullopt,
            .strongest_score = 0.0,
            .ambiguity_ratio = std::nullopt,
            .reliability = VerificationReliability::unmeasurable,
        };
    }

    const auto coarse_step = std::max<Frame>(
        1,
        std::min<Frame>(
            static_cast<Frame>(constants::verification_sweep::maximum_coarse_step_frames),
            static_cast<Frame>(reference.size()
                / constants::verification_sweep::coarse_reference_frames_per_step)));
    const auto coarse_strongest = best_correlation_match(
        recorded,
        reference,
        AudioFrameRange{.lower_bound = 0, .upper_bound = max_start_frame + 1},
        coarse_step);
    auto refined_candidate = *direct;
    if (coarse_strongest.has_value()) {
        const auto refined = best_correlation_match(
            recorded,
            reference,
            AudioFrameRange{
                .lower_bound = std::max<Frame>(0, coarse_strongest->frame - coarse_step),
                .upper_bound = std::min(
                    max_start_frame + 1,
                    coarse_strongest->frame + coarse_step + 1),
            },
            1);
        if (refined.has_value()) refined_candidate = *refined;
    }
    const auto refined_strongest = refined_candidate.score > direct->score
        ? refined_candidate
        : *direct;
    const auto separate_peak = std::abs(refined_strongest.frame - direct->frame)
        > separate_peak_tolerance_frames(signal.format.sample_rate);
    std::optional<double> ambiguity_ratio;
    if (separate_peak && direct->score > 0.0) {
        ambiguity_ratio = refined_strongest.score / direct->score;
    }
    const auto reliability = ambiguity_ratio.has_value()
            && *ambiguity_ratio >= constants::verification_sweep::ambiguity_ratio_threshold
        ? VerificationReliability::ambiguous
        : VerificationReliability::reliable;

    return VerificationSweepResult{
        .expected_frame = expected_frame,
        .detected_frame = direct->frame,
        .error_frames = direct->frame - expected_frame,
        .direct_score = direct->score,
        .strongest_frame = refined_strongest.frame,
        .strongest_offset_frames = refined_strongest.frame - expected_frame,
        .strongest_score = refined_strongest.score,
        .ambiguity_ratio = ambiguity_ratio,
        .reliability = reliability,
    };
}

[[nodiscard]] double peak(const std::vector<float>& samples) {
    double result = 0.0;
    for (const auto sample : samples) {
        result = std::max(result, std::abs(static_cast<double>(sample)));
    }
    return result;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> clamped_sample_range(
    AudioFrameRange frame_range,
    std::uint32_t channel_count,
    std::size_t sample_count) {
    if (channel_count == 0 || sample_count == 0) return {0, 0};
    const auto lower_frame = std::max<Frame>(0, frame_range.lower_bound);
    const auto upper_frame = std::max(lower_frame, frame_range.upper_bound);
    const auto lower = std::min(
        sample_count,
        static_cast<std::size_t>(lower_frame)
            * static_cast<std::size_t>(channel_count));
    const auto upper = std::max(
        lower,
        std::min(
            sample_count,
            static_cast<std::size_t>(upper_frame)
                * static_cast<std::size_t>(channel_count)));
    return {lower, upper};
}

[[nodiscard]] double peak(
    const std::vector<float>& samples,
    std::uint32_t channel_count,
    AudioFrameRange frame_range) {
    const auto [lower, upper] = clamped_sample_range(
        frame_range, channel_count, samples.size());
    double result = 0.0;
    for (auto index = lower; index < upper; ++index) {
        result = std::max(result, std::abs(static_cast<double>(samples[index])));
    }
    return result;
}

[[nodiscard]] double rms(
    const std::vector<float>& samples,
    std::uint32_t channel_count,
    AudioFrameRange frame_range) {
    const auto [lower, upper] = clamped_sample_range(
        frame_range, channel_count, samples.size());
    if (lower >= upper) return 0.0;
    double squared_sum = 0.0;
    for (auto index = lower; index < upper; ++index) {
        const auto value = static_cast<double>(samples[index]);
        squared_sum += value * value;
    }
    return std::sqrt(squared_sum / static_cast<double>(upper - lower));
}

[[nodiscard]] Frame timing_tolerance_frames(double sample_rate) {
    return std::max<Frame>(
        static_cast<Frame>(constants::verification_evaluation::minimum_timing_tolerance_frames),
        static_cast<Frame>(std::llround(
            sample_rate * constants::verification_evaluation::timing_tolerance_seconds)));
}

[[nodiscard]] double peak_around(
    const AudioBuffer& aligned,
    std::uint32_t channel_count,
    Frame aligned_frame_count,
    Frame frame,
    Frame tolerance_frames) {
    const auto lower = std::max<Frame>(0, frame - tolerance_frames);
    const auto upper = std::min(
        aligned_frame_count, frame + tolerance_frames + 1);
    if (lower >= upper) return 0.0;
    return peak(
        aligned.samples,
        channel_count,
        AudioFrameRange{.lower_bound = lower, .upper_bound = upper});
}

[[nodiscard]] bool leading_silence_looks_contaminated(
    const AudioBuffer& aligned,
    const VerificationSignal& signal,
    double first_event_peak) {
    if (first_event_peak <= 0.0 || signal.leading_silence_frame_range.empty()) {
        return false;
    }
    const auto leading_peak = peak(
        aligned.samples,
        aligned.channel_count,
        signal.leading_silence_frame_range);
    const auto leading_rms = rms(
        aligned.samples,
        aligned.channel_count,
        signal.leading_silence_frame_range);
    return leading_peak > first_event_peak * linear_from_dbfs(
               constants::verification_evaluation::leading_silence_peak_relative_dbfs)
        || leading_rms > first_event_peak * linear_from_dbfs(
               constants::verification_evaluation::leading_silence_rms_relative_dbfs);
}

void append_unique_warning(
    std::vector<CaptureWarning>& warnings,
    CaptureWarning warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

void append_unique_failure(
    std::vector<CaptureFailure>& failures,
    CaptureFailure failure) {
    if (std::find(failures.begin(), failures.end(), failure) == failures.end()) {
        failures.push_back(failure);
    }
}

} // namespace

VerificationSignal make_verification_signal(
    double sample_rate,
    std::uint32_t channel_count,
    double level_dbfs) {
    const auto safe_channel_count = std::max<std::uint32_t>(1, channel_count);
    const auto safe_sample_rate = sample_rate > 0.0
        ? sample_rate
        : constants::audio::fallback_sample_rate;
    const auto leading_silence_frames = frame_count(
        constants::verification_signal::leading_silence_seconds,
        safe_sample_rate);
    const auto sweep_frames = frame_count(
        constants::verification_signal::sweep_seconds,
        safe_sample_rate);
    const auto trailing_silence_frames = frame_count(
        constants::verification_signal::trailing_silence_seconds,
        safe_sample_rate);
    const auto sweep_range = AudioFrameRange{
        .lower_bound = leading_silence_frames,
        .upper_bound = leading_silence_frames + sweep_frames,
    };
    const auto total_frames = leading_silence_frames + sweep_frames
        + trailing_silence_frames;
    AudioBuffer audio{
        .sample_rate = safe_sample_rate,
        .channel_count = safe_channel_count,
        .samples = std::vector<float>(
            static_cast<std::size_t>(total_frames)
                * static_cast<std::size_t>(safe_channel_count),
            0.0F),
    };
    write_log_sweep(
        audio.samples,
        sweep_range,
        safe_channel_count,
        safe_sample_rate,
        linear_from_dbfs(level_dbfs));

    return VerificationSignal{
        .audio = std::move(audio),
        .format = WavFormat{
            .sample_rate = safe_sample_rate,
            .channel_count = safe_channel_count,
            .bit_depth = AudioBitDepth::pcm24,
            .total_frames = total_frames,
        },
        .channel_count = safe_channel_count,
        .leading_silence_frame_range = AudioFrameRange{
            .lower_bound = 0,
            .upper_bound = leading_silence_frames,
        },
        .sweep_frame_range = sweep_range,
    };
}

AlignmentVerificationResult evaluate_verification(
    const AudioBuffer& aligned,
    const VerificationSignal& signal,
    const PayloadAlignmentInfo& alignment_info,
    double input_peak_dbfs) {
    static_cast<void>(alignment_info);
    const auto channel_count = aligned.channel_count;
    const auto aligned_frame_count = aligned.frame_count();
    std::vector<CaptureWarning> warnings;
    std::vector<CaptureFailure> failures;

    if (input_peak_dbfs >= constants::verification_evaluation::clipping_threshold_dbfs) {
        append_unique_failure(failures, CaptureFailure::digital_clipping);
    }
    if (peak(aligned.samples)
            <= constants::verification_evaluation::missing_signal_peak_threshold
        || aligned_frame_count <= 0) {
        append_unique_failure(failures, CaptureFailure::verification_signal_missing);
        return AlignmentVerificationResult{
            .start_offset_frames = std::nullopt,
            .timing_fit_error_frames = std::nullopt,
            .max_timing_error_frames = std::nullopt,
            .sweep = unmeasurable_sweep(signal),
            .ambiguous_match_count = 0,
            .warnings = std::move(warnings),
            .failures = std::move(failures),
        };
    }

    const auto sweep = match_sweep(aligned, aligned_frame_count, signal);
    if (!sweep.has_value()) {
        append_unique_failure(failures, CaptureFailure::verification_signal_missing);
        return AlignmentVerificationResult{
            .start_offset_frames = std::nullopt,
            .timing_fit_error_frames = std::nullopt,
            .max_timing_error_frames = std::nullopt,
            .sweep = unmeasurable_sweep(signal),
            .ambiguous_match_count = 0,
            .warnings = std::move(warnings),
            .failures = std::move(failures),
        };
    }
    if (!sweep->error_frames.has_value()) {
        append_unique_failure(failures, CaptureFailure::verification_signal_missing);
        return AlignmentVerificationResult{
            .start_offset_frames = std::nullopt,
            .timing_fit_error_frames = std::nullopt,
            .max_timing_error_frames = std::nullopt,
            .sweep = sweep,
            .ambiguous_match_count = 0,
            .warnings = std::move(warnings),
            .failures = std::move(failures),
        };
    }

    const auto timing_error = *sweep->error_frames;
    const auto absolute_error = std::abs(timing_error);
    const auto fit_error = static_cast<double>(absolute_error);
    const auto tolerance = timing_tolerance_frames(signal.format.sample_rate);
    if (fit_error > static_cast<double>(tolerance)) {
        append_unique_failure(failures, CaptureFailure::verification_timing_mismatch);
    } else if (fit_error
        > static_cast<double>(tolerance)
            * constants::verification_evaluation::high_timing_error_warning_ratio) {
        append_unique_warning(warnings, CaptureWarning::alignment_fit_error_high);
    }
    const auto ambiguous_match_count =
        sweep->reliability == VerificationReliability::ambiguous ? 1U : 0U;
    if (sweep->reliability == VerificationReliability::ambiguous) {
        append_unique_warning(warnings, CaptureWarning::verification_ambiguous);
    }
    if (sweep->reliability == VerificationReliability::unmeasurable) {
        append_unique_failure(failures, CaptureFailure::verification_signal_missing);
    }

    const auto first_event_peak = peak_around(
        aligned,
        channel_count,
        aligned_frame_count,
        signal.sweep_frame_range.lower_bound,
        tolerance);
    if (leading_silence_looks_contaminated(aligned, signal, first_event_peak)) {
        append_unique_warning(warnings, CaptureWarning::equipment_decay_may_affect_capture);
    }

    return AlignmentVerificationResult{
        .start_offset_frames = timing_error,
        .timing_fit_error_frames = fit_error,
        .max_timing_error_frames = absolute_error,
        .sweep = sweep,
        .ambiguous_match_count = ambiguous_match_count,
        .warnings = std::move(warnings),
        .failures = std::move(failures),
    };
}

} // namespace capture_panel
