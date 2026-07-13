#include "capture_panel/core/streaming.hpp"

#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace capture_panel {
namespace {

constexpr std::size_t preferred_chunk_samples = 1U << 20U;

[[noreturn]] void throw_invalid_stream(const std::string& detail) {
    throw CaptureError(ErrorCode::validation_failed, "Invalid audio stream: " + detail);
}

[[nodiscard]] std::size_t checked_sample_count(
    const std::int64_t frames,
    const std::uint32_t channels,
    const char* detail) {
    if (frames < 0 || channels == 0
        || static_cast<std::uint64_t>(frames)
            > std::numeric_limits<std::size_t>::max() / channels) {
        throw_invalid_stream(detail);
    }
    return static_cast<std::size_t>(frames) * channels;
}

[[nodiscard]] std::size_t bounded_chunk_frames(
    const std::size_t requested_frames,
    const std::uint32_t channels) {
    if (requested_frames == 0 || channels == 0) {
        throw_invalid_stream("audio chunk dimensions are invalid");
    }
    const auto sample_bounded_frames = std::max<std::size_t>(
        1, preferred_chunk_samples / channels);
    const auto frames = std::min(requested_frames, sample_bounded_frames);
    if (frames > std::numeric_limits<std::size_t>::max() / channels) {
        throw_invalid_stream("audio chunk sample count is too large");
    }
    return frames;
}

void validate_audio_buffer(const AudioBuffer& audio, const char* label) {
    if (!std::isfinite(audio.sample_rate) || audio.sample_rate <= 0.0
        || audio.channel_count == 0
        || audio.samples.size() % audio.channel_count != 0) {
        throw_invalid_stream(std::string(label) + " metadata or frame alignment is invalid");
    }
}

class MemoryFrameReader final : public IFloat32FrameReader {
public:
    MemoryFrameReader(std::shared_ptr<const AudioBuffer> audio, const std::int64_t start_frame)
        : audio_(std::move(audio)) {
        if (start_frame < 0 || start_frame > audio_->frame_count()) {
            throw_invalid_stream("memory reader start frame is outside the asset");
        }
        sample_offset_ = checked_sample_count(
            start_frame, audio_->channel_count, "memory reader offset is too large");
    }

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return audio_->channel_count;
    }

    std::int64_t read_frames(std::span<float> destination) override {
        if (destination.size() % audio_->channel_count != 0) {
            throw_invalid_stream("reader buffer is not frame-aligned");
        }
        const auto remaining = audio_->samples.size() - sample_offset_;
        const auto samples = std::min(destination.size(), remaining);
        if (samples == 0) return 0;
        std::copy_n(audio_->samples.data() + sample_offset_, samples, destination.data());
        sample_offset_ += samples;
        return static_cast<std::int64_t>(samples / audio_->channel_count);
    }

private:
    std::shared_ptr<const AudioBuffer> audio_;
    std::size_t sample_offset_ = 0;
};

class RawFloat32FileReader final : public IFloat32FrameReader {
public:
    RawFloat32FileReader(
        const std::filesystem::path& path,
        const std::uint32_t channel_count,
        const std::int64_t frame_count,
        const std::int64_t start_frame,
        std::shared_ptr<void> lifetime)
        : path_(path), channel_count_(channel_count), lifetime_(std::move(lifetime)) {
        static_assert(std::endian::native == std::endian::little,
            "Capture Panel raw Float32 assets use little-endian samples");
        if (channel_count == 0 || frame_count < 0 || start_frame < 0
            || start_frame > frame_count) {
            throw_invalid_stream("raw file reader range is invalid");
        }
        const auto sample_offset = checked_sample_count(
            start_frame, channel_count, "raw file reader offset is too large");
        if (sample_offset > std::numeric_limits<std::uint64_t>::max() / sizeof(float)) {
            throw_invalid_stream("raw file byte offset is too large");
        }
        remaining_frames_ = frame_count - start_frame;
        stream_.open(path_, std::ios::binary);
        if (!stream_) {
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "Unable to reopen temporary recording: " + path_.string());
        }
        const auto byte_offset = static_cast<std::uint64_t>(sample_offset) * sizeof(float);
        if (byte_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw_invalid_stream("raw file byte offset exceeds stream limits");
        }
        stream_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
        if (!stream_) {
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "Unable to seek within temporary recording: " + path_.string());
        }
    }

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return channel_count_;
    }

    std::int64_t read_frames(std::span<float> destination) override {
        if (destination.size() % channel_count_ != 0) {
            throw_invalid_stream("reader buffer is not frame-aligned");
        }
        const auto requested_frames = std::min<std::int64_t>(
            remaining_frames_,
            static_cast<std::int64_t>(destination.size() / channel_count_));
        if (requested_frames <= 0) return 0;
        const auto sample_count = checked_sample_count(
            requested_frames, channel_count_, "raw read size is too large");
        if (sample_count > static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max() / sizeof(float))) {
            throw_invalid_stream("raw read byte count exceeds stream limits");
        }
        const auto byte_count = static_cast<std::streamsize>(sample_count * sizeof(float));
        stream_.read(reinterpret_cast<char*>(destination.data()), byte_count);
        if (stream_.gcount() != byte_count) {
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "Temporary recording is truncated: " + path_.string());
        }
        remaining_frames_ -= requested_frames;
        return requested_frames;
    }

