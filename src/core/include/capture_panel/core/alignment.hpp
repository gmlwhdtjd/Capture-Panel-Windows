#pragma once

#include "capture_panel/core/types.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace capture_panel {

struct MarkerCandidate {
    std::int64_t frame = 0;
    float peak = 0.0F;
};

struct MarkerSequence {
    std::vector<MarkerCandidate> candidates;
    double fit_score = 0.0;
    double average_peak = 0.0;

    [[nodiscard]] std::vector<std::int64_t> frames() const;
};

struct AlignmentPlaybackPlan {
    AudioBuffer audio;
    std::int64_t playback_frame_count = 0;
    std::vector<std::int64_t> marker_frames;
    std::int64_t source_start_frame = 0;
};

struct AlignmentReference {
    std::vector<std::int64_t> expected_marker_frames;
    std::int64_t source_start_frame = 0;
    std::int64_t source_frame_count = 0;
};

struct PayloadAlignment {
    AudioBuffer audio;
    PayloadAlignmentInfo info;
    std::optional<ImpulseDetection> impulse_detection;
    std::vector<CaptureWarning> warnings;
};

[[nodiscard]] std::optional<MarkerSequence> select_marker_sequence(
    const std::vector<MarkerCandidate>& candidates,
    const std::vector<std::int64_t>& expected_marker_positions,
    double sample_rate);

[[nodiscard]] AlignmentPlaybackPlan make_alignment_playback_plan(
    const AudioBuffer& source,
    float playback_gain = 1.0F);

// Throws CaptureError{ErrorCode::alignment_failed} when marker evidence is insufficient.
[[nodiscard]] PayloadAlignment align_payload(
    const AudioBuffer& recorded,
    const AlignmentReference& reference);

} // namespace capture_panel
