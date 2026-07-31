#include "capture_panel/core/wav.hpp"

#include "capture_panel/core/errors.hpp"
#include "atomic_file_transaction.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace capture_panel {
namespace {

constexpr std::uint16_t wave_format_pcm = 0x0001;
constexpr std::uint16_t wave_format_ieee_float = 0x0003;
constexpr std::uint16_t wave_format_extensible = 0xFFFE;
constexpr std::size_t preferred_chunk_samples = 1U << 20U;

struct ParsedFormat {
    std::uint16_t encoding = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t container_bits = 0;
    std::uint16_t valid_bits = 0;
};

struct LocatedData {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

[[noreturn]] void throw_read_error(const std::filesystem::path& path, const std::string& detail) {
    throw CaptureError(
        ErrorCode::wav_read,
        "Unable to read WAV file '" + path.string() + "': " + detail);
}

[[noreturn]] void throw_unsupported_format(
    const std::filesystem::path& path,
    const std::string& detail) {
    throw CaptureError(
        ErrorCode::unsupported_format,
        "Unsupported WAV format in '" + path.string() + "': " + detail);
}

[[noreturn]] void throw_unsupported_bit_depth(
    const std::filesystem::path& path,
    const std::uint16_t bits) {
    throw CaptureError(
        ErrorCode::unsupported_bit_depth,
        "Unsupported WAV bit depth in '" + path.string() + "': " + std::to_string(bits));
}

[[noreturn]] void throw_write_error(const std::filesystem::path& path, const std::string& detail) {
    throw CaptureError(
        ErrorCode::wav_write,
        "Unable to write WAV file '" + path.string() + "': " + detail);
}

[[nodiscard]] std::size_t bounded_chunk_frames(
    const std::size_t requested_frames,
    const std::uint32_t channels,
    const std::filesystem::path& path) {
    if (requested_frames == 0 || channels == 0) {
        throw_read_error(path, "audio chunk dimensions are invalid");
    }
    const auto frames = std::min(
        requested_frames,
        std::max<std::size_t>(1, preferred_chunk_samples / channels));
    if (frames > std::numeric_limits<std::size_t>::max() / channels) {
        throw_read_error(path, "audio chunk sample count is too large");
    }
    return frames;
}

[[nodiscard]] platform::AtomicFileTransaction begin_atomic_write(
    const std::filesystem::path& destination) {
    try {
        return platform::AtomicFileTransaction(destination);
    } catch (const platform::AtomicFileTransactionError& error) {
        throw_write_error(destination, error.what());
    }
}

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void read_exact(
    std::istream& stream,
    const std::span<std::uint8_t> destination,
    const std::filesystem::path& path,
    const char* what) {
    if (destination.empty()) return;
    stream.read(
        reinterpret_cast<char*>(destination.data()),
        static_cast<std::streamsize>(destination.size()));
    if (stream.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw_read_error(path, std::string("truncated ") + what);
    }
}

void seek_to(
    std::istream& stream,
    const std::uint64_t offset,
    const std::filesystem::path& path) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw_read_error(path, "file offset is too large");
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) throw_read_error(path, "failed to seek within the file");
}

