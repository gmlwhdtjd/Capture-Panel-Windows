#pragma once

#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/types.hpp"

#include <string_view>

namespace capture_panel {

[[nodiscard]] constexpr std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::file_not_found: return "file_not_found";
    case ErrorCode::wav_read: return "wav_read";
    case ErrorCode::wav_write: return "wav_write";
    case ErrorCode::unsupported_format: return "unsupported_format";
    case ErrorCode::unsupported_bit_depth: return "unsupported_bit_depth";
    case ErrorCode::unsupported_sample_rate: return "unsupported_sample_rate";
    case ErrorCode::device_not_found: return "device_not_found";
    case ErrorCode::invalid_channel_specification: return "invalid_channel_specification";
    case ErrorCode::validation_failed: return "validation_failed";
    case ErrorCode::alignment_failed: return "alignment_failed";
    case ErrorCode::capture_cancelled: return "capture_cancelled";
    case ErrorCode::capture_timed_out: return "capture_timed_out";
    case ErrorCode::backend_failure: return "backend_failure";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view warning_name(CaptureWarning warning) noexcept {
    switch (warning) {
    case CaptureWarning::source_channel_count_mismatch:
        return "source_channel_count_mismatch";
    case CaptureWarning::source_near_digital_full_scale:
        return "source_near_digital_full_scale";
    case CaptureWarning::equipment_decay_may_affect_capture:
        return "equipment_decay_may_affect_capture";
    case CaptureWarning::marker_evidence_low: return "marker_evidence_low";
    case CaptureWarning::alignment_fit_error_high: return "alignment_fit_error_high";
    case CaptureWarning::verification_ambiguous: return "verification_ambiguous";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view failure_name(CaptureFailure failure) noexcept {
    switch (failure) {
    case CaptureFailure::digital_clipping: return "digital_clipping";
    case CaptureFailure::verification_signal_missing: return "verification_signal_missing";
    case CaptureFailure::verification_timing_mismatch: return "verification_timing_mismatch";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view stage_name(CaptureStage stage) noexcept {
    switch (stage) {
    case CaptureStage::sample_rate_configuration: return "sample_rate_configuration";
    case CaptureStage::recording: return "recording";
    case CaptureStage::alignment: return "alignment";
    case CaptureStage::verification: return "verification";
    case CaptureStage::output_writing: return "output_writing";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view event_type_name(CaptureEventType type) noexcept {
    switch (type) {
    case CaptureEventType::started: return "started";
    case CaptureEventType::input_loaded: return "input_loaded";
    case CaptureEventType::warning: return "warning";
    case CaptureEventType::devices_validated: return "devices_validated";
    case CaptureEventType::stage_changed: return "stage_changed";
    case CaptureEventType::recording_progress: return "recording_progress";
    case CaptureEventType::capture_finished: return "capture_finished";
    case CaptureEventType::impulse_detection: return "impulse_detection";
    case CaptureEventType::alignment_finished: return "alignment_finished";
    case CaptureEventType::verification_finished: return "verification_finished";
    case CaptureEventType::output_written: return "output_written";
    }
    return "unknown";
}

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