private:
    std::filesystem::path path_;
    std::uint32_t channel_count_ = 0;
    std::int64_t remaining_frames_ = 0;
    // A reader may outlive the descriptor used to create it. Retain the raw
    // file's owner until the stream itself is closed. This member deliberately
    // precedes stream_: reverse member destruction closes the Windows file
    // handle before the lifetime owner tries to delete the scratch file.
    std::shared_ptr<void> lifetime_;
    std::ifstream stream_;
};

struct TemporaryFileLifetime final {
    explicit TemporaryFileLifetime(std::filesystem::path value)
        : path(std::move(value)) {}

    ~TemporaryFileLifetime() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

class AlignedPayloadReader final : public IFloat32FrameReader {
public:
    explicit AlignedPayloadReader(const AlignedCapturePayload& payload)
        : reader_(payload.asset.make_reader(payload.start_frame)),
          channel_count_(payload.channel_count()),
          remaining_frames_(payload.frame_count),
          gain_(std::isfinite(payload.gain) ? payload.gain : 1.0F) {
        if (channel_count_ == 0 || remaining_frames_ < 0 || payload.start_frame < 0) {
            throw_invalid_stream("aligned payload range is invalid");
        }
    }

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return channel_count_;
    }

    std::int64_t read_frames(std::span<float> destination) override {
        if (destination.size() % channel_count_ != 0) {
            throw_invalid_stream("aligned reader buffer is not frame-aligned");
        }
        const auto requested_frames = std::min<std::int64_t>(
            remaining_frames_,
            static_cast<std::int64_t>(destination.size() / channel_count_));
        if (requested_frames <= 0) return 0;
        const auto requested_samples = checked_sample_count(
            requested_frames, channel_count_, "aligned read size is too large");
        auto requested = destination.first(requested_samples);
        std::int64_t frames_read = 0;
        if (!source_ended_) {
            while (frames_read < requested_frames) {
                const auto sample_offset = checked_sample_count(
                    frames_read, channel_count_, "aligned read offset is too large");
                const auto read = reader_->read_frames(requested.subspan(sample_offset));
                if (read < 0 || read > requested_frames - frames_read) {
                    throw CaptureError(
                        ErrorCode::backend_failure,
                        "Aligned payload source violated the frame-reader contract.");
                }
                if (read == 0) {
                    source_ended_ = true;
                    break;
                }
                frames_read += read;
            }
        }
        const auto read_samples = checked_sample_count(
            frames_read, channel_count_, "aligned read result is too large");
        if (gain_ != 1.0F) {
            for (auto& sample : requested.first(read_samples)) sample *= gain_;
        }
        std::fill(requested.begin() + static_cast<std::ptrdiff_t>(read_samples),
                  requested.end(), 0.0F);
        remaining_frames_ -= requested_frames;
        return requested_frames;
    }

private:
    std::unique_ptr<IFloat32FrameReader> reader_;
    std::uint32_t channel_count_ = 0;
    std::int64_t remaining_frames_ = 0;
    float gain_ = 1.0F;
    bool source_ended_ = false;
};

} // namespace

CaptureAudioSource::CaptureAudioSource(
    WavFormat format,
    std::optional<std::filesystem::path> path,
    ReaderFactory reader_factory,
    IdentityValidator identity_validator)
    : format_(format),
      path_(std::move(path)),
      reader_factory_(std::move(reader_factory)),
      identity_validator_(std::move(identity_validator)) {}

CaptureAudioSource CaptureAudioSource::from_memory(AudioBuffer audio) {
    validate_audio_buffer(audio, "memory source");
    if (std::any_of(
            audio.samples.begin(), audio.samples.end(),
            [](const float sample) { return !std::isfinite(sample); })) {
        throw CaptureError(
            ErrorCode::source_stream_failure,
            "Memory audio source contains NaN or infinity.");
    }
    const WavFormat format{
        .sample_rate = audio.sample_rate,
        .channel_count = audio.channel_count,
        .bit_depth = AudioBitDepth::pcm32,
        .total_frames = audio.frame_count(),
    };
    auto shared = std::make_shared<AudioBuffer>(std::move(audio));
    return CaptureAudioSource(
        format,
        std::nullopt,
        [shared](const std::int64_t start_frame) {
            return std::make_unique<MemoryFrameReader>(shared, start_frame);
        },
        {});
}