[[nodiscard]] ParsedFormat parse_format_chunk(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t chunk_size,
    const std::filesystem::path& path) {
    if (chunk_size < 16U || bytes.size() < 16U) {
        throw_read_error(path, "fmt chunk is shorter than 16 bytes");
    }

    ParsedFormat format;
    auto format_tag = read_u16(bytes.data());
    format.channels = read_u16(bytes.data() + 2);
    format.sample_rate = read_u32(bytes.data() + 4);
    format.block_align = read_u16(bytes.data() + 12);
    format.container_bits = read_u16(bytes.data() + 14);
    format.valid_bits = format.container_bits;

    if (format_tag == wave_format_extensible) {
        const auto extension_size = bytes.size() >= 18U ? read_u16(bytes.data() + 16) : 0U;
        if (chunk_size < 40U || bytes.size() < 40U || extension_size < 22U
            || static_cast<std::uint32_t>(extension_size) + 18U > chunk_size) {
            throw_read_error(path, "WAVE_FORMAT_EXTENSIBLE fmt chunk is incomplete");
        }

        format.valid_bits = read_u16(bytes.data() + 18);
        if (format.valid_bits == 0) format.valid_bits = format.container_bits;

        static constexpr std::array<std::uint8_t, 12> canonical_guid_tail{
            0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
            0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
        };
        if (!std::equal(
                canonical_guid_tail.begin(),
                canonical_guid_tail.end(),
                bytes.begin() + 28)) {
            throw_unsupported_format(path, "unknown extensible subformat GUID");
        }

        const auto subformat = read_u32(bytes.data() + 24);
        if (subformat != wave_format_pcm && subformat != wave_format_ieee_float) {
            throw_unsupported_format(path, "extensible subformat is neither PCM nor IEEE float");
        }
        format_tag = static_cast<std::uint16_t>(subformat);
    }

    if (format_tag != wave_format_pcm && format_tag != wave_format_ieee_float) {
        throw_unsupported_format(path, "format tag " + std::to_string(format_tag));
    }
    format.encoding = format_tag;

    if (format.channels == 0 || format.sample_rate == 0 || format.block_align == 0) {
        throw_read_error(path, "fmt chunk contains a zero channel count, sample rate, or block alignment");
    }
    if (format.container_bits != 16 && format.container_bits != 24 && format.container_bits != 32) {
        throw_unsupported_bit_depth(path, format.container_bits);
    }
    if (format.encoding == wave_format_ieee_float && format.container_bits != 32) {
        throw_unsupported_format(path, "IEEE float WAV must use 32-bit samples");
    }
    if (format.valid_bits == 0 || format.valid_bits > format.container_bits) {
        throw_unsupported_format(path, "invalid valid-bits-per-sample value");
    }
    if (format.encoding == wave_format_ieee_float && format.valid_bits != 32) {
        throw_unsupported_format(path, "IEEE float valid bit depth must be 32");
    }

    const auto bytes_per_sample = static_cast<std::uint32_t>(format.container_bits / 8U);
    const auto expected_block_align = static_cast<std::uint32_t>(format.channels) * bytes_per_sample;
    if (expected_block_align > std::numeric_limits<std::uint16_t>::max()
        || format.block_align != expected_block_align) {
        throw_read_error(path, "fmt block alignment does not match channel count and bit depth");
    }
    return format;
}

[[nodiscard]] std::int32_t decode_signed_integer(
    const std::uint8_t* bytes,
    const std::uint16_t bits) noexcept {
    if (bits == 16) {
        const auto raw = read_u16(bytes);
        return std::bit_cast<std::int16_t>(raw);
    }
    if (bits == 24) {
        auto raw = static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8U)
            | (static_cast<std::uint32_t>(bytes[2]) << 16U);
        if ((raw & 0x00800000U) != 0) raw |= 0xFF000000U;
        return std::bit_cast<std::int32_t>(raw);
    }
    return std::bit_cast<std::int32_t>(read_u32(bytes));
}

[[nodiscard]] AudioBitDepth bit_depth_from_bits(const std::uint16_t bits) {
    switch (bits) {
    case 16: return AudioBitDepth::pcm16;
    case 24: return AudioBitDepth::pcm24;
    case 32: return AudioBitDepth::pcm32;
    default:
        // All callers validate this first; keep the switch exhaustive against corrupt state.
        throw CaptureError(ErrorCode::unsupported_bit_depth, "Unsupported WAV bit depth");
    }
}

[[nodiscard]] std::int64_t quantize_sample(const float input, const std::uint16_t bits) noexcept {
    double sample = static_cast<double>(input);
    if (std::isnan(sample)) sample = 0.0;
    sample = std::clamp(sample, -1.0, 1.0);

    const auto minimum = -(std::int64_t{1} << (bits - 1U));
    const auto maximum = (std::int64_t{1} << (bits - 1U)) - 1;
    const auto scale = static_cast<double>(std::int64_t{1} << (bits - 1U));
    if (sample <= -1.0) return minimum;
    if (sample >= 1.0) return maximum;
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(sample * scale)),
        minimum,
        maximum);
}

[[nodiscard]] float sanitized_float_sample(const float sample) noexcept {
    if (std::isnan(sample)) return 0.0F;
    if (std::isinf(sample)) return std::signbit(sample) ? -1.0F : 1.0F;
    return sample;
}

