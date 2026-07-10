#pragma once

#include "capture_panel/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace capture_panel {

/// Reads RIFF/WAVE PCM16/24/32 or IEEE float32 into normalized interleaved float32.
[[nodiscard]] WavContents read_wav(const std::filesystem::path& path);

/// Writes normalized interleaved float32 as little-endian integer PCM.
void write_wav(
    const std::filesystem::path& path,
    std::span<const float> samples,
    double sample_rate,
    std::uint32_t channel_count,
    AudioBitDepth bit_depth = AudioBitDepth::pcm24);

/// AudioBuffer convenience overload.
void write_wav(
    const std::filesystem::path& path,
    const AudioBuffer& audio,
    AudioBitDepth bit_depth = AudioBitDepth::pcm24);

} // namespace capture_panel
