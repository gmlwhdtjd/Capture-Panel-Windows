#pragma once

#include "capture_panel/core/streaming.hpp"
#include "capture_panel/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace capture_panel {

/// Reads and validates the RIFF/WAVE container without decoding its payload.
[[nodiscard]] WavFormat read_wav_format(const std::filesystem::path& path);

/// Decodes in bounded chunks, validates the declared payload length, and
/// returns only the running absolute peak.
[[nodiscard]] float wav_peak_level(
    const std::filesystem::path& path,
    const WavFormat& expected_format,
    std::size_t max_frames_per_chunk = default_audio_chunk_frames,
    const std::shared_ptr<CancellationToken>& cancellation = {});

/// Reads RIFF/WAVE PCM16/24/32 or IEEE float32 into normalized interleaved float32.
/// Retained as a bounded-test/convenience helper; the live path uses readers.
[[nodiscard]] WavContents read_wav(const std::filesystem::path& path);

/// Rejects output dimensions that cannot fit classic RIFF/WAVE before a
/// device is opened or a destination is touched.
void validate_wav_output_capacity(
    std::int64_t frame_count,
    double sample_rate,
    std::uint32_t channel_count,
    AudioBitDepth bit_depth);

/// Streams exactly frame_count frames to an atomically promoted sibling WAV.
void write_wav(
    const std::filesystem::path& path,
    IFloat32FrameReader& reader,
    std::int64_t frame_count,
    double sample_rate,
    std::uint32_t channel_count,
    AudioBitDepth bit_depth = AudioBitDepth::pcm24,
    const std::shared_ptr<CancellationToken>& cancellation = {});

/// Writes normalized interleaved float32 as little-endian integer PCM. The WAV
/// is completed in a sibling temporary file and atomically promoted so a failed
/// write never truncates an existing destination.
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