void write_bytes(std::ostream& stream, const std::span<const std::uint8_t> bytes) {
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void write_fourcc(std::ostream& stream, const char (&value)[5]) {
    stream.write(value, 4);
}

void write_u16(std::ostream& stream, const std::uint16_t value) {
    const std::array<std::uint8_t, 2> bytes{
        static_cast<std::uint8_t>(value & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
    };
    write_bytes(stream, bytes);
}

void write_u32(std::ostream& stream, const std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
    };
    write_bytes(stream, bytes);
}

struct ParsedWavDescriptor {
    ParsedFormat encoded_format;
    LocatedData data;
    WavFormat public_format;
};

struct SourceFileSnapshot {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type last_write_time{};
};

[[nodiscard]] SourceFileSnapshot source_file_snapshot(
    const std::filesystem::path& path) {
    std::error_code size_error;
    std::error_code time_error;
    const auto size = std::filesystem::file_size(path, size_error);
    const auto write_time = std::filesystem::last_write_time(path, time_error);
    if (size_error || time_error) {
        throw CaptureError(
            ErrorCode::source_stream_failure,
            "Unable to snapshot source WAV identity: " + path.string());
    }
    return {.size = size, .last_write_time = write_time};
}

void validate_source_snapshot(
    const std::filesystem::path& path,
    const SourceFileSnapshot& expected) {
    const auto current = source_file_snapshot(path);
    if (current.size != expected.size
        || current.last_write_time != expected.last_write_time) {
        throw CaptureError(
            ErrorCode::source_stream_failure,
            "Source WAV changed while the capture was running: " + path.string());
    }
}

[[nodiscard]] ParsedWavDescriptor parse_wav_descriptor(
    const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw CaptureError(ErrorCode::file_not_found, "File not found: " + path.string());
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw_read_error(path, "file could not be opened");

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size < 12U) {
        throw_read_error(path, "file is shorter than a RIFF header");
    }

    std::array<std::uint8_t, 12> riff_header{};
    read_exact(stream, riff_header, path, "RIFF header");
    if (std::memcmp(riff_header.data(), "RIFF", 4) != 0
        || std::memcmp(riff_header.data() + 8, "WAVE", 4) != 0) {
        throw_unsupported_format(path, "file is not RIFF/WAVE");
    }

    const auto riff_size = read_u32(riff_header.data() + 4);
    const auto riff_end = static_cast<std::uint64_t>(riff_size) + 8U;
    if (riff_end < 12U || riff_end > file_size) {
        throw_read_error(path, "RIFF size extends beyond the file");
    }

    std::optional<ParsedFormat> format;
    std::optional<LocatedData> data;
    std::uint64_t position = 12;
    while (position + 8U <= riff_end) {
        seek_to(stream, position, path);
        std::array<std::uint8_t, 8> chunk_header{};
        read_exact(stream, chunk_header, path, "chunk header");

        const auto chunk_size = read_u32(chunk_header.data() + 4);
        const auto payload_offset = position + 8U;
        const auto padded_size = static_cast<std::uint64_t>(chunk_size) + (chunk_size & 1U);
        if (payload_offset > riff_end || padded_size > riff_end - payload_offset) {
            throw_read_error(path, "chunk extends beyond the RIFF container");
        }

        if (std::memcmp(chunk_header.data(), "fmt ", 4) == 0 && !format.has_value()) {
            std::array<std::uint8_t, 40> fmt_bytes{};
            const auto bytes_to_read = std::min<std::size_t>(chunk_size, fmt_bytes.size());
            read_exact(
                stream,
                std::span<std::uint8_t>(fmt_bytes).first(bytes_to_read),
                path,
                "fmt chunk");
            format = parse_format_chunk(
                std::span<const std::uint8_t>(fmt_bytes).first(bytes_to_read),
                chunk_size,
                path);
        } else if (std::memcmp(chunk_header.data(), "data", 4) == 0 && !data.has_value()) {
            data = LocatedData{payload_offset, chunk_size};
        }

        position = payload_offset + padded_size;
    }

    if (!format.has_value()) throw_read_error(path, "fmt chunk is missing");
    if (!data.has_value()) throw_read_error(path, "data chunk is missing");
    if (data->size % format->block_align != 0) {
        throw_read_error(path, "data chunk contains a partial audio frame");
    }
    const auto total_frames = static_cast<std::uint64_t>(data->size / format->block_align);
    if (total_frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw_read_error(path, "audio frame count is too large");
    }

    return {
        .encoded_format = *format,
        .data = *data,
        .public_format = {
            .sample_rate = static_cast<double>(format->sample_rate),
            .channel_count = format->channels,
            .bit_depth = bit_depth_from_bits(format->container_bits),
            .total_frames = static_cast<std::int64_t>(total_frames),
        },
    };
}