bool CaptureAudioSource::valid() const noexcept {
    return static_cast<bool>(reader_factory_)
        && std::isfinite(format_.sample_rate) && format_.sample_rate > 0.0
        && format_.channel_count > 0 && format_.total_frames >= 0;
}

std::unique_ptr<IFloat32FrameReader> CaptureAudioSource::make_reader(
    const std::int64_t start_frame) const {
    if (!valid()) throw_invalid_stream("source descriptor is empty");
    if (start_frame < 0 || start_frame > format_.total_frames) {
        throw_invalid_stream("source reader start frame is outside the asset");
    }
    auto reader = reader_factory_(start_frame);
    if (!reader || reader->channel_count() != format_.channel_count) {
        throw CaptureError(
            ErrorCode::source_stream_failure,
            "Audio source returned an invalid frame reader.");
    }
    return reader;
}

float CaptureAudioSource::validated_peak_level(
    const std::size_t chunk_frames,
    const std::shared_ptr<CancellationToken>& cancellation) const {
    if (!valid() || chunk_frames == 0) {
        throw_invalid_stream("source peak scan dimensions are invalid");
    }
    const auto effective_chunk_frames = bounded_chunk_frames(
        chunk_frames, format_.channel_count);
    auto reader = make_reader();
    std::vector<float> buffer(effective_chunk_frames * format_.channel_count);
    std::int64_t scanned_frames = 0;
    float result = 0.0F;
    while (scanned_frames < format_.total_frames) {
        if (cancellation && cancellation->is_cancelled()) {
            throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
        }
        const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(
            format_.total_frames - scanned_frames,
            static_cast<std::int64_t>(effective_chunk_frames)));
        const auto read = reader->read_frames(
            std::span<float>(buffer).first(frames * format_.channel_count));
        if (read <= 0 || read > static_cast<std::int64_t>(frames)) {
            throw CaptureError(
                ErrorCode::source_stream_failure,
                "Audio source ended before its declared frame count.");
        }
        for (const auto sample : std::span<const float>(buffer).first(
                 static_cast<std::size_t>(read) * format_.channel_count)) {
            if (!std::isfinite(sample)) {
                throw CaptureError(
                    ErrorCode::source_stream_failure,
                    "Audio source contains NaN or infinity.");
            }
            result = std::max(result, std::abs(sample));
        }
        scanned_frames += read;
    }
    validate_identity();
    return result;
}

void CaptureAudioSource::validate_identity() const {
    if (!valid()) throw_invalid_stream("source descriptor is empty");
    if (identity_validator_) identity_validator_();
}

Float32AudioAsset::Float32AudioAsset(
    const double sample_rate,
    const std::uint32_t channel_count,
    const std::int64_t frame_count,
    std::optional<float> raw_peak,
    std::optional<std::filesystem::path> path,
    ReaderFactory reader_factory,
    std::shared_ptr<void> lifetime)
    : sample_rate_(sample_rate),
      channel_count_(channel_count),
      frame_count_(frame_count),
      raw_peak_(raw_peak),
      path_(std::move(path)),
      reader_factory_(std::move(reader_factory)),
      lifetime_(std::move(lifetime)) {}

Float32AudioAsset Float32AudioAsset::from_memory(AudioBuffer audio) {
    validate_audio_buffer(audio, "memory recording");
    auto shared = std::make_shared<AudioBuffer>(std::move(audio));
    const auto sample_rate = shared->sample_rate;
    const auto channel_count = shared->channel_count;
    const auto frame_count = shared->frame_count();
    return Float32AudioAsset(
        sample_rate,
        channel_count,
        frame_count,
        std::nullopt,
        std::nullopt,
        [shared](const std::int64_t start_frame) {
            return std::make_unique<MemoryFrameReader>(shared, start_frame);
        },
        shared);
}

