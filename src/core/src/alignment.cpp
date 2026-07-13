#include "capture_panel/core/alignment.hpp"

#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
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

struct MarkerSequenceEvaluation {
    MarkerSequence sequence;
    Frame median_latency = 0;
    double timing_fit_score = 0.0;
    bool coherent = false;
};

struct SequenceAlignmentScore {
    std::size_t match_count = 0;
    double squared_residual_sum = 0.0;
    double peak_sum = 0.0;
    bool reachable = false;
};

enum class SequenceAlignmentStep : std::uint8_t {
    none,
    skip_expected,
    skip_candidate,
    match,
};

struct SequenceAlignmentCell {
    SequenceAlignmentScore score;
    std::size_t previous_expected = 0;
    std::size_t previous_candidate = 0;
    SequenceAlignmentStep step = SequenceAlignmentStep::none;
};

[[nodiscard]] bool is_better_alignment_score(
    const SequenceAlignmentScore& candidate,
    const SequenceAlignmentScore& current) noexcept {
    if (!candidate.reachable) return false;
    if (!current.reachable) return true;
    if (candidate.match_count != current.match_count) {
        return candidate.match_count > current.match_count;
    }
    if (std::abs(candidate.squared_residual_sum - current.squared_residual_sum)
        > constants::marker::fit_score_epsilon) {
        return candidate.squared_residual_sum < current.squared_residual_sum;
    }
    return candidate.peak_sum > current.peak_sum;
}

void update_alignment_cell(
    SequenceAlignmentCell& destination,
    const SequenceAlignmentScore& score,
    const std::size_t previous_expected,
    const std::size_t previous_candidate,
    const SequenceAlignmentStep step) {
    if (!is_better_alignment_score(score, destination.score)) return;
    destination = {
        .score = score,
        .previous_expected = previous_expected,
        .previous_candidate = previous_candidate,
        .step = step,
    };
}

[[nodiscard]] std::optional<Frame> frame_difference(
    const Frame left,
    const Frame right) noexcept {
    const auto difference = static_cast<long double>(left) - static_cast<long double>(right);
    if (difference < static_cast<long double>(std::numeric_limits<Frame>::min())
        || difference > static_cast<long double>(std::numeric_limits<Frame>::max())) {
        return std::nullopt;
    }
    return static_cast<Frame>(difference);
}

