#include "capture_panel/core/audio.hpp"

#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace capture_panel {
namespace {

[[nodiscard]] std::span<const float> clamped_frame_span(
    const AudioBuffer& buffer,
    const std::int64_t start_frame,
    const std::int64_t frame_count) noexcept {
    if (buffer.channel_count == 0 || buffer.samples.empty() || frame_count <= 0) {
        return {};
    }

    const auto total_frames = buffer.frame_count();
    const auto first_frame = std::clamp<std::int64_t>(start_frame, 0, total_frames);

    // Avoid signed overflow when computing start_frame + frame_count.
    const auto last_requested = start_frame
            > std::numeric_limits<std::int64_t>::max() - frame_count
        ? std::numeric_limits<std::int64_t>::max()
        : start_frame + frame_count;
    const auto last_frame = std::clamp<std::int64_t>(last_requested, first_frame, total_frames);

    const auto first_sample = static_cast<std::size_t>(first_frame)
        * static_cast<std::size_t>(buffer.channel_count);
    const auto sample_count = static_cast<std::size_t>(last_frame - first_frame)
        * static_cast<std::size_t>(buffer.channel_count);
    return std::span<const float>(buffer.samples).subspan(first_sample, sample_count);
}

} // namespace

double linear_from_dbfs(const double dbfs) noexcept {
    return std::pow(10.0, dbfs / 20.0);
}

double dbfs_from_peak(const double value) noexcept {
    const auto floor = linear_from_dbfs(silence_dbfs);
    return 20.0 * std::log10(std::max(std::abs(value), floor));
}

double dbfs_from_rms(const double value) noexcept {
    const auto floor = linear_from_dbfs(silence_dbfs);
    return 20.0 * std::log10(std::max(value, floor));
}

double peak(const std::span<const float> samples) noexcept {
    double result = 0.0;
    for (const auto sample : samples) {
        result = std::max(result, std::abs(static_cast<double>(sample)));
    }
    return result;
}

double rms(const std::span<const float> samples) noexcept {
    if (samples.empty()) return 0.0;

    // Accumulation in double avoids the precision loss of a float accumulator.
    double square_sum = 0.0;
    for (const auto sample : samples) {
        const auto value = static_cast<double>(sample);
        square_sum += value * value;
    }
    return std::sqrt(square_sum / static_cast<double>(samples.size()));
}

double peak(
    const AudioBuffer& buffer,
    const std::int64_t start_frame,
    const std::int64_t frame_count) noexcept {
    return peak(clamped_frame_span(buffer, start_frame, frame_count));
}

double rms(
    const AudioBuffer& buffer,
    const std::int64_t start_frame,
    const std::int64_t frame_count) noexcept {
    return rms(clamped_frame_span(buffer, start_frame, frame_count));
}

void apply_gain_db(const std::span<float> samples, const double gain_db) noexcept {
    const auto multiplier = static_cast<float>(linear_from_dbfs(gain_db));
    for (auto& sample : samples) {
        sample *= multiplier;
    }
}

std::vector<float> extract_frames(
    const std::span<const float> samples,
    const std::int64_t start_frame,
    const std::int64_t frame_count,
    const std::uint32_t channel_count,
    const std::int64_t target_frame_count) {
    if (channel_count == 0) {
        throw CaptureError(ErrorCode::validation_failed, "Channel count must be greater than zero");
    }
    if (frame_count < 0 || target_frame_count < 0) {
        throw CaptureError(ErrorCode::validation_failed, "Frame counts cannot be negative");
    }

    const auto channels = static_cast<std::size_t>(channel_count);
    const auto target_frames = static_cast<std::uint64_t>(target_frame_count);
    if (target_frames > std::numeric_limits<std::size_t>::max() / channels) {
        throw CaptureError(ErrorCode::validation_failed, "Requested audio buffer is too large");
    }

    std::vector<float> result(static_cast<std::size_t>(target_frames) * channels, 0.0F);
    const auto copied_frame_count = std::min(frame_count, target_frame_count);
    const auto source_frame_count = static_cast<std::int64_t>(samples.size() / channels);

    for (std::int64_t destination_frame = 0;
         destination_frame < copied_frame_count;
         ++destination_frame) {
        if (start_frame > std::numeric_limits<std::int64_t>::max() - destination_frame) {
            break;
        }
        const auto source_frame = start_frame + destination_frame;
        if (source_frame < 0 || source_frame >= source_frame_count) continue;

        const auto source_offset = static_cast<std::size_t>(source_frame) * channels;
        const auto destination_offset = static_cast<std::size_t>(destination_frame) * channels;
        std::copy_n(
            samples.begin() + static_cast<std::ptrdiff_t>(source_offset),
            channels,
            result.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    }
    return result;
}

AudioBuffer extract_frames(
    const AudioBuffer& buffer,
    const std::int64_t start_frame,
    const std::int64_t frame_count,
    const std::int64_t target_frame_count) {
    AudioBuffer result;
    result.sample_rate = buffer.sample_rate;
    result.channel_count = buffer.channel_count;
    result.samples = extract_frames(
        buffer.samples,
        start_frame,
        frame_count,
        buffer.channel_count,
        target_frame_count);
    return result;
}

} // namespace capture_panel
