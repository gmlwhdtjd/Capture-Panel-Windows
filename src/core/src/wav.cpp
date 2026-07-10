#include "capture_panel/core/wav.hpp"

#include "capture_panel/core/errors.hpp"

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

} // namespace

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
                audio.samples[destination_sample++] = std::bit_cast<float>(read_u32(block.data() + offset));
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
    if (bits != 16 && bits != 24 && bits != 32) {
        throw CaptureError(ErrorCode::unsupported_bit_depth, "Unsupported output bit depth");
    }
    const auto bytes_per_sample = static_cast<std::uint32_t>(bits / 8U);
    const auto block_align_u32 = channel_count * bytes_per_sample;
    if (block_align_u32 > std::numeric_limits<std::uint16_t>::max()) {
        throw_write_error(path, "block alignment exceeds the WAV limit");
    }
    const auto byte_rate_u64 = static_cast<std::uint64_t>(sample_rate_u32) * block_align_u32;
    if (byte_rate_u64 > std::numeric_limits<std::uint32_t>::max()) {
        throw_write_error(path, "byte rate exceeds the WAV limit");
    }
    if (samples.size() > std::numeric_limits<std::uint32_t>::max() / bytes_per_sample) {
        throw_write_error(path, "audio data exceeds the 4 GiB RIFF/WAVE limit");
    }
    const auto data_size = static_cast<std::uint32_t>(samples.size() * bytes_per_sample);
    const auto data_padding = data_size & 1U;
    if (data_size > std::numeric_limits<std::uint32_t>::max() - 36U - data_padding) {
        throw_write_error(path, "RIFF size exceeds the WAV limit");
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
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

    std::vector<std::uint8_t> encoded;
    encoded.reserve(64U * 1024U);
    for (const auto sample : samples) {
        const auto quantized = quantize_sample(sample, bits);
        const auto raw = static_cast<std::uint64_t>(quantized);
        for (std::uint32_t byte = 0; byte < bytes_per_sample; ++byte) {
            encoded.push_back(static_cast<std::uint8_t>((raw >> (byte * 8U)) & 0xFFU));
        }
        if (encoded.size() >= 64U * 1024U) {
            write_bytes(stream, encoded);
            encoded.clear();
        }
    }
    write_bytes(stream, encoded);
    if (data_padding != 0) {
        const std::array<std::uint8_t, 1> padding{0};
        write_bytes(stream, padding);
    }

    stream.flush();
    if (!stream) throw_write_error(path, "an I/O error occurred while writing audio data");
}

void write_wav(
    const std::filesystem::path& path,
    const AudioBuffer& audio,
    const AudioBitDepth bit_depth) {
    write_wav(path, audio.samples, audio.sample_rate, audio.channel_count, bit_depth);
}

} // namespace capture_panel
