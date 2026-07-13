#include "test_framework.hpp"

#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/streaming.hpp"
#include "capture_panel/core/wav.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace capture_panel;

class TemporaryFile final {
public:
    explicit TemporaryFile(const std::string& suffix) {
        static std::atomic_uint64_t sequence{0};
        path = std::filesystem::temp_directory_path()
            / ("capture-panel-streaming-"
               + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed))
               + suffix);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

class PartialVectorReader final : public IFloat32FrameReader {
public:
    PartialVectorReader(
        std::vector<float> samples,
        const std::uint32_t channels,
        const std::size_t max_frames_per_read)
        : samples_(std::move(samples)),
          channels_(channels),
          max_frames_per_read_(max_frames_per_read) {}

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return channels_;
    }

    std::int64_t read_frames(std::span<float> destination) override {
        const auto available_frames = (samples_.size() - offset_) / channels_;
        const auto requested_frames = destination.size() / channels_;
        const auto frames = std::min(
            {available_frames, requested_frames, max_frames_per_read_});
        const auto sample_count = frames * channels_;
        std::copy_n(samples_.data() + offset_, sample_count, destination.data());
        offset_ += sample_count;
        return static_cast<std::int64_t>(frames);
    }

private:
    std::vector<float> samples_;
    std::uint32_t channels_ = 0;
    std::size_t max_frames_per_read_ = 0;
    std::size_t offset_ = 0;
};

[[nodiscard]] bool throws_code(const auto& operation, const ErrorCode expected) {
    try {
        operation();
    } catch (const CaptureError& error) {
        return error.code() == expected;
    }
    return false;
}

} // namespace

CP_TEST_CASE("streaming WAV metadata peak and chunk reader agree with the container") {
    TemporaryFile file("-source.wav");
    const AudioBuffer audio{
        .sample_rate = 48'000.0,
        .channel_count = 2,
        .samples = {0.125F, -0.25F, 0.5F, -0.75F, 0.25F, -0.5F},
    };
    write_wav(file.path, audio, AudioBitDepth::pcm24);

    const auto format = read_wav_format(file.path);
    CP_REQUIRE(format.channel_count == 2);
    CP_REQUIRE(format.total_frames == 3);
    CP_REQUIRE(format.bit_depth == AudioBitDepth::pcm24);

    const auto source = CaptureAudioSource::from_wav(file.path, format);
    CP_REQUIRE_NEAR(source.validated_peak_level(1), 0.75, 0.00001);
    auto reader = source.make_reader(1);
    std::vector<float> decoded(4);
    CP_REQUIRE(reader->read_frames(decoded) == 2);
    CP_REQUIRE_NEAR(decoded[0], 0.5, 0.00001);
    CP_REQUIRE_NEAR(decoded[1], -0.75, 0.00001);
}