[[nodiscard]] bool same_wav_format(
    const WavFormat& left,
    const WavFormat& right) noexcept {
    return std::abs(left.sample_rate - right.sample_rate) < 0.5
        && left.channel_count == right.channel_count
        && left.bit_depth == right.bit_depth
        && left.total_frames == right.total_frames;
}

class WavFloat32FrameReader final : public IFloat32FrameReader {
public:
    WavFloat32FrameReader(
        std::filesystem::path path,
        const WavFormat& expected_format,
        const std::int64_t start_frame,
        SourceFileSnapshot snapshot)
        : path_(std::move(path)), snapshot_(snapshot) {
        validate_source_snapshot(path_, snapshot_);
        const auto descriptor = parse_wav_descriptor(path_);
        if (!same_wav_format(descriptor.public_format, expected_format)) {
            throw CaptureError(
                ErrorCode::source_stream_failure,
                "Source WAV metadata changed after validation: " + path_.string());
        }
        format_ = descriptor.encoded_format;
        if (start_frame < 0 || start_frame > expected_format.total_frames) {
            throw CaptureError(
                ErrorCode::validation_failed,
                "Source WAV reader start frame is outside the asset.");
        }
        const auto byte_offset = static_cast<std::uint64_t>(start_frame)
            * format_.block_align;
        if (byte_offset > descriptor.data.size) {
            throw_read_error(path_, "source reader offset exceeds the data chunk");
        }
        remaining_frames_ = expected_format.total_frames - start_frame;
        stream_.open(path_, std::ios::binary);
        if (!stream_) throw_read_error(path_, "file could not be reopened");
        seek_to(stream_, descriptor.data.offset + byte_offset, path_);
    }

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return format_.channels;
    }

    std::int64_t read_frames(std::span<float> destination) override {
        if (destination.size() % format_.channels != 0) {
            throw CaptureError(
                ErrorCode::validation_failed,
                "WAV reader destination is not frame-aligned.");
        }
        const auto requested_frames = std::min<std::int64_t>(
            remaining_frames_,
            static_cast<std::int64_t>(destination.size() / format_.channels));
        if (requested_frames <= 0) return 0;
        const auto byte_count_u64 = static_cast<std::uint64_t>(requested_frames)
            * format_.block_align;
        if (byte_count_u64 > std::numeric_limits<std::size_t>::max()
            || byte_count_u64 > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamsize>::max())) {
            throw CaptureError(
                ErrorCode::validation_failed,
                "WAV read chunk is too large.");
        }
        encoded_.resize(static_cast<std::size_t>(byte_count_u64));
        read_exact(stream_, encoded_, path_, "audio data");

        const auto bytes_per_sample = static_cast<std::size_t>(format_.container_bits / 8U);
        std::size_t destination_sample = 0;
        for (std::size_t offset = 0; offset < encoded_.size(); offset += bytes_per_sample) {
            if (format_.encoding == wave_format_ieee_float) {
                destination[destination_sample++] = sanitized_float_sample(
                    std::bit_cast<float>(read_u32(encoded_.data() + offset)));
            } else {
                auto integer = static_cast<std::int64_t>(decode_signed_integer(
                    encoded_.data() + offset,
                    format_.container_bits));
                const auto padding_bits = format_.container_bits - format_.valid_bits;
                if (padding_bits > 0) integer /= (std::int64_t{1} << padding_bits);
                const auto scale = static_cast<double>(
                    std::int64_t{1} << (format_.valid_bits - 1U));
                destination[destination_sample++] = static_cast<float>(
                    static_cast<double>(integer) / scale);
            }
        }
        remaining_frames_ -= requested_frames;
        if (remaining_frames_ == 0 && !identity_checked_) {
            validate_source_snapshot(path_, snapshot_);
            identity_checked_ = true;
        }
        return requested_frames;
    }