[[nodiscard]] std::optional<MarkerSequenceEvaluation> make_marker_sequence(
    const std::vector<MarkerCandidate>& candidates,
    const std::vector<Frame>& expected_marker_positions,
    const Frame latency,
    const Frame tolerance_frames,
    const double sample_rate) {
    const auto expected_count = expected_marker_positions.size();
    const auto candidate_count = candidates.size();
    const auto column_count = candidate_count + 1U;
    std::vector<SequenceAlignmentCell> cells((expected_count + 1U) * column_count);
    const auto cell = [&](const std::size_t expected, const std::size_t candidate)
        -> SequenceAlignmentCell& {
        return cells[expected * column_count + candidate];
    };
    cell(0, 0).score.reachable = true;

    for (std::size_t expected_index = 0; expected_index <= expected_count;
         ++expected_index) {
        for (std::size_t candidate_index = 0; candidate_index <= candidate_count;
             ++candidate_index) {
            const auto current = cell(expected_index, candidate_index).score;
            if (!current.reachable) continue;

            if (expected_index < expected_count) {
                update_alignment_cell(
                    cell(expected_index + 1U, candidate_index),
                    current,
                    expected_index,
                    candidate_index,
                    SequenceAlignmentStep::skip_expected);
            }
            if (candidate_index < candidate_count) {
                update_alignment_cell(
                    cell(expected_index, candidate_index + 1U),
                    current,
                    expected_index,
                    candidate_index,
                    SequenceAlignmentStep::skip_candidate);
            }
            if (expected_index >= expected_count || candidate_index >= candidate_count) {
                continue;
            }

            const auto measured_latency = frame_difference(
                candidates[candidate_index].frame,
                expected_marker_positions[expected_index]);
            if (!measured_latency.has_value()) continue;
            const auto residual = static_cast<long double>(*measured_latency)
                - static_cast<long double>(latency);
            if (std::abs(residual) > static_cast<long double>(tolerance_frames)) continue;

            auto matched = current;
            ++matched.match_count;
            matched.squared_residual_sum += static_cast<double>(residual * residual);
            matched.peak_sum += static_cast<double>(candidates[candidate_index].peak);
            update_alignment_cell(
                cell(expected_index + 1U, candidate_index + 1U),
                matched,
                expected_index,
                candidate_index,
                SequenceAlignmentStep::match);
        }
    }

    const auto& final_cell = cell(expected_count, candidate_count);
    if (!final_cell.score.reachable
        || final_cell.score.match_count
            < constants::impulse::minimum_impulses_for_analysis) {
        return std::nullopt;
    }

    std::vector<MarkerCandidate> sequence;
    std::vector<std::size_t> expected_indices;
    sequence.reserve(final_cell.score.match_count);
    expected_indices.reserve(final_cell.score.match_count);
    auto expected_cursor = expected_count;
    auto candidate_cursor = candidate_count;
    while (expected_cursor != 0U || candidate_cursor != 0U) {
        const auto& current = cell(expected_cursor, candidate_cursor);
        if (current.step == SequenceAlignmentStep::none) return std::nullopt;
        if (current.step == SequenceAlignmentStep::match) {
            sequence.push_back(candidates[current.previous_candidate]);
            expected_indices.push_back(current.previous_expected);
        }
        expected_cursor = current.previous_expected;
        candidate_cursor = current.previous_candidate;
    }
    std::reverse(sequence.begin(), sequence.end());
    std::reverse(expected_indices.begin(), expected_indices.end());

    std::vector<Frame> frames;
    std::vector<Frame> latencies;
    frames.reserve(sequence.size());
    latencies.reserve(sequence.size());
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        frames.push_back(sequence[index].frame);
        const auto measured_latency = frame_difference(
            sequence[index].frame,
            expected_marker_positions[expected_indices[index]]);
        if (!measured_latency.has_value()) return std::nullopt;
        latencies.push_back(*measured_latency);
    }

    std::vector<double> interval_errors;
    interval_errors.reserve(sequence.size() - 1);
    for (std::size_t index = 1; index < sequence.size(); ++index) {
        const auto measured = frame_difference(frames[index], frames[index - 1]);
        const auto expected = frame_difference(
            expected_marker_positions[expected_indices[index]],
            expected_marker_positions[expected_indices[index - 1]]);
        if (!measured.has_value() || !expected.has_value()) return std::nullopt;
        interval_errors.push_back(
            static_cast<double>(*measured) - static_cast<double>(*expected));
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
    const auto latency_spread = standard_deviation(latencies);
    const auto interval_error_mean = mean(absolute_interval_errors);
    const auto interval_error_spread = standard_deviation(interval_errors);
    const auto timing_fit_score = latency_spread
        + interval_error_mean
        + interval_error_spread;
    const auto fit_score = timing_fit_score
        + negative_latency_penalty
        + late_echo_tie_breaker
        + missing_marker_penalty;
    const auto average_peak = marker_sequence_average_peak(sequence);
    const auto coherence_limit = constants::impulse::marker_latency_spread_threshold;

    return MarkerSequenceEvaluation{
        .sequence = MarkerSequence{
            .candidates = std::move(sequence),
            .expected_indices = std::move(expected_indices),
            .fit_score = fit_score,
            .average_peak = average_peak,
        },
        .median_latency = median_latency,
        .timing_fit_score = timing_fit_score,
        .coherent = latency_spread <= coherence_limit
            && interval_error_mean <= coherence_limit
            && interval_error_spread <= coherence_limit,
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

[[nodiscard]] bool has_strong_direct_evidence(
    const MarkerSequenceEvaluation& evaluation,
    const std::size_t expected_count) noexcept {
    if (!evaluation.coherent || evaluation.median_latency < 0 || expected_count == 0) {
        return false;
    }
    const auto evidence_ratio = static_cast<double>(evaluation.sequence.candidates.size())
        / static_cast<double>(expected_count);
    return evidence_ratio + constants::marker::fit_score_epsilon
        >= constants::marker::warning_detected_marker_ratio;
}

[[nodiscard]] bool is_better_direct_sequence_in_latency_band(
    const MarkerSequenceEvaluation& candidate,
    const MarkerSequenceEvaluation& current) noexcept {
    if (candidate.sequence.candidates.size() != current.sequence.candidates.size()) {
        return candidate.sequence.candidates.size() > current.sequence.candidates.size();
    }
    if (std::abs(candidate.timing_fit_score - current.timing_fit_score)
        > constants::marker::fit_score_epsilon) {
        return candidate.timing_fit_score < current.timing_fit_score;
    }
    if (is_better_marker_sequence(candidate.sequence, current.sequence)) return true;
    if (is_better_marker_sequence(current.sequence, candidate.sequence)) return false;
    return candidate.median_latency < current.median_latency;
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
    const MarkerSequence& sequence,
    const std::vector<Frame>& expected) {
    std::vector<Frame> result;
    result.reserve(sequence.candidates.size());
    for (std::size_t index = 0; index < sequence.candidates.size(); ++index) {
        if (index >= sequence.expected_indices.size()
            || sequence.expected_indices[index] >= expected.size()) {
            return {};
        }
        const auto latency = frame_difference(
            sequence.candidates[index].frame,
            expected[sequence.expected_indices[index]]);
        if (!latency.has_value()) return {};
        result.push_back(*latency);
    }
    return result;
}

[[nodiscard]] std::vector<Frame> matched_expected_positions(
    const MarkerSequence& sequence,
    const std::vector<Frame>& expected) {
    std::vector<Frame> result;
    result.reserve(sequence.expected_indices.size());
    for (const auto index : sequence.expected_indices) {
        if (index >= expected.size()) return {};
        result.push_back(expected[index]);
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
    if (expected_count == 0
        || expected_count > constants::alignment::impulse_count
        || candidates.empty()
        || candidates.size() > constants::marker::maximum_sequence_candidates
        || !std::isfinite(sample_rate) || sample_rate <= 0.0
        || !std::is_sorted(expected_marker_positions.begin(), expected_marker_positions.end())
        || std::adjacent_find(
               expected_marker_positions.begin(), expected_marker_positions.end())
            != expected_marker_positions.end()) {
        return std::nullopt;
    }

    auto sorted_candidates = candidates;
    std::sort(
        sorted_candidates.begin(), sorted_candidates.end(),
        [](const MarkerCandidate& left, const MarkerCandidate& right) {
            if (left.frame == right.frame) return left.peak > right.peak;
            return left.frame < right.frame;
        });
    sorted_candidates.erase(
        std::unique(
            sorted_candidates.begin(),
            sorted_candidates.end(),
            [](const MarkerCandidate& left, const MarkerCandidate& right) {
                return left.frame == right.frame;
            }),
        sorted_candidates.end());
    const auto tolerance_frames = sequence_match_tolerance_frames(sample_rate);
    std::unordered_set<Frame> evaluated_latencies;
    std::optional<MarkerSequence> best_sequence;
    std::vector<MarkerSequenceEvaluation> direct_sequences;

    for (const auto& candidate : sorted_candidates) {
        for (const auto expected_marker : expected_marker_positions) {
            const auto latency = frame_difference(candidate.frame, expected_marker);
            if (!latency.has_value()) continue;
            if (!evaluated_latencies.insert(*latency).second) continue;
            auto sequence = make_marker_sequence(
                sorted_candidates,
                expected_marker_positions,
                *latency,
                tolerance_frames,
                sample_rate);
            if (sequence.has_value()
                && is_better_marker_sequence(sequence->sequence, best_sequence)) {
                best_sequence = sequence->sequence;
            }
            if (sequence.has_value()
                && has_strong_direct_evidence(*sequence, expected_count)) {
                direct_sequences.push_back(std::move(*sequence));
            }
        }
    }

    if (!direct_sequences.empty()) {
        // A later echo can contain every marker even when the causal/direct
        // train lost one. Do not make absolute latency part of signal quality:
        // first locate the earliest stable, sufficiently complete non-negative
        // timing hypothesis, then compare evidence within the existing match
        // tolerance as one latency band. This still lets a full nearby train
        // beat a partial one without allowing a distinct delayed echo to win
        // solely because it contains one extra marker.
        const auto earliest = std::min_element(
            direct_sequences.begin(), direct_sequences.end(),
            [](const MarkerSequenceEvaluation& left,
               const MarkerSequenceEvaluation& right) {
                return left.median_latency < right.median_latency;
            })->median_latency;
        std::optional<std::size_t> best_direct_index;
        for (std::size_t index = 0; index < direct_sequences.size(); ++index) {
            const auto relative_latency = frame_difference(
                direct_sequences[index].median_latency, earliest);
            if (!relative_latency.has_value() || *relative_latency > tolerance_frames) {
                continue;
            }
            if (!best_direct_index.has_value()
                || is_better_direct_sequence_in_latency_band(
                    direct_sequences[index], direct_sequences[*best_direct_index])) {
                best_direct_index = index;
            }
        }
        if (best_direct_index.has_value()) {
            return std::move(direct_sequences[*best_direct_index].sequence);
        }
    }
    return best_sequence;
}

CapturePassPlaybackPlan make_alignment_playback_plan(
    const CaptureAudioSource& source,
    const float playback_gain) {
    if (!source.valid() || source.format().total_frames <= 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Alignment playback source must contain at least one frame and channel.");
    }
    const auto sample_rate = source.format().sample_rate;
    const auto source_frame_count = source.format().total_frames;
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
    if (source_start_frame > std::numeric_limits<Frame>::max() - source_frame_count) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Alignment playback duration is too large.");
    }
    const auto playback_frame_count = source_start_frame + source_frame_count;

    return CapturePassPlaybackPlan{
        .source = source,
        .playback_frame_count = playback_frame_count,
        .marker_frames = std::move(marker_frames),
        .source_start_frame = source_start_frame,
        .playback_gain = gain,
    };
}

CapturePassPlaybackPlan make_alignment_playback_plan(
    const AudioBuffer& source,
    const float playback_gain) {
    return make_alignment_playback_plan(
        CaptureAudioSource::from_memory(source), playback_gain);
}

AudioBuffer materialize_playback_plan(const CapturePassPlaybackPlan& plan) {
    if (!plan.source.valid() || plan.playback_frame_count <= 0
        || plan.source_start_frame < 0
        || plan.source_start_frame > plan.playback_frame_count
        || plan.source.format().total_frames
            != plan.playback_frame_count - plan.source_start_frame) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Playback plan descriptor is invalid.");
    }
    const auto channels = plan.channel_count();
    if (static_cast<std::uint64_t>(plan.playback_frame_count)
        > std::numeric_limits<std::size_t>::max() / channels) {
        throw CaptureError(ErrorCode::validation_failed, "Playback plan is too large.");
    }
    AudioBuffer result{
        .sample_rate = plan.sample_rate(),
        .channel_count = channels,
        .samples = std::vector<float>(
            static_cast<std::size_t>(plan.playback_frame_count) * channels,
            0.0F),
    };
    const auto gain = std::isfinite(plan.playback_gain) ? plan.playback_gain : 1.0F;
    const auto marker_amplitude = static_cast<float>(std::pow(
        10.0, constants::alignment::impulse_level_dbfs / 20.0)) * gain;
    for (const auto marker : plan.marker_frames) {
        if (marker < 0 || marker >= plan.source_start_frame) {
            throw CaptureError(ErrorCode::validation_failed, "Playback marker is out of range.");
        }
        const auto offset = static_cast<std::size_t>(marker) * channels;
        std::fill_n(result.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                    channels, marker_amplitude);
    }

    auto reader = plan.source.make_reader();
    std::int64_t frames_read = 0;
    while (frames_read < plan.source.format().total_frames) {
        const auto destination_frame = plan.source_start_frame + frames_read;
        const auto destination_sample = static_cast<std::size_t>(destination_frame) * channels;
        const auto read = reader->read_frames(
            std::span<float>(result.samples).subspan(destination_sample));
        if (read <= 0 || read > plan.source.format().total_frames - frames_read) {
            throw CaptureError(
                ErrorCode::source_stream_failure,
                "Playback source ended before its declared frame count.");
        }
        const auto samples_read = static_cast<std::size_t>(read) * channels;
        if (gain != 1.0F) {
            for (auto& sample : std::span<float>(result.samples).subspan(
                     destination_sample, samples_read)) {
                sample *= gain;
            }
        }
        frames_read += read;
    }
    return result;
}

PayloadAlignment align_payload(
    const Float32AudioAsset& recorded,
    const float recording_gain,
    const AlignmentReference& reference) {
    if (!recorded.valid() || reference.source_frame_count < 0
        || reference.source_start_frame < 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Capture alignment asset or reference is invalid.");
    }
    const auto channel_count = recorded.channel_count();
    const auto total_recorded_frames = recorded.frame_count();
    const auto search_range = marker_search_frame_range(
        reference.expected_marker_frames,
        reference,
        total_recorded_frames,
        recorded.sample_rate());

    // Marker detection only needs the absolute peak of each frame. Collapse
    // channels while reading bounded chunks instead of materializing the full
    // interleaved marker window (which scales poorly with large ASIO routes).
    std::vector<float> marker_frame_peaks;
    if (!search_range.empty()) {
        const auto window_frames = search_range.upper - search_range.lower;
        if (window_frames < 0
            || static_cast<std::uint64_t>(window_frames)
                > std::numeric_limits<std::size_t>::max()) {
            throw CaptureError(
                ErrorCode::validation_failed,
                "Capture marker analysis window is too large.");
        }
        marker_frame_peaks.resize(static_cast<std::size_t>(window_frames));
        auto reader = recorded.make_reader(search_range.lower);
        constexpr std::size_t preferred_marker_chunk_samples = 1U << 20U;
        const auto chunk_frames = std::max<std::size_t>(
            1,
            std::min<std::size_t>(
                default_audio_chunk_frames,
                preferred_marker_chunk_samples / channel_count));
        if (chunk_frames > std::numeric_limits<std::size_t>::max() / channel_count) {
            throw CaptureError(
                ErrorCode::validation_failed,
                "Capture marker read chunk is too large.");
        }
        std::vector<float> chunk(chunk_frames * channel_count);
        Frame frames_read = 0;
        while (frames_read < window_frames) {
            const auto requested = static_cast<std::size_t>(std::min<Frame>(
                window_frames - frames_read,
                static_cast<Frame>(chunk_frames)));
            std::size_t filled = 0;
            while (filled < requested) {
                const auto sample_offset = filled * channel_count;
                const auto read = reader->read_frames(
                    std::span<float>(chunk).subspan(
                        sample_offset,
                        (requested - filled) * channel_count));
                if (read <= 0
                    || static_cast<std::uint64_t>(read) > requested - filled) {
                    throw CaptureError(
                        ErrorCode::backend_failure,
                        "Recorded marker window ended unexpectedly.");
                }
                filled += static_cast<std::size_t>(read);
            }
            const auto gain = std::abs(
                std::isfinite(recording_gain) ? recording_gain : 1.0F);
            for (std::size_t frame = 0; frame < requested; ++frame) {
                float frame_peak_level = 0.0F;
                const auto sample_offset = frame * channel_count;
                for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
                    const auto sample = chunk[sample_offset + channel];
                    if (!std::isfinite(sample)) {
                        throw CaptureError(
                            ErrorCode::backend_failure,
                            "Recorded marker window contains NaN or infinity.");
                    }
                    frame_peak_level = std::max(frame_peak_level, std::abs(sample));
                }
                marker_frame_peaks[static_cast<std::size_t>(frames_read) + frame] =
                    frame_peak_level * gain;
            }
            frames_read += static_cast<Frame>(requested);
        }
    }
    auto marker_candidates = detect_marker_impulse_candidates(
        marker_frame_peaks,
        1,
        {.lower = 0, .upper = static_cast<Frame>(
            marker_frame_peaks.size())});
    for (auto& candidate : marker_candidates) candidate.frame += search_range.lower;
    const auto selected_sequence = select_marker_sequence(
        marker_candidates,
        reference.expected_marker_frames,
        recorded.sample_rate());
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
    if (!selected_sequence.has_value()
        || detected_count < constants::impulse::minimum_impulses_for_analysis
        || evidence_ratio < constants::marker::minimum_detected_marker_ratio) {
        throw CaptureError(
            ErrorCode::alignment_failed,
            "Marker-based alignment failed: the recorded markers could not be found "
            "with enough evidence.");
    }

    const auto latencies = marker_latencies(
        *selected_sequence, reference.expected_marker_frames);
    const auto matched_expected = matched_expected_positions(
        *selected_sequence, reference.expected_marker_frames);
    if (latencies.size() != detected_impulses.size()
        || matched_expected.size() != detected_impulses.size()) {
        throw CaptureError(
            ErrorCode::alignment_failed,
            "Marker-based alignment failed: the marker mapping was inconsistent.");
    }
    const auto marker_latency = median(latencies);
    const auto trim_start_frame = std::max<Frame>(
        0, reference.source_start_frame + marker_latency);
    const auto target_frame_count = std::max<Frame>(0, reference.source_frame_count);
    const auto trimmed_frame_count = std::min(
        target_frame_count,
        std::max<Frame>(0, total_recorded_frames - trim_start_frame));

    PayloadAlignmentInfo info;
    info.marker_latency_samples = marker_latency;
    if (recorded.sample_rate() > 0.0) {
        info.marker_latency_milliseconds =
            (static_cast<double>(marker_latency) / recorded.sample_rate()) * 1'000.0;
    }
    info.trim_start_frame = trim_start_frame;
    info.trimmed_frame_count = trimmed_frame_count;
    info.target_frame_count = reference.source_frame_count;

    return PayloadAlignment{
        .payload = AlignedCapturePayload{
            .asset = recorded,
            .start_frame = trim_start_frame,
            .frame_count = target_frame_count,
            .gain = std::isfinite(recording_gain) ? recording_gain : 1.0F,
        },
        .info = info,
        .impulse_detection = impulse_detection,
        .warnings = marker_evidence_warnings(
            detected_count,
            expected_count,
            latencies,
            matched_expected,
            detected_impulses),
    };
}

PayloadAlignment align_payload(
    const AudioBuffer& recorded,
    const AlignmentReference& reference) {
    return align_payload(Float32AudioAsset::from_memory(recorded), 1.0F, reference);
}

} // namespace capture_panel
