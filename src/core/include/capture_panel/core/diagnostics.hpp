#pragma once

#include "capture_panel/core/types.hpp"

#include <string_view>

namespace capture_panel {

[[nodiscard]] constexpr std::string_view warning_message(CaptureWarning warning) noexcept {
    switch (warning) {
    case CaptureWarning::source_channel_count_mismatch:
        return "The source and playback route channel counts differ; channels are mapped in order.";
    case CaptureWarning::source_near_digital_full_scale:
        return "The source is at or near digital full scale.";
    case CaptureWarning::equipment_decay_may_affect_capture:
        return "The device response may linger into the capture.";
    case CaptureWarning::marker_evidence_low:
        return "Start marker evidence is low.";
    case CaptureWarning::alignment_fit_error_high:
        return "Start detection error is high.";
    case CaptureWarning::verification_ambiguous:
        return "The test signal overlaps the device response; measurement is ambiguous.";
    }
    return "Unknown capture warning.";
}

[[nodiscard]] constexpr std::string_view failure_message(CaptureFailure failure) noexcept {
    switch (failure) {
    case CaptureFailure::digital_clipping:
        return "Clipping was detected.";
    case CaptureFailure::verification_signal_missing:
        return "The test signal was not found.";
    case CaptureFailure::verification_timing_mismatch:
        return "The test signal timing does not match.";
    }
    return "Unknown capture failure.";
}

[[nodiscard]] constexpr std::string_view reliability_name(
    VerificationReliability reliability) noexcept {
    switch (reliability) {
    case VerificationReliability::reliable: return "reliable";
    case VerificationReliability::ambiguous: return "ambiguous";
    case VerificationReliability::unmeasurable: return "unmeasurable";
    }
    return "unknown";
}

} // namespace capture_panel