private:
    std::filesystem::path path_;
    ParsedFormat format_{};
    std::int64_t remaining_frames_ = 0;
    std::ifstream stream_;
    std::vector<std::uint8_t> encoded_;
    SourceFileSnapshot snapshot_{};
    bool identity_checked_ = false;
};

class SpanFloat32FrameReader final : public IFloat32FrameReader {
public:
    SpanFloat32FrameReader(
        const std::span<const float> samples,
        const std::uint32_t channel_count)
        : samples_(samples), channel_count_(channel_count) {}

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return channel_count_;
    }

    std::int64_t read_frames(std::span<float> destination) override {
        if (destination.size() % channel_count_ != 0) {
            throw CaptureError(
                ErrorCode::validation_failed,
                "Memory WAV reader destination is not frame-aligned.");
        }
        const auto sample_count = std::min(destination.size(), samples_.size() - offset_);
        if (sample_count == 0) return 0;
        std::copy_n(samples_.data() + offset_, sample_count, destination.data());
        offset_ += sample_count;
        return static_cast<std::int64_t>(sample_count / channel_count_);
    }

private:
    std::span<const float> samples_;
    std::uint32_t channel_count_ = 0;
    std::size_t offset_ = 0;
};

} // namespace

WavFormat read_wav_format(const std::filesystem::path& path) {
    return parse_wav_descriptor(path).public_format;
}

CaptureAudioSource CaptureAudioSource::from_wav(
    std::filesystem::path path,
    const WavFormat format) {
    const auto snapshot = source_file_snapshot(path);
    const auto reader_path = path;
    return CaptureAudioSource(
        format,
        std::move(path),
        [reader_path, format, snapshot](const std::int64_t start_frame) {
            return std::make_unique<WavFloat32FrameReader>(
                reader_path, format, start_frame, snapshot);
        },
        [reader_path, snapshot] {
            validate_source_snapshot(reader_path, snapshot);
        });
}

float wav_peak_level(
    const std::filesystem::path& path,
    const WavFormat& expected_format,
    const std::size_t max_frames_per_chunk,
    const std::shared_ptr<CancellationToken>& cancellation) {
    if (max_frames_per_chunk == 0 || expected_format.channel_count == 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "WAV peak scan chunk size is invalid.");
    }
    const auto chunk_frames = bounded_chunk_frames(
        max_frames_per_chunk, expected_format.channel_count, path);
    auto reader = CaptureAudioSource::from_wav(path, expected_format).make_reader();
    std::vector<float> buffer(chunk_frames * expected_format.channel_count);
    std::int64_t scanned_frames = 0;
    float result = 0.0F;
    while (scanned_frames < expected_format.total_frames) {
        if (cancellation && cancellation->is_cancelled()) {
            throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
        }
        const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(
            expected_format.total_frames - scanned_frames,
            static_cast<std::int64_t>(chunk_frames)));
        const auto read = reader->read_frames(
            std::span<float>(buffer).first(frames * expected_format.channel_count));
        if (read <= 0 || read > static_cast<std::int64_t>(frames)) {
            throw_read_error(path, "audio payload ended before the declared frame count");
        }
        for (const auto sample : std::span<const float>(buffer).first(
                 static_cast<std::size_t>(read) * expected_format.channel_count)) {
            result = std::max(result, std::abs(sample));
        }
        scanned_frames += read;
    }
    return result;
}