CP_TEST_CASE("WAV source descriptor fails closed when its file identity changes") {
    TemporaryFile file("-identity.wav");
    write_wav(file.path, std::vector<float>{0.1F, 0.2F}, 48'000.0, 1);
    const auto format = read_wav_format(file.path);
    const auto source = CaptureAudioSource::from_wav(file.path, format);
    const auto original_time = std::filesystem::last_write_time(file.path);

    write_wav(file.path, std::vector<float>{0.3F, 0.4F}, 48'000.0, 1);
    std::filesystem::last_write_time(file.path, original_time + std::chrono::seconds(2));
    CP_REQUIRE(throws_code(
        [&] { source.validate_identity(); },
        ErrorCode::source_stream_failure));
    CP_REQUIRE(throws_code(
        [&] { static_cast<void>(source.make_reader()); },
        ErrorCode::source_stream_failure));
}

CP_TEST_CASE("temporary Float32 asset remains alive until its last reader closes") {
    TemporaryFile file("-owned.f32");
    {
        std::ofstream stream(file.path, std::ios::binary | std::ios::trunc);
        const std::vector<float> samples{0.25F, -0.5F};
        stream.write(
            reinterpret_cast<const char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(float)));
    }

    auto asset = Float32AudioAsset::from_temporary_file(
        file.path, 48'000.0, 1, 2, 0.5F);
    auto reader = asset.make_reader();
    asset = {};
    CP_REQUIRE(std::filesystem::exists(file.path));
    std::vector<float> samples(2);
    CP_REQUIRE(reader->read_frames(samples) == 2);
    CP_REQUIRE_NEAR(samples[1], -0.5, 0.000001);
    reader.reset();
    CP_REQUIRE(!std::filesystem::exists(file.path));
}

CP_TEST_CASE("aligned payload reader applies gain and zero pads an early tail") {
    const auto asset = Float32AudioAsset::from_memory(AudioBuffer{
        .sample_rate = 48'000.0,
        .channel_count = 1,
        .samples = {0.1F, 0.2F, 0.3F},
    });
    const auto output = AlignedCapturePayload{
        .asset = asset,
        .start_frame = 1,
        .frame_count = 4,
        .gain = 2.0F,
    }.materialize(1);
    CP_REQUIRE(output.samples.size() == 4);
    CP_REQUIRE_NEAR(output.samples[0], 0.4, 0.000001);
    CP_REQUIRE_NEAR(output.samples[1], 0.6, 0.000001);
    CP_REQUIRE_NEAR(output.samples[2], 0.0, 0.000001);
    CP_REQUIRE_NEAR(output.samples[3], 0.0, 0.000001);
}

CP_TEST_CASE("streaming WAV writer accepts partial reads and writes odd PCM24 padding") {
    TemporaryFile file("-partial.wav");
    PartialVectorReader reader({0.25F, -0.5F, 0.75F}, 1, 1);
    write_wav(file.path, reader, 3, 48'000.0, 1, AudioBitDepth::pcm24);

    const auto result = read_wav(file.path);
    CP_REQUIRE(result.audio.frame_count() == 3);
    CP_REQUIRE_NEAR(result.audio.samples[2], 0.75, 0.00001);
    // 44-byte header + 9-byte data + one RIFF pad byte.
    CP_REQUIRE(std::filesystem::file_size(file.path) == 54);
}

CP_TEST_CASE("streaming WAV cancellation preserves an existing destination") {
    TemporaryFile file("-cancel.wav");
    write_wav(file.path, std::vector<float>{0.125F}, 48'000.0, 1);
    PartialVectorReader reader({0.75F, 0.5F}, 1, 1);
    const auto cancellation = std::make_shared<CancellationToken>();
    cancellation->cancel();

    CP_REQUIRE(throws_code(
        [&] {
            write_wav(
                file.path,
                reader,
                2,
                48'000.0,
                1,
                AudioBitDepth::pcm24,
                cancellation);
        },
        ErrorCode::capture_cancelled));
    const auto preserved = read_wav(file.path);
    CP_REQUIRE(preserved.audio.frame_count() == 1);
    CP_REQUIRE_NEAR(preserved.audio.samples.front(), 0.125, 0.00001);
}

CP_TEST_CASE("RIFF preflight rejects byte-rate and data-size overflow") {
    CP_REQUIRE(throws_code(
        [] {
            validate_wav_output_capacity(
                1, 768'000.0, 2'000, AudioBitDepth::pcm32);
        },
        ErrorCode::validation_failed));
    CP_REQUIRE(throws_code(
        [] {
            validate_wav_output_capacity(
                static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()),
                48'000.0,
                1,
                AudioBitDepth::pcm24);
        },
        ErrorCode::validation_failed));
}

CP_TEST_CASE("bounded source and asset peak scans observe cancellation") {
    const AudioBuffer audio{
        .sample_rate = 48'000.0,
        .channel_count = 1,
        .samples = std::vector<float>(32'768, 0.25F),
    };
    const auto source = CaptureAudioSource::from_memory(audio);
    const auto asset = Float32AudioAsset::from_memory(audio);
    const auto cancellation = std::make_shared<CancellationToken>();
    cancellation->cancel();
    CP_REQUIRE(throws_code(
        [&] {
            static_cast<void>(source.validated_peak_level(
                default_audio_chunk_frames, cancellation));
        },
        ErrorCode::capture_cancelled));
    CP_REQUIRE(throws_code(
        [&] {
            static_cast<void>(asset.validated_peak_level(
                default_audio_chunk_frames, cancellation));
        },
        ErrorCode::capture_cancelled));
}
