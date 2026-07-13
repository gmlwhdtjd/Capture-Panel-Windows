#pragma once

#include "capture_panel/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace capture_panel {

inline constexpr std::size_t default_audio_chunk_frames = 8'192;

/// Pull-based interleaved Float32 reader. The supplied span must contain a
/// whole number of frames. A zero return value means end of stream.
class IFloat32FrameReader {
public:
    virtual ~IFloat32FrameReader() = default;
    [[nodiscard]] virtual std::uint32_t channel_count() const noexcept = 0;
    virtual std::int64_t read_frames(std::span<float> interleaved_samples) = 0;
};

/// Immutable, cheaply-copyable source descriptor. WAV sources reopen the file
/// for each reader; memory sources retain their backing AudioBuffer.
class CaptureAudioSource {
public:
    CaptureAudioSource() = default;

    [[nodiscard]] static CaptureAudioSource from_wav(
        std::filesystem::path path,
        WavFormat format);
    [[nodiscard]] static CaptureAudioSource from_memory(AudioBuffer audio);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const WavFormat& format() const noexcept { return format_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::unique_ptr<IFloat32FrameReader> make_reader(
        std::int64_t start_frame = 0) const;
    [[nodiscard]] float validated_peak_level(
        std::size_t max_frames_per_chunk = default_audio_chunk_frames,
        const std::shared_ptr<CancellationToken>& cancellation = {}) const;
    /// Revalidates the immutable file snapshot captured when a WAV descriptor
    /// was created. Memory sources are always stable.
    void validate_identity() const;

private:
    using ReaderFactory = std::function<
        std::unique_ptr<IFloat32FrameReader>(std::int64_t)>;
    using IdentityValidator = std::function<void()>;

    CaptureAudioSource(
        WavFormat format,
        std::optional<std::filesystem::path> path,
        ReaderFactory reader_factory,
        IdentityValidator identity_validator = {});

    WavFormat format_{};
    std::optional<std::filesystem::path> path_;
    ReaderFactory reader_factory_;
    IdentityValidator identity_validator_;
};

/// A recorded interleaved Float32 asset. Temporary-file ownership is shared,
/// and the file is removed when the last asset/payload reference is released.
class Float32AudioAsset {
public:
    Float32AudioAsset() = default;

    [[nodiscard]] static Float32AudioAsset from_memory(AudioBuffer audio);
    [[nodiscard]] static Float32AudioAsset from_temporary_file(
        std::filesystem::path path,
        double sample_rate,
        std::uint32_t channel_count,
        std::int64_t frame_count,
        float raw_peak,
        bool owns_file = true);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::uint32_t channel_count() const noexcept { return channel_count_; }
    [[nodiscard]] std::int64_t frame_count() const noexcept { return frame_count_; }
    [[nodiscard]] std::optional<float> raw_peak() const noexcept { return raw_peak_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::unique_ptr<IFloat32FrameReader> make_reader(
        std::int64_t start_frame = 0) const;

    /// Scans exactly the declared frame count, rejecting truncation and any
    /// NaN/infinity from an untrusted backend while retaining bounded memory.
    [[nodiscard]] float validated_peak_level(
        std::size_t max_frames_per_chunk = default_audio_chunk_frames,
        const std::shared_ptr<CancellationToken>& cancellation = {}) const;

private:
    using ReaderFactory = std::function<
        std::unique_ptr<IFloat32FrameReader>(std::int64_t)>;

    Float32AudioAsset(
        double sample_rate,
        std::uint32_t channel_count,
        std::int64_t frame_count,
        std::optional<float> raw_peak,
        std::optional<std::filesystem::path> path,
        ReaderFactory reader_factory,
        std::shared_ptr<void> lifetime);

    double sample_rate_ = 0.0;
    std::uint32_t channel_count_ = 0;
    std::int64_t frame_count_ = 0;
    std::optional<float> raw_peak_;
    std::optional<std::filesystem::path> path_;
    ReaderFactory reader_factory_;
    std::shared_ptr<void> lifetime_;
};

struct AlignedCapturePayload {
    Float32AudioAsset asset;
    std::int64_t start_frame = 0;
    std::int64_t frame_count = 0;
    float gain = 1.0F;

    [[nodiscard]] std::uint32_t channel_count() const noexcept {
        return asset.channel_count();
    }
    [[nodiscard]] std::unique_ptr<IFloat32FrameReader> make_reader() const;
    [[nodiscard]] AudioBuffer materialize(
        std::size_t max_frames_per_chunk = default_audio_chunk_frames) const;
};

struct CapturePassPlaybackPlan {
    CaptureAudioSource source;
    std::int64_t playback_frame_count = 0;
    std::vector<std::int64_t> marker_frames;
    std::int64_t source_start_frame = 0;
    float playback_gain = 1.0F;

    [[nodiscard]] double sample_rate() const noexcept {
        return source.format().sample_rate;
    }
    [[nodiscard]] std::uint32_t channel_count() const noexcept {
        return source.format().channel_count;
    }
};

} // namespace capture_panel