WavContents read_wav(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw CaptureError(ErrorCode::file_not_found, "File not found: " + path.string());
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw_read_error(path, "file could not be opened");

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size < 12U) throw_read_error(path, "file is shorter than a RIFF header");

    std::array<std::uint8_t, 12> riff_header{};
    read_exact(stream, riff_header, path, "RIFF header");
    if (std::memcmp(riff_header.data(), "RIFF", 4) != 0
        || std::memcmp(riff_header.data() + 8, "WAVE", 4) != 0) {
        throw_unsupported_format(path, "file is not RIFF/WAVE");
    }

    const auto riff_size = read_u32(riff_header.data() + 4);
    const auto riff_end = static_cast<std::uint64_t>(riff_size) + 8U;
    if (riff_end < 12U || riff_end > file_size) {
        throw_read_error(path, "RIFF size extends beyond the file");
    }

    std::optional<ParsedFormat> format;
    std::optional<LocatedData> data;
    std::uint64_t position = 12;
    while (position + 8U <= riff_end) {
        seek_to(stream, position, path);
        std::array<std::uint8_t, 8> chunk_header{};
        read_exact(stream, chunk_header, path, "chunk header");

        const auto chunk_size = read_u32(chunk_header.data() + 4);
        const auto payload_offset = position + 8U;
        const auto padded_size = static_cast<std::uint64_t>(chunk_size) + (chunk_size & 1U);
        if (payload_offset > riff_end || padded_size > riff_end - payload_offset) {
            throw_read_error(path, "chunk extends beyond the RIFF container");
        }

        if (std::memcmp(chunk_header.data(), "fmt ", 4) == 0 && !format.has_value()) {
            std::array<std::uint8_t, 40> fmt_bytes{};
            const auto bytes_to_read = std::min<std::size_t>(chunk_size, fmt_bytes.size());
            read_exact(
                stream,
                std::span<std::uint8_t>(fmt_bytes).first(bytes_to_read),
                path,
                "fmt chunk");
            format = parse_format_chunk(
                std::span<const std::uint8_t>(fmt_bytes).first(bytes_to_read),
                chunk_size,
                path);
        } else if (std::memcmp(chunk_header.data(), "data", 4) == 0 && !data.has_value()) {
            data = LocatedData{payload_offset, chunk_size};
        }

        position = payload_offset + padded_size;
    }

    if (!format.has_value()) throw_read_error(path, "fmt chunk is missing");
    if (!data.has_value()) throw_read_error(path, "data chunk is missing");
    if (data->size % format->block_align != 0) {
        throw_read_error(path, "data chunk contains a partial audio frame");
    }

    const auto total_frames = static_cast<std::uint64_t>(data->size / format->block_align);
    if (total_frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw_read_error(path, "audio frame count is too large");
    }
    if (total_frames > std::numeric_limits<std::size_t>::max() / format->channels) {
        throw_read_error(path, "decoded audio buffer is too large");
    }

    AudioBuffer audio;
    audio.sample_rate = static_cast<double>(format->sample_rate);
    audio.channel_count = format->channels;
    audio.samples.resize(static_cast<std::size_t>(total_frames) * format->channels);

    seek_to(stream, data->offset, path);
    const auto bytes_per_sample = static_cast<std::size_t>(format->container_bits / 8U);
    constexpr std::size_t preferred_block_bytes = 64U * 1024U;
    auto block_size = preferred_block_bytes - (preferred_block_bytes % format->block_align);
    if (block_size == 0) block_size = format->block_align;
    std::vector<std::uint8_t> block(block_size);

    std::uint64_t bytes_remaining = data->size;
    std::size_t destination_sample = 0;
    while (bytes_remaining > 0) {
        const auto bytes_this_time = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes_remaining, block.size()));
        read_exact(
            stream,
            std::span<std::uint8_t>(block).first(bytes_this_time),
            path,
            "audio data");

        for (std::size_t offset = 0; offset < bytes_this_time; offset += bytes_per_sample) {
            if (format->encoding == wave_format_ieee_float) {
                audio.samples[destination_sample++] = sanitized_float_sample(
                    std::bit_cast<float>(read_u32(block.data() + offset)));
            } else {
                auto integer = static_cast<std::int64_t>(decode_signed_integer(
                    block.data() + offset,
                    format->container_bits));
                const auto padding_bits = format->container_bits - format->valid_bits;
                if (padding_bits > 0) integer /= (std::int64_t{1} << padding_bits);
                const auto scale = static_cast<double>(std::int64_t{1} << (format->valid_bits - 1U));
                audio.samples[destination_sample++] = static_cast<float>(
                    static_cast<double>(integer) / scale);
            }
        }
        bytes_remaining -= bytes_this_time;
    }

    WavFormat public_format;
    public_format.sample_rate = audio.sample_rate;
    public_format.channel_count = audio.channel_count;
    public_format.bit_depth = bit_depth_from_bits(format->container_bits);
    public_format.total_frames = static_cast<std::int64_t>(total_frames);
    return {public_format, std::move(audio)};
}