Float32AudioAsset Float32AudioAsset::from_temporary_file(
    std::filesystem::path path,
    const double sample_rate,
    const std::uint32_t channel_count,
    const std::int64_t frame_count,
    const float raw_peak,
    const bool owns_file) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || channel_count == 0
        || frame_count < 0 || !std::isfinite(raw_peak) || raw_peak < 0.0F) {
        throw_invalid_stream("temporary recording metadata is invalid");
    }
    auto lifetime = owns_file
        ? std::static_pointer_cast<void>(std::make_shared<TemporaryFileLifetime>(path))
        : std::shared_ptr<void>{};
    const auto reader_path = path;
    const auto reader_lifetime = lifetime;
    return Float32AudioAsset(
        sample_rate,
        channel_count,
        frame_count,
        raw_peak,
        std::move(path),
        [reader_path, channel_count, frame_count, reader_lifetime](
            const std::int64_t start_frame) {
            return std::make_unique<RawFloat32FileReader>(
                reader_path, channel_count, frame_count, start_frame, reader_lifetime);
        },
        std::move(lifetime));
}

bool Float32AudioAsset::valid() const noexcept {
    return static_cast<bool>(reader_factory_)
        && std::isfinite(sample_rate_) && sample_rate_ > 0.0
        && channel_count_ > 0 && frame_count_ >= 0;
}

std::unique_ptr<IFloat32FrameReader> Float32AudioAsset::make_reader(
    const std::int64_t start_frame) const {
    if (!valid()) throw_invalid_stream("recorded asset descriptor is empty");
    if (start_frame < 0 || start_frame > frame_count_) {
        throw_invalid_stream("recorded reader start frame is outside the asset");
    }
    auto reader = reader_factory_(start_frame);
    if (!reader || reader->channel_count() != channel_count_) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "Recorded asset returned an invalid frame reader.");
    }
    return reader;
}

float Float32AudioAsset::validated_peak_level(
    const std::size_t chunk_frames,
    const std::shared_ptr<CancellationToken>& cancellation) const {
    if (!valid() || chunk_frames == 0) {
        throw_invalid_stream("recorded peak scan dimensions are invalid");
    }
    const auto effective_chunk_frames = bounded_chunk_frames(
        chunk_frames, channel_count_);
    auto reader = make_reader();
    std::vector<float> buffer(effective_chunk_frames * channel_count_);
    std::int64_t scanned_frames = 0;
    float result = 0.0F;
    while (scanned_frames < frame_count_) {
        if (cancellation && cancellation->is_cancelled()) {
            throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
        }
        const auto remaining = frame_count_ - scanned_frames;
        const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(
            remaining, static_cast<std::int64_t>(effective_chunk_frames)));
        const auto read = reader->read_frames(
            std::span<float>(buffer).first(frames * channel_count_));
        if (read <= 0 || read > static_cast<std::int64_t>(frames)) {
            throw CaptureError(
                ErrorCode::backend_failure,
                "Recorded asset ended before its declared frame count.");
        }
        for (const auto sample : std::span<const float>(buffer).first(
                 static_cast<std::size_t>(read) * channel_count_)) {
            if (!std::isfinite(sample)) {
                throw CaptureError(
                    ErrorCode::backend_failure,
                    "Recorded asset contains NaN or infinity.");
            }
            result = std::max(result, std::abs(sample));
        }
        scanned_frames += read;
    }
    return result;
}

std::unique_ptr<IFloat32FrameReader> AlignedCapturePayload::make_reader() const {
    if (!asset.valid() || start_frame < 0 || start_frame > asset.frame_count()
        || frame_count < 0) {
        throw_invalid_stream("aligned payload descriptor is invalid");
    }
    return std::make_unique<AlignedPayloadReader>(*this);
}

AudioBuffer AlignedCapturePayload::materialize(const std::size_t chunk_frames) const {
    if (chunk_frames == 0 || channel_count() == 0) {
        throw_invalid_stream("materialization chunk size is invalid");
    }
    AudioBuffer result{
        .sample_rate = asset.sample_rate(),
        .channel_count = channel_count(),
        .samples = std::vector<float>(checked_sample_count(
            frame_count, channel_count(), "aligned payload is too large")),
    };
    auto reader = make_reader();
    std::int64_t offset_frames = 0;
    while (offset_frames < frame_count) {
        const auto frames = std::min<std::int64_t>(
            frame_count - offset_frames,
            static_cast<std::int64_t>(chunk_frames));
        const auto sample_offset = checked_sample_count(
            offset_frames, channel_count(), "materialization offset is too large");
        const auto sample_count = checked_sample_count(
            frames, channel_count(), "materialization read is too large");
        const auto read = reader->read_frames(
            std::span<float>(result.samples).subspan(sample_offset, sample_count));
        if (read != frames) {
            throw CaptureError(
                ErrorCode::backend_failure,
                "Aligned payload reader ended unexpectedly.");
        }
        offset_frames += read;
    }
    return result;
}

} // namespace capture_panel
