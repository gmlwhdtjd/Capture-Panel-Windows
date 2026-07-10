#include "capture_panel/core/alignment.hpp"

#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace capture_panel {
namespace {

using Frame = std::int64_t;

struct FrameRange {
    Frame lower = 0;
    Frame upper = 0;

    [[nodiscard]] bool empty() const noexcept { return lower >= upper; }
};

[[nodiscard]] double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0)
        / static_cast<double>(values.size());
}

[[nodiscard]] double mean(const std::vector<Frame>& values) {
    if (values.empty()) return 0.0;
    const auto sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

[[nodiscard]] double standard_deviation(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const auto average = mean(values);
    const auto squared_difference_sum = std::accumulate(
        values.begin(), values.end(), 0.0,
        [average](double sum, double value) {
            const auto difference = value - average;
            return sum + difference * difference;
        });
    return std::sqrt(squared_difference_sum / static_cast<double>(values.size()));
}

[[nodiscard]] double standard_deviation(const std::vector<Frame>& values) {
    std::vector<double> converted;
    converted.reserve(values.size());
    for (const auto value : values) converted.push_back(static_cast<double>(value));
    return standard_deviation(converted);
}

[[nodiscard]] Frame median(std::vector<Frame> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

[[nodiscard]] double marker_sequence_average_peak(
    const std::vector<MarkerCandidate>& candidates) {
    if (candidates.empty()) return 0.0;
    const auto sum = std::accumulate(
        candidates.begin(), candidates.end(), 0.0,
        [](double value, const MarkerCandidate& candidate) {
            return value + static_cast<double>(candidate.peak);
        });
    return sum / static_cast<double>(candidates.size());
}

[[nodiscard]] Frame sequence_match_tolerance_frames(double sample_rate) {
    return std::max<Frame>(
        static_cast<Frame>(constants::marker::minimum_sequence_match_tolerance_frames),
        static_cast<Frame>(std::llround(
            sample_rate * constants::marker::sequence_match_tolerance_seconds)));
}

[[nodiscard]] double missing_marker_penalty_frames(double sample_rate) {
    return std::max(
        constants::marker::minimum_missing_marker_penalty_frames,
        sample_rate * constants::marker::missing_marker_penalty_seconds);
}

[[nodiscard]] std::size_t lower_bound_candidate_index(
    const std::vector<MarkerCandidate>& candidates,
    Frame frame) {
    return static_cast<std::size_t>(std::lower_bound(
        candidates.begin(), candidates.end(), frame,
        [](const MarkerCandidate& candidate, Frame target) {
            return candidate.frame < target;
        }) - candidates.begin());
}

[[nodiscard]] bool is_better_nearest_candidate(
    const MarkerCandidate& candidate,
    const std::optional<MarkerCandidate>& current,
    Frame target_frame) {
    if (!current.has_value()) return true;
    const auto candidate_distance = std::abs(candidate.frame - target_frame);
    const auto current_distance = std::abs(current->frame - target_frame);
    if (candidate_distance != current_distance) {
        return candidate_distance < current_distance;
    }
    return candidate.peak > current->peak;
}

[[nodiscard]] std::optional<MarkerCandidate> nearest_marker_candidate(
    Frame target_frame,
    const std::vector<MarkerCandidate>& candidates,
    Frame tolerance_frames) {
    const auto insertion_index = static_cast<Frame>(
        lower_bound_candidate_index(candidates, target_frame));
    std::optional<MarkerCandidate> nearest;

    for (const auto index : std::array<Frame, 3>{
             insertion_index - 1, insertion_index, insertion_index + 1}) {
        if (index < 0 || index >= static_cast<Frame>(candidates.size())) continue;
        const auto& candidate = candidates[static_cast<std::size_t>(index)];
        if (std::abs(candidate.frame - target_frame) > tolerance_frames) continue;
        if (is_better_nearest_candidate(candidate, nearest, target_frame)) {
            nearest = candidate;
        }
    }
    return nearest;
}

[[nodiscard]] std::optional<MarkerSequence> make_marker_sequence(
    const std::vector<MarkerCandidate>& candidates,
    const std::vector<Frame>& expected_marker_positions,
    Frame latency,
    Frame tolerance_frames,
    double sample_rate) {
    std::vector<MarkerCandidate> sequence;
    sequence.reserve(expected_marker_positions.size());

    for (const auto expected : expected_marker_positions) {
        const auto candidate = nearest_marker_candidate(
            expected + latency, candidates, tolerance_frames);
        if (!candidate.has_value()) break;
        if (!sequence.empty() && sequence.back().frame == candidate->frame) break;
        sequence.push_back(*candidate);
    }

    if (sequence.size() < constants::impulse::minimum_impulses_for_analysis) {
        return std::nullopt;
    }

    std::vector<Frame> frames;
    std::vector<Frame> latencies;
    frames.reserve(sequence.size());
    latencies.reserve(sequence.size());
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        frames.push_back(sequence[index].frame);
        latencies.push_back(sequence[index].frame - expected_marker_positions[index]);
    }

    std::vector<double> interval_errors;
    interval_errors.reserve(sequence.size() - 1);
    for (std::size_t index = 1; index < sequence.size(); ++index) {
        const auto measured = frames[index] - frames[index - 1];
        const auto expected = expected_marker_positions[index]
            - expected_marker_positions[index - 1];
        interval_errors.push_back(static_cast<double>(measured - expected));
    }

    const auto median_latency = median(latencies);
    const auto negative_latency_penalty = median_latency < 0
        ? std::abs(static_cast<double>(median_latency))
            * constants::marker::negative_latency_penalty_multiplier
        : 0.0;
    const auto late_echo_tie_breaker = std::max(0.0, static_cast<double>(median_latency))
        * constants::marker::late_echo_tie_breaker_weight;
    const auto missing_marker_penalty =
        static_cast<double>(expected_marker_positions.size() - sequence.size())
        * missing_marker_penalty_frames(sample_rate);

    auto absolute_interval_errors = interval_errors;
    std::transform(
        absolute_interval_errors.begin(), absolute_interval_errors.end(),
        absolute_interval_errors.begin(),
        [](double value) { return std::abs(value); });
    const auto fit_score = standard_deviation(latencies)
        + mean(absolute_interval_errors)
        + standard_deviation(interval_errors)
        + negative_latency_penalty
        + late_echo_tie_breaker
        + missing_marker_penalty;
    const auto average_peak = marker_sequence_average_peak(sequence);

    return MarkerSequence{
        .candidates = std::move(sequence),
        .fit_score = fit_score,
        .average_peak = average_peak,
    };
}

[[nodiscard]] bool is_better_marker_sequence(
    const MarkerSequence& candidate,
    const std::optional<MarkerSequence>& current) {
    if (!current.has_value()) return true;
    if (candidate.fit_score
        < current->fit_score - constants::marker::fit_score_epsilon) {
        return true;
    }
    if (std::abs(candidate.fit_score - current->fit_score)
        <= constants::marker::fit_score_epsilon) {
        return candidate.average_peak > current->average_peak;
    }
    return false;
}

[[nodiscard]] float frame_peak(
    const std::vector<float>& samples,
    std::uint32_t channel_count,
    Frame frame) {
    float peak = 0.0F;
    for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
        const auto index = frame * static_cast<Frame>(channel_count)
            + static_cast<Frame>(channel);
        if (index < 0 || index >= static_cast<Frame>(samples.size())) continue;
        peak = std::max(peak, std::abs(samples[static_cast<std::size_t>(index)]));
    }
    return peak;
}