void validate_wav_output_capacity(
    const std::int64_t frame_count,
    const double sample_rate,
    const std::uint32_t channel_count,
    const AudioBitDepth bit_depth) {
    const auto bits = static_cast<std::uint16_t>(bit_depth);
    if (bits != 16 && bits != 24 && bits != 32) {
        throw CaptureError(ErrorCode::unsupported_bit_depth, "Unsupported output bit depth");
    }
    if (frame_count < 0 || channel_count == 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "WAV output dimensions must be non-negative and have at least one channel.");
    }
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0
        || sample_rate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        || std::abs(sample_rate - std::round(sample_rate)) >= 0.001) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "WAV sample rate must be a positive integer number of hertz.");
    }

    const auto bytes_per_sample = static_cast<std::uint64_t>(bits / 8U);
    const auto block_align = static_cast<std::uint64_t>(channel_count) * bytes_per_sample;
    if (block_align > std::numeric_limits<std::uint16_t>::max()) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "WAV output block alignment exceeds the RIFF/WAVE limit.");
    }
    if (static_cast<std::uint64_t>(std::llround(sample_rate))
            > std::numeric_limits<std::uint32_t>::max() / block_align) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "WAV output byte rate exceeds the RIFF/WAVE limit.");
    }
    const auto frames = static_cast<std::uint64_t>(frame_count);
    if (frames > std::numeric_limits<std::uint64_t>::max() / block_align) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "WAV output dimensions overflow.");
    }
    const auto data_size = frames * block_align;
    const auto padding = data_size & 1U;
    if (data_size > std::numeric_limits<std::uint32_t>::max()
        || data_size > std::numeric_limits<std::uint32_t>::max() - 36U - padding) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Capture output exceeds the 4 GiB classic RIFF/WAVE limit; RF64/BW64 is not supported.");
    }
}

