#pragma once

#include "capture_panel/core/streaming.hpp"
#include "capture_panel/core/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace capture_panel {

struct MarkerCandidate {
    std::int64_t frame = 0;
    float peak = 0.0F;
};

struct MarkerSequence {
    std::vector<MarkerCandidate> candidates;
    // Index of the expected marker paired with each candidate. Keeping this
    // mapping is essential when the first or a middle marker is missing.
    std::vector<std::size_t> expected_indices;
    double fit_score = 0.0;
    double average_peak = 0.0;

    [[nodiscard]] std::vector<std::int64_t> frames() const;
};

struct AlignmentReference {
    std::vector<std::int64_t> expected_marker_frames;
    std::int64_t source_start_frame = 0;
    std::int64_t source_frame_count = 0;
};

struct PayloadAlignment {
    AlignedCapturePayload payload;
    PayloadAlignmentInfo info;
    std::optional<ImpulseDetection> impulse_detection;
    std::vector<CaptureWarning> warnings;
};

[[nodiscard]] std::optional<MarkerSequence> select_marker_sequence(
    const std::vector<MarkerCandidate>& candidates,
    const std::vector<std::int64_t>& expected_marker_positions,
    double sample_rate);

[[nodiscard]] CapturePassPlaybackPlan make_alignment_playback_plan(
    const CaptureAudioSource& source,
    float playback_gain = 1.0F);

// Convenience overload retained for the in-memory setup-verification signal
// and focused tests. It still produces a descriptor rather than a prebuilt
// marker+payload buffer.
[[nodiscard]] CapturePassPlaybackPlan make_alignment_playback_plan(
    const AudioBuffer& source,
    float playback_gain = 1.0F);

/// Bounded-test/compatibility helper. Live capture backends consume the plan
/// through a streaming source reader and must not materialize arbitrary files.
[[nodiscard]] AudioBuffer materialize_playback_plan(
    const CapturePassPlaybackPlan& plan);

// Throws CaptureError{ErrorCode::alignment_failed} when marker evidence is insufficient.
[[nodiscard]] PayloadAlignment align_payload(
    const Float32AudioAsset& recorded,
    float recording_gain,
    const AlignmentReference& reference,
    const std::shared_ptr<CancellationToken>& cancellation = {});

// Compatibility overload for bounded in-memory tests.
[[nodiscard]] PayloadAlignment align_payload(
    const AudioBuffer& recorded,
    const AlignmentReference& reference,
    const std::shared_ptr<CancellationToken>& cancellation = {});

} // namespace capture_panel
