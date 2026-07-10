#pragma once

#include "capture_panel/core/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace capture_panel {

/// Floor used when a zero-amplitude value is represented in dBFS.
inline constexpr double silence_dbfs = -120.0;

/// Converts a dBFS/dB gain value to a linear amplitude multiplier.
[[nodiscard]] double linear_from_dbfs(double dbfs) noexcept;

/// Converts an absolute peak amplitude to dBFS, with silence floored at -120 dBFS.
[[nodiscard]] double dbfs_from_peak(double peak) noexcept;

/// Converts an RMS amplitude to dBFS, with silence floored at -120 dBFS.
[[nodiscard]] double dbfs_from_rms(double rms) noexcept;

/// Returns the greatest absolute sample value in normalized interleaved float audio.
[[nodiscard]] double peak(std::span<const float> samples) noexcept;

/// Returns the root-mean-square value of normalized interleaved float audio.
[[nodiscard]] double rms(std::span<const float> samples) noexcept;

/// Computes the peak over a clamped frame range in an AudioBuffer.
[[nodiscard]] double peak(
    const AudioBuffer& buffer,
    std::int64_t start_frame,
    std::int64_t frame_count) noexcept;

/// Computes RMS over a clamped frame range in an AudioBuffer.
[[nodiscard]] double rms(
    const AudioBuffer& buffer,
    std::int64_t start_frame,
    std::int64_t frame_count) noexcept;

/// Applies an unclipped dB gain to normalized float samples in place.
void apply_gain_db(std::span<float> samples, double gain_db) noexcept;

/// Extracts interleaved frames and zero-fills source positions outside the input.
/// target_frame_count controls the output length and may differ from frame_count.
[[nodiscard]] std::vector<float> extract_frames(
    std::span<const float> samples,
    std::int64_t start_frame,
    std::int64_t frame_count,
    std::uint32_t channel_count,
    std::int64_t target_frame_count);

/// AudioBuffer convenience overload. The returned buffer preserves format metadata.
[[nodiscard]] AudioBuffer extract_frames(
    const AudioBuffer& buffer,
    std::int64_t start_frame,
    std::int64_t frame_count,
    std::int64_t target_frame_count);

} // namespace capture_panel