[[nodiscard]] float peak_in_frame_range(
    const std::vector<float>& samples,
    std::uint32_t channel_count,
    FrameRange frame_range) {
    float peak = 0.0F;
    for (auto frame = frame_range.lower; frame < frame_range.upper; ++frame) {
        peak = std::max(peak, frame_peak(samples, channel_count, frame));
    }
    return peak;
}

[[nodiscard]] std::vector<MarkerCandidate> detect_marker_impulse_candidates(
    const std::vector<float>& samples,
    std::uint32_t channel_count,
    FrameRange frame_range) {
    if (frame_range.empty()) return {};

    const auto peak_level = peak_in_frame_range(samples, channel_count, frame_range);
    const auto adaptive_threshold = std::max(
        peak_level * constants::impulse::adaptive_threshold_ratio,
        constants::impulse::minimum_threshold);
    std::vector<MarkerCandidate> candidates;
    auto frame = frame_range.lower;
    while (frame < frame_range.upper) {
        const auto onset_peak = frame_peak(samples, channel_count, frame);
        if (onset_peak <= adaptive_threshold) {
            ++frame;
            continue;
        }

        const auto onset_frame = frame;
        auto peak = onset_peak;
        ++frame;
        while (frame < frame_range.upper) {
            const auto next_peak = frame_peak(samples, channel_count, frame);
            if (next_peak <= adaptive_threshold) break;
            peak = std::max(peak, next_peak);
            ++frame;
        }
        candidates.push_back({.frame = onset_frame, .peak = peak});
    }
    return candidates;
}