void write_wav(
    const std::filesystem::path& path,
    IFloat32FrameReader& reader,
    const std::int64_t frame_count,
    const double sample_rate,
    const std::uint32_t channel_count,
    const AudioBitDepth bit_depth,
    const std::shared_ptr<CancellationToken>& cancellation) {
    validate_wav_output_capacity(frame_count, sample_rate, channel_count, bit_depth);
    if (reader.channel_count() != channel_count) {
        throw_write_error(path, "reader channel count does not match the output");
    }
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0
        || sample_rate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw CaptureError(ErrorCode::unsupported_sample_rate, "Unsupported sample rate");
    }
    const auto rounded_sample_rate = std::round(sample_rate);
    if (std::abs(sample_rate - rounded_sample_rate) >= 0.001) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "WAV sample rate must be an integer number of hertz");
    }

    const auto sample_rate_u32 = static_cast<std::uint32_t>(rounded_sample_rate);
    const auto bits = static_cast<std::uint16_t>(bit_depth);
    const auto bytes_per_sample = static_cast<std::uint32_t>(bits / 8U);
    const auto block_align_u32 = channel_count * bytes_per_sample;
    const auto byte_rate_u64 = static_cast<std::uint64_t>(sample_rate_u32) * block_align_u32;
    if (byte_rate_u64 > std::numeric_limits<std::uint32_t>::max()) {
        throw_write_error(path, "byte rate exceeds the WAV limit");
    }
    const auto data_size_u64 = static_cast<std::uint64_t>(frame_count) * block_align_u32;
    const auto data_size = static_cast<std::uint32_t>(data_size_u64);
    const auto data_padding = data_size & 1U;

    auto temporary = begin_atomic_write(path);
    std::ofstream stream(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!stream) throw_write_error(path, "file could not be created");

    write_fourcc(stream, "RIFF");
    write_u32(stream, 36U + data_size + data_padding);
    write_fourcc(stream, "WAVE");
    write_fourcc(stream, "fmt ");
    write_u32(stream, 16U);
    write_u16(stream, wave_format_pcm);
    write_u16(stream, static_cast<std::uint16_t>(channel_count));
    write_u32(stream, sample_rate_u32);
    write_u32(stream, static_cast<std::uint32_t>(byte_rate_u64));
    write_u16(stream, static_cast<std::uint16_t>(block_align_u32));
    write_u16(stream, bits);
    write_fourcc(stream, "data");
    write_u32(stream, data_size);

    const auto chunk_frames = bounded_chunk_frames(
        default_audio_chunk_frames, channel_count, path);
    std::vector<float> decoded(chunk_frames * channel_count);
    std::vector<std::uint8_t> encoded;
    encoded.reserve(decoded.size() * bytes_per_sample);
    std::int64_t written_frames = 0;
    while (written_frames < frame_count) {
        if (cancellation && cancellation->is_cancelled()) {
            throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
        }
        const auto requested_frames = static_cast<std::size_t>(std::min<std::int64_t>(
            frame_count - written_frames,
            static_cast<std::int64_t>(chunk_frames)));
        const auto sample_count = requested_frames * channel_count;
        std::size_t filled_frames = 0;
        while (filled_frames < requested_frames) {
            if (cancellation && cancellation->is_cancelled()) {
                throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
            }
            const auto sample_offset = filled_frames * channel_count;
            const auto read = reader.read_frames(
                std::span<float>(decoded).subspan(
                    sample_offset, sample_count - sample_offset));
            if (read <= 0
                || static_cast<std::uint64_t>(read)
                    > requested_frames - filled_frames) {
                throw_write_error(
                    path,
                    read == 0
                        ? "audio reader ended before the declared frame count"
                        : "audio reader violated the frame-reader contract");
            }
            filled_frames += static_cast<std::size_t>(read);
        }

        encoded.clear();
        for (const auto sample : std::span<const float>(decoded).first(sample_count)) {
            const auto quantized = quantize_sample(sample, bits);
            const auto raw = static_cast<std::uint64_t>(quantized);
            for (std::uint32_t byte = 0; byte < bytes_per_sample; ++byte) {
                encoded.push_back(static_cast<std::uint8_t>((raw >> (byte * 8U)) & 0xFFU));
            }
        }
        write_bytes(stream, encoded);
        if (!stream) throw_write_error(path, "an I/O error occurred while writing audio data");
        written_frames += static_cast<std::int64_t>(requested_frames);
    }
    if (data_padding != 0) {
        const std::array<std::uint8_t, 1> padding{0};
        write_bytes(stream, padding);
    }

    stream.flush();
    if (!stream) throw_write_error(path, "an I/O error occurred while writing audio data");
    stream.close();
    if (!stream) throw_write_error(path, "an I/O error occurred while closing audio data");
    if (cancellation && !cancellation->begin_output_commit()) {
        throw CaptureError(ErrorCode::capture_cancelled, "Capture was cancelled.");
    }
    try {
        temporary.promote_to(path);
    } catch (const platform::AtomicFileTransactionError& error) {
        throw_write_error(path, error.what());
    }
}

void write_wav(
    const std::filesystem::path& path,
    const std::span<const float> samples,
    const double sample_rate,
    const std::uint32_t channel_count,
    const AudioBitDepth bit_depth) {
    if (channel_count == 0 || channel_count > std::numeric_limits<std::uint16_t>::max()) {
        throw_write_error(path, "channel count is outside the WAV range");
    }
    if (samples.size() % channel_count != 0) {
        throw_write_error(path, "sample count is not divisible by channel count");
    }
    const auto frames = samples.size() / channel_count;
    if (frames > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw_write_error(path, "audio frame count exceeds stream limits");
    }
    SpanFloat32FrameReader reader(samples, channel_count);
    write_wav(
        path,
        reader,
        static_cast<std::int64_t>(frames),
        sample_rate,
        channel_count,
        bit_depth);
}

void write_wav(
    const std::filesystem::path& path,
    const AudioBuffer& audio,
    const AudioBitDepth bit_depth) {
    write_wav(path, audio.samples, audio.sample_rate, audio.channel_count, bit_depth);
}

} // namespace capture_panel
