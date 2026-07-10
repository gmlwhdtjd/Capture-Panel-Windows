#include "asio_sample_converter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace capture_panel::asio {
namespace {

enum class SampleEncoding {
    signed_integer,
    ieee_float32,
    ieee_float64,
};

enum class ByteOrder {
    little_endian,
    big_endian,
};

struct SampleFormat {
    std::size_t byte_count = 0;
    unsigned valid_bits = 0;
    SampleEncoding encoding = SampleEncoding::signed_integer;
    ByteOrder byte_order = ByteOrder::little_endian;
};

[[nodiscard]] bool describe_sample_type(
    const ASIOSampleType sample_type,
    SampleFormat& format) noexcept {
    switch (sample_type) {
    case ASIOSTInt16MSB:
        format = {2, 16, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTInt24MSB:
        format = {3, 24, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTInt32MSB:
        format = {4, 32, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTFloat32MSB:
        format = {4, 32, SampleEncoding::ieee_float32, ByteOrder::big_endian};
        return true;
    case ASIOSTFloat64MSB:
        format = {8, 64, SampleEncoding::ieee_float64, ByteOrder::big_endian};
        return true;
    case ASIOSTInt32MSB16:
        format = {4, 16, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTInt32MSB18:
        format = {4, 18, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTInt32MSB20:
        format = {4, 20, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTInt32MSB24:
        format = {4, 24, SampleEncoding::signed_integer, ByteOrder::big_endian};
        return true;
    case ASIOSTInt16LSB:
        format = {2, 16, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    case ASIOSTInt24LSB:
        format = {3, 24, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    case ASIOSTInt32LSB:
        format = {4, 32, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    case ASIOSTFloat32LSB:
        format = {4, 32, SampleEncoding::ieee_float32, ByteOrder::little_endian};
        return true;
    case ASIOSTFloat64LSB:
        format = {8, 64, SampleEncoding::ieee_float64, ByteOrder::little_endian};
        return true;
    case ASIOSTInt32LSB16:
        format = {4, 16, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    case ASIOSTInt32LSB18:
        format = {4, 18, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    case ASIOSTInt32LSB20:
        format = {4, 20, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    case ASIOSTInt32LSB24:
        format = {4, 24, SampleEncoding::signed_integer, ByteOrder::little_endian};
        return true;
    default:
        return false;
    }
}

[[nodiscard]] double normalized_sample(const double value) noexcept {
    if (std::isnan(value)) return 0.0;
    return std::clamp(value, -1.0, 1.0);
}

[[nodiscard]] std::int64_t quantize_sample(
    const float input,
    const unsigned valid_bits) noexcept {
    const auto sample = normalized_sample(static_cast<double>(input));
    const auto scale_integer = std::int64_t{1} << (valid_bits - 1U);
    const auto minimum = -scale_integer;
    const auto maximum = scale_integer - 1;

    if (sample <= -1.0) return minimum;
    if (sample >= 1.0) return maximum;

    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(
            sample * static_cast<double>(scale_integer))),
        minimum,
        maximum);
}

void write_unsigned(
    const std::uint64_t value,
    const ByteOrder byte_order,
    const std::span<std::byte> destination) noexcept {
    for (std::size_t index = 0; index < destination.size(); ++index) {
        const auto destination_index = byte_order == ByteOrder::little_endian
            ? index
            : destination.size() - 1U - index;
        const auto shift = static_cast<unsigned>(index * 8U);
        destination[destination_index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

[[nodiscard]] std::uint64_t read_unsigned(
    const std::span<const std::byte> source,
    const ByteOrder byte_order) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto source_index = byte_order == ByteOrder::little_endian
            ? index
            : source.size() - 1U - index;
        const auto shift = static_cast<unsigned>(index * 8U);
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(source[source_index]))
            << shift;
    }
    return value;
}

[[nodiscard]] std::int64_t sign_extend(
    std::uint64_t value,
    const unsigned valid_bits) noexcept {
    const auto value_mask = (std::uint64_t{1} << valid_bits) - 1U;
    const auto sign_bit = std::uint64_t{1} << (valid_bits - 1U);
    value &= value_mask;
    if ((value & sign_bit) == 0) return static_cast<std::int64_t>(value);

    const auto magnitude = ((~value) & value_mask) + 1U;
    return -static_cast<std::int64_t>(magnitude);
}

void encode_sample(
    const float input,
    const SampleFormat& format,
    const std::span<std::byte> destination) noexcept {
    if (format.encoding == SampleEncoding::signed_integer) {
        const auto value = static_cast<std::int32_t>(quantize_sample(input, format.valid_bits));
        const auto raw = static_cast<std::uint32_t>(value);
        write_unsigned(raw, format.byte_order, destination);
        return;
    }

    const auto sample = normalized_sample(static_cast<double>(input));
    if (format.encoding == SampleEncoding::ieee_float32) {
        const auto raw = std::bit_cast<std::uint32_t>(static_cast<float>(sample));
        write_unsigned(raw, format.byte_order, destination);
        return;
    }

    const auto raw = std::bit_cast<std::uint64_t>(sample);
    write_unsigned(raw, format.byte_order, destination);
}

[[nodiscard]] float decode_sample(
    const SampleFormat& format,
    const std::span<const std::byte> source) noexcept {
    const auto raw = read_unsigned(source, format.byte_order);
    if (format.encoding == SampleEncoding::signed_integer) {
        const auto value = sign_extend(raw, format.valid_bits);
        const auto scale = static_cast<double>(
            std::int64_t{1} << (format.valid_bits - 1U));
        return static_cast<float>(static_cast<double>(value) / scale);
    }

    if (format.encoding == SampleEncoding::ieee_float32) {
        const auto raw32 = static_cast<std::uint32_t>(raw);
        return static_cast<float>(normalized_sample(
            static_cast<double>(std::bit_cast<float>(raw32))));
    }

    return static_cast<float>(normalized_sample(std::bit_cast<double>(raw)));
}

[[nodiscard]] bool shape_is_valid(
    const std::size_t sample_count,
    const std::size_t frame_count,
    const std::size_t channel_count) noexcept {
    if (channel_count == 0) {
        return frame_count == 0 && sample_count == 0;
    }
    if (frame_count > std::numeric_limits<std::size_t>::max() / channel_count) {
        return false;
    }
    return sample_count == frame_count * channel_count;
}

template <typename ChannelBuffer>
[[nodiscard]] SampleConversionResult validate_buffers(
    const std::span<const ChannelBuffer> channels,
    const std::size_t frame_count) noexcept {
    for (const auto& channel : channels) {
        SampleFormat format;
        if (!describe_sample_type(channel.sample_type, format)) {
            return SampleConversionResult::unsupported_sample_type;
        }
        if (frame_count > std::numeric_limits<std::size_t>::max() / format.byte_count
            || channel.bytes.size() < frame_count * format.byte_count) {
            return SampleConversionResult::buffer_too_small;
        }
    }
    return SampleConversionResult::success;
}

} // namespace

std::size_t asio_sample_size(const ASIOSampleType sample_type) noexcept {
    SampleFormat format;
    return describe_sample_type(sample_type, format) ? format.byte_count : 0;
}

bool is_supported_asio_sample_type(const ASIOSampleType sample_type) noexcept {
    SampleFormat format;
    return describe_sample_type(sample_type, format);
}

const char* sample_conversion_result_message(const SampleConversionResult result) noexcept {
    switch (result) {
    case SampleConversionResult::success:
        return "success";
    case SampleConversionResult::invalid_shape:
        return "interleaved sample count does not match frame and channel counts";
    case SampleConversionResult::buffer_too_small:
        return "ASIO channel buffer is too small";
    case SampleConversionResult::unsupported_sample_type:
        return "unsupported ASIO sample type";
    }
    return "unknown sample conversion result";
}

SampleConversionResult interleaved_float_to_asio(
    const std::span<const float> interleaved,
    const std::size_t frame_count,
    const std::span<const MutableAsioChannelBuffer> channels) noexcept {
    if (!shape_is_valid(interleaved.size(), frame_count, channels.size())) {
        return SampleConversionResult::invalid_shape;
    }
    const auto validation = validate_buffers(channels, frame_count);
    if (validation != SampleConversionResult::success) return validation;

    for (std::size_t channel_index = 0; channel_index < channels.size(); ++channel_index) {
        const auto& channel = channels[channel_index];
        SampleFormat format;
        static_cast<void>(describe_sample_type(channel.sample_type, format));

        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            const auto byte_offset = frame * format.byte_count;
            encode_sample(
                interleaved[frame * channels.size() + channel_index],
                format,
                channel.bytes.subspan(byte_offset, format.byte_count));
        }
    }
    return SampleConversionResult::success;
}

SampleConversionResult asio_to_interleaved_float(
    const std::span<const ConstAsioChannelBuffer> channels,
    const std::size_t frame_count,
    const std::span<float> interleaved) noexcept {
    if (!shape_is_valid(interleaved.size(), frame_count, channels.size())) {
        return SampleConversionResult::invalid_shape;
    }
    const auto validation = validate_buffers(channels, frame_count);
    if (validation != SampleConversionResult::success) return validation;

    for (std::size_t channel_index = 0; channel_index < channels.size(); ++channel_index) {
        const auto& channel = channels[channel_index];
        SampleFormat format;
        static_cast<void>(describe_sample_type(channel.sample_type, format));

        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            const auto byte_offset = frame * format.byte_count;
            interleaved[frame * channels.size() + channel_index] = decode_sample(
                format,
                channel.bytes.subspan(byte_offset, format.byte_count));
        }
    }
    return SampleConversionResult::success;
}

} // namespace capture_panel::asio