[[nodiscard]] FrameRange marker_search_frame_range(
    const std::vector<Frame>& expected_marker_positions,
    const AlignmentReference& reference,
    Frame total_recorded_frames,
    double sample_rate) {
    if (total_recorded_frames <= 0 || expected_marker_positions.empty()) return {};

    const auto radius_frames = std::max<Frame>(
        static_cast<Frame>(constants::marker::minimum_search_radius_frames),
        static_cast<Frame>(std::llround(
            sample_rate * constants::marker::search_radius_seconds)));
    const auto marker_tail_frames = std::max<Frame>(
        static_cast<Frame>(constants::marker::minimum_marker_tail_frames),
        static_cast<Frame>(std::llround(
            sample_rate * constants::marker::marker_tail_seconds)));
    const auto first_expected = expected_marker_positions.front();
    const auto last_expected = expected_marker_positions.back();
    const auto lower = std::max<Frame>(0, first_expected - radius_frames);
    const auto marker_upper = last_expected + radius_frames + marker_tail_frames;
    const auto capped_upper = reference.source_start_frame > last_expected
        ? std::min(marker_upper, reference.source_start_frame)
        : marker_upper;
    const auto upper = std::min(total_recorded_frames, std::max(lower, capped_upper));
    if (upper <= lower) return {};
    return {.lower = lower, .upper = upper};
}

[[nodiscard]] std::vector<Frame> marker_latencies(
    const std::vector<Frame>& detected,
    const std::vector<Frame>& expected) {
    const auto count = std::min(detected.size(), expected.size());
    std::vector<Frame> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(detected[index] - expected[index]);
    }
    return result;
}

[[nodiscard]] std::optional<double> marker_interval_error_ppm(
    const std::vector<Frame>& expected,
    const std::vector<Frame>& detected) {
    const auto count = std::min(expected.size(), detected.size());
    if (count < 2) return std::nullopt;

    std::vector<Frame> expected_intervals;
    std::vector<Frame> detected_intervals;
    expected_intervals.reserve(count - 1);
    detected_intervals.reserve(count - 1);
    for (std::size_t index = 1; index < count; ++index) {
        expected_intervals.push_back(expected[index] - expected[index - 1]);
        detected_intervals.push_back(detected[index] - detected[index - 1]);
    }
    const auto expected_mean = mean(expected_intervals);
    if (expected_mean <= 0.0) return std::nullopt;
    return ((mean(detected_intervals) - expected_mean) / expected_mean) * 1'000'000.0;
}

void append_unique_warning(
    std::vector<CaptureWarning>& warnings,
    CaptureWarning warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

[[nodiscard]] std::vector<CaptureWarning> marker_evidence_warnings(
    std::size_t detected_count,
    std::size_t expected_count,
    const std::vector<Frame>& latencies,
    const std::vector<Frame>& expected_positions,
    const std::vector<Frame>& detected_positions) {
    std::vector<CaptureWarning> warnings;
    if (expected_count > 0
        && static_cast<double>(detected_count) / static_cast<double>(expected_count)
            < constants::marker::warning_detected_marker_ratio) {
        append_unique_warning(warnings, CaptureWarning::marker_evidence_low);
    }
    if (standard_deviation(latencies)
        > constants::impulse::marker_latency_spread_threshold) {
        append_unique_warning(warnings, CaptureWarning::alignment_fit_error_high);
    }
    const auto interval_error = marker_interval_error_ppm(
        expected_positions, detected_positions);
    if (interval_error.has_value()
        && std::abs(*interval_error)
            > constants::impulse::marker_interval_error_ppm_threshold) {
        append_unique_warning(warnings, CaptureWarning::alignment_fit_error_high);
    }
    return warnings;
}

[[nodiscard]] AudioBuffer extract_aligned_audio(
    const AudioBuffer& recorded,
    std::uint32_t channel_count,
    Frame trim_start_frame,
    Frame target_frame_count) {
    const auto safe_target_frames = std::max<Frame>(0, target_frame_count);
    AudioBuffer output{
        .sample_rate = recorded.sample_rate,
        .channel_count = channel_count,
        .samples = std::vector<float>(
            static_cast<std::size_t>(safe_target_frames)
                * static_cast<std::size_t>(channel_count),
            0.0F),
    };
    const auto total_recorded_frames = static_cast<Frame>(
        recorded.samples.size() / static_cast<std::size_t>(channel_count));
    const auto actual_frames = std::min(
        safe_target_frames,
        std::max<Frame>(0, total_recorded_frames - trim_start_frame));
    for (Frame frame = 0; frame < actual_frames; ++frame) {
        for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
            const auto source_index = (trim_start_frame + frame)
                    * static_cast<Frame>(channel_count)
                + static_cast<Frame>(channel);
            const auto destination_index = frame * static_cast<Frame>(channel_count)
                + static_cast<Frame>(channel);
            output.samples[static_cast<std::size_t>(destination_index)] =
                recorded.samples[static_cast<std::size_t>(source_index)];
        }
    }
    return output;
}

} // namespace

std::vector<std::int64_t> MarkerSequence::frames() const {
    std::vector<std::int64_t> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) result.push_back(candidate.frame);
    return result;
}

std::optional<MarkerSequence> select_marker_sequence(
    const std::vector<MarkerCandidate>& candidates,
    const std::vector<std::int64_t>& expected_marker_positions,
    double sample_rate) {
    const auto expected_count = expected_marker_positions.size();
    if (expected_count == 0 || candidates.empty()) return std::nullopt;
    if (candidates.size() <= expected_count) {
        return MarkerSequence{
            .candidates = candidates,
            .fit_score = 0.0,
            .average_peak = marker_sequence_average_peak(candidates),
        };
    }

    auto sorted_candidates = candidates;
    std::sort(
        sorted_candidates.begin(), sorted_candidates.end(),
        [](const MarkerCandidate& left, const MarkerCandidate& right) {
            if (left.frame == right.frame) return left.peak > right.peak;
            return left.frame < right.frame;
        });
    const auto tolerance_frames = sequence_match_tolerance_frames(sample_rate);
    std::unordered_set<Frame> evaluated_latencies;
    std::optional<MarkerSequence> best_sequence;

    for (const auto& candidate : sorted_candidates) {
        for (const auto expected_marker : expected_marker_positions) {
            const auto latency = candidate.frame - expected_marker;
            if (!evaluated_latencies.insert(latency).second) continue;
            auto sequence = make_marker_sequence(
                sorted_candidates,
                expected_marker_positions,
                latency,
                tolerance_frames,
                sample_rate);
            if (sequence.has_value()
                && is_better_marker_sequence(*sequence, best_sequence)) {
                best_sequence = std::move(sequence);
            }
        }
    }
    return best_sequence;
}

AlignmentPlaybackPlan make_alignment_playback_plan(
    const AudioBuffer& source,
    float playback_gain) {
    const auto channel_count = std::max<std::uint32_t>(1, source.channel_count);
    const auto sample_rate = source.sample_rate;
    const auto source_frame_count = static_cast<Frame>(
        source.samples.size() / static_cast<std::size_t>(channel_count));
    const auto impulse_interval_frames = std::max<Frame>(
        1,
        static_cast<Frame>(std::llround(
            sample_rate * constants::alignment::impulse_interval_seconds)));
    const auto marker_to_payload_silence_frames = std::max<Frame>(
        1,
        static_cast<Frame>(std::llround(
            sample_rate * constants::alignment::marker_to_payload_silence_seconds)));
    const auto gain = std::isfinite(playback_gain) ? playback_gain : 1.0F;
    std::vector<Frame> marker_frames;
    marker_frames.reserve(constants::alignment::impulse_count);
    for (std::size_t index = 0; index < constants::alignment::impulse_count; ++index) {
        marker_frames.push_back(static_cast<Frame>(index) * impulse_interval_frames);
    }
    const auto source_start_frame = (marker_frames.empty() ? 0 : marker_frames.back())
        + marker_to_payload_silence_frames;
    const auto playback_frame_count = source_start_frame + source_frame_count;
    AudioBuffer playback{
        .sample_rate = source.sample_rate,
        .channel_count = channel_count,
        .samples = std::vector<float>(
            static_cast<std::size_t>(playback_frame_count)
                * static_cast<std::size_t>(channel_count),
            0.0F),
    };

    const auto marker_amplitude = static_cast<float>(std::pow(
        10.0, constants::alignment::impulse_level_dbfs / 20.0)) * gain;
    for (const auto marker_frame : marker_frames) {
        for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
            const auto index = marker_frame * static_cast<Frame>(channel_count)
                + static_cast<Frame>(channel);
            playback.samples[static_cast<std::size_t>(index)] = marker_amplitude;
        }
    }
    const auto source_sample_count = static_cast<std::size_t>(source_frame_count)
        * static_cast<std::size_t>(channel_count);
    const auto source_offset = static_cast<std::size_t>(source_start_frame)
        * static_cast<std::size_t>(channel_count);
    for (std::size_t index = 0; index < source_sample_count; ++index) {
        playback.samples[source_offset + index] = source.samples[index] * gain;
    }

    return AlignmentPlaybackPlan{
        .audio = std::move(playback),
        .playback_frame_count = playback_frame_count,
        .marker_frames = std::move(marker_frames),
        .source_start_frame = source_start_frame,
    };
}

PayloadAlignment align_payload(
    const AudioBuffer& recorded,
    const AlignmentReference& reference) {
    const auto channel_count = std::max<std::uint32_t>(1, recorded.channel_count);
    const auto total_recorded_frames = static_cast<Frame>(
        recorded.samples.size() / static_cast<std::size_t>(channel_count));
    const auto search_range = marker_search_frame_range(
        reference.expected_marker_frames,
        reference,
        total_recorded_frames,
        recorded.sample_rate);
    const auto marker_candidates = detect_marker_impulse_candidates(
        recorded.samples, channel_count, search_range);
    const auto selected_sequence = select_marker_sequence(
        marker_candidates,
        reference.expected_marker_frames,
        recorded.sample_rate);
    const auto detected_impulses = selected_sequence.has_value()
        ? selected_sequence->frames()
        : std::vector<Frame>{};
    const auto impulse_detection = ImpulseDetection{
        .expected_positions = reference.expected_marker_frames,
        .detected_positions = detected_impulses,
    };

    const auto expected_count = reference.expected_marker_frames.size();
    const auto detected_count = detected_impulses.size();
    const auto evidence_ratio = expected_count == 0
        ? 0.0
        : static_cast<double>(detected_count) / static_cast<double>(expected_count);
    if (detected_count < constants::impulse::minimum_impulses_for_analysis
        || evidence_ratio < constants::marker::minimum_detected_marker_ratio) {
        throw CaptureError(
            ErrorCode::alignment_failed,
            "Marker-based alignment failed: the recorded markers could not be found "
            "with enough evidence.");
    }

    const auto latencies = marker_latencies(
        detected_impulses, reference.expected_marker_frames);
    const auto marker_latency = median(latencies);
    const auto trim_start_frame = std::max<Frame>(
        0, reference.source_start_frame + marker_latency);
    const auto target_frame_count = std::max<Frame>(0, reference.source_frame_count);
    const auto trimmed_frame_count = std::min(
        target_frame_count,
        std::max<Frame>(0, total_recorded_frames - trim_start_frame));

    PayloadAlignmentInfo info;
    info.marker_latency_samples = marker_latency;
    if (recorded.sample_rate > 0.0) {
        info.marker_latency_milliseconds =
            (static_cast<double>(marker_latency) / recorded.sample_rate) * 1'000.0;
    }
    info.trim_start_frame = trim_start_frame;
    info.trimmed_frame_count = trimmed_frame_count;
    info.target_frame_count = reference.source_frame_count;

    return PayloadAlignment{
        .audio = extract_aligned_audio(
            recorded, channel_count, trim_start_frame, target_frame_count),
        .info = info,
        .impulse_detection = impulse_detection,
        .warnings = marker_evidence_warnings(
            detected_count,
            expected_count,
            latencies,
            reference.expected_marker_frames,
            detected_impulses),
    };
}

} // namespace capture_panel
