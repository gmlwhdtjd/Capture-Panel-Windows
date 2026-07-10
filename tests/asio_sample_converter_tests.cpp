#include "test_framework.hpp"

#include "asio_sample_converter.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace capture_panel::asio;

namespace {

[[nodiscard]] std::vector<std::byte> encode_mono(
    const ASIOSampleType sample_type,
    const std::span<const float> samples) {
    std::vector<std::byte> bytes(samples.size() * asio_sample_size(sample_type));
    const std::array channels{
        MutableAsioChannelBuffer{bytes, sample_type},
    };
    CP_REQUIRE(interleaved_float_to_asio(samples, samples.size(), channels)
        == SampleConversionResult::success);
    return bytes;
}

[[nodiscard]] std::vector<float> decode_mono(
    const ASIOSampleType sample_type,
    const std::span<const std::byte> bytes) {
    const auto sample_size = asio_sample_size(sample_type);
    CP_REQUIRE(sample_size > 0);
    CP_REQUIRE(bytes.size() % sample_size == 0);
    std::vector<float> samples(bytes.size() / sample_size);
    const std::array channels{
        ConstAsioChannelBuffer{bytes, sample_type},
    };
    CP_REQUIRE(asio_to_interleaved_float(channels, samples.size(), samples)
        == SampleConversionResult::success);
    return samples;
}

struct IntegerFormat {
    ASIOSampleType type;
    unsigned valid_bits;
};

constexpr std::array integer_formats{
    IntegerFormat{ASIOSTInt16MSB, 16},
    IntegerFormat{ASIOSTInt24MSB, 24},
    IntegerFormat{ASIOSTInt32MSB, 32},
    IntegerFormat{ASIOSTInt32MSB16, 16},
    IntegerFormat{ASIOSTInt32MSB18, 18},
    IntegerFormat{ASIOSTInt32MSB20, 20},
    IntegerFormat{ASIOSTInt32MSB24, 24},
    IntegerFormat{ASIOSTInt16LSB, 16},
    IntegerFormat{ASIOSTInt24LSB, 24},
    IntegerFormat{ASIOSTInt32LSB, 32},
    IntegerFormat{ASIOSTInt32LSB16, 16},
    IntegerFormat{ASIOSTInt32LSB18, 18},
    IntegerFormat{ASIOSTInt32LSB20, 20},
    IntegerFormat{ASIOSTInt32LSB24, 24},
};

} // namespace

CP_TEST_CASE("ASIO sample converter reports every supported PCM width") {
    const std::array two_byte_types{ASIOSTInt16MSB, ASIOSTInt16LSB};
    for (const auto type : two_byte_types) {
        CP_REQUIRE(asio_sample_size(type) == 2);
        CP_REQUIRE(is_supported_asio_sample_type(type));
    }

    const std::array three_byte_types{ASIOSTInt24MSB, ASIOSTInt24LSB};
    for (const auto type : three_byte_types) {
        CP_REQUIRE(asio_sample_size(type) == 3);
        CP_REQUIRE(is_supported_asio_sample_type(type));
    }

    const std::array four_byte_types{
        ASIOSTInt32MSB,
        ASIOSTFloat32MSB,
        ASIOSTInt32MSB16,
        ASIOSTInt32MSB18,
        ASIOSTInt32MSB20,
        ASIOSTInt32MSB24,
        ASIOSTInt32LSB,
        ASIOSTFloat32LSB,
        ASIOSTInt32LSB16,
        ASIOSTInt32LSB18,
        ASIOSTInt32LSB20,
        ASIOSTInt32LSB24,
    };
    for (const auto type : four_byte_types) {
        CP_REQUIRE(asio_sample_size(type) == 4);
        CP_REQUIRE(is_supported_asio_sample_type(type));
    }

    const std::array eight_byte_types{ASIOSTFloat64MSB, ASIOSTFloat64LSB};
    for (const auto type : eight_byte_types) {
        CP_REQUIRE(asio_sample_size(type) == 8);
        CP_REQUIRE(is_supported_asio_sample_type(type));
    }
}

CP_TEST_CASE("ASIO sample converter rejects DSD and unknown sample types") {
    const std::array unsupported_types{
        static_cast<ASIOSampleType>(ASIOSTDSDInt8LSB1),
        static_cast<ASIOSampleType>(ASIOSTDSDInt8MSB1),
        static_cast<ASIOSampleType>(ASIOSTDSDInt8NER8),
        static_cast<ASIOSampleType>(9'999),
    };

    for (const auto type : unsupported_types) {
        CP_REQUIRE(asio_sample_size(type) == 0);
        CP_REQUIRE(!is_supported_asio_sample_type(type));

        const std::array<float, 1> input{0.0F};
        std::array<std::byte, 8> storage{};
        const std::array channels{
            MutableAsioChannelBuffer{storage, type},
        };
        CP_REQUIRE(interleaved_float_to_asio(input, 1, channels)
            == SampleConversionResult::unsupported_sample_type);
    }
}

CP_TEST_CASE("ASIO integer PCM output uses exact normalized endpoint quantization") {
    const std::array<float, 3> samples{-1.0F, 0.5F, 1.0F};

    const std::vector<std::byte> int16_lsb_expected{
        std::byte{0x00}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x40},
        std::byte{0xFF}, std::byte{0x7F},
    };
    const std::vector<std::byte> int16_msb_expected{
        std::byte{0x80}, std::byte{0x00},
        std::byte{0x40}, std::byte{0x00},
        std::byte{0x7F}, std::byte{0xFF},
    };
    CP_REQUIRE(encode_mono(ASIOSTInt16LSB, samples) == int16_lsb_expected);
    CP_REQUIRE(encode_mono(ASIOSTInt16MSB, samples) == int16_msb_expected);

    const std::vector<std::byte> int24_lsb_expected{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x40},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F},
    };
    const std::vector<std::byte> int24_msb_expected{
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x40}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x7F}, std::byte{0xFF}, std::byte{0xFF},
    };
    CP_REQUIRE(encode_mono(ASIOSTInt24LSB, samples) == int24_lsb_expected);
    CP_REQUIRE(encode_mono(ASIOSTInt24MSB, samples) == int24_msb_expected);

    const std::vector<std::byte> int32_lsb_expected{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x40},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F},
    };
    const std::vector<std::byte> int32_msb_expected{
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x7F}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
    };
    CP_REQUIRE(encode_mono(ASIOSTInt32LSB, samples) == int32_lsb_expected);
    CP_REQUIRE(encode_mono(ASIOSTInt32MSB, samples) == int32_msb_expected);
}

CP_TEST_CASE("ASIO right-aligned integer containers sign extend unused high bits") {
    struct Case {
        ASIOSampleType lsb_type;
        ASIOSampleType msb_type;
        std::array<std::byte, 4> negative_lsb;
        std::array<std::byte, 4> positive_lsb;
    };
    const std::array cases{
        Case{ASIOSTInt32LSB16, ASIOSTInt32MSB16,
            {std::byte{0x00}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}},
            {std::byte{0xFF}, std::byte{0x7F}, std::byte{0x00}, std::byte{0x00}}},
        Case{ASIOSTInt32LSB18, ASIOSTInt32MSB18,
            {std::byte{0x00}, std::byte{0x00}, std::byte{0xFE}, std::byte{0xFF}},
            {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x01}, std::byte{0x00}}},
        Case{ASIOSTInt32LSB20, ASIOSTInt32MSB20,
            {std::byte{0x00}, std::byte{0x00}, std::byte{0xF8}, std::byte{0xFF}},
            {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x07}, std::byte{0x00}}},
        Case{ASIOSTInt32LSB24, ASIOSTInt32MSB24,
            {std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0xFF}},
            {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x00}}},
    };
    const std::array<float, 2> samples{-1.0F, 1.0F};

    for (const auto& test_case : cases) {
        const auto lsb = encode_mono(test_case.lsb_type, samples);
        CP_REQUIRE(lsb.size() == 8);
        CP_REQUIRE(std::equal(
            test_case.negative_lsb.begin(),
            test_case.negative_lsb.end(),
            lsb.begin()));
        CP_REQUIRE(std::equal(
            test_case.positive_lsb.begin(),
            test_case.positive_lsb.end(),
            lsb.begin() + 4));

        const auto msb = encode_mono(test_case.msb_type, samples);
        CP_REQUIRE(msb.size() == 8);
        for (std::size_t index = 0; index < 4; ++index) {
            CP_REQUIRE(msb[index] == test_case.negative_lsb[3U - index]);
            CP_REQUIRE(msb[index + 4U] == test_case.positive_lsb[3U - index]);
        }
    }
}

CP_TEST_CASE("ASIO float formats honor byte order and sanitize output") {
    const std::array<float, 4> samples{
        0.5F,
        -1.0F,
        2.0F,
        std::numeric_limits<float>::quiet_NaN(),
    };
    const std::vector<std::byte> float32_lsb_expected{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3F},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0xBF},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3F},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    const std::vector<std::byte> float32_msb_expected{
        std::byte{0x3F}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xBF}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x3F}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    CP_REQUIRE(encode_mono(ASIOSTFloat32LSB, samples) == float32_lsb_expected);
    CP_REQUIRE(encode_mono(ASIOSTFloat32MSB, samples) == float32_msb_expected);

    const std::array<float, 2> double_samples{0.5F, -1.0F};
    const std::vector<std::byte> float64_lsb_expected{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xE0}, std::byte{0x3F},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xF0}, std::byte{0xBF},
    };
    const std::vector<std::byte> float64_msb_expected{
        std::byte{0x3F}, std::byte{0xE0}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xBF}, std::byte{0xF0}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    CP_REQUIRE(encode_mono(ASIOSTFloat64LSB, double_samples) == float64_lsb_expected);
    CP_REQUIRE(encode_mono(ASIOSTFloat64MSB, double_samples) == float64_msb_expected);
}

CP_TEST_CASE("ASIO float input clamps infinities and maps NaN to silence") {
    const std::array<std::uint32_t, 4> words{
        std::bit_cast<std::uint32_t>(2.0F),
        std::bit_cast<std::uint32_t>(-2.0F),
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()),
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()),
    };
    std::vector<std::byte> bytes;
    bytes.reserve(words.size() * sizeof(std::uint32_t));
    for (const auto word : words) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((word >> shift) & 0xFFU));
        }
    }

    const auto decoded = decode_mono(ASIOSTFloat32LSB, bytes);
    const std::vector<float> expected{1.0F, -1.0F, 1.0F, 0.0F};
    CP_REQUIRE(decoded == expected);
}

CP_TEST_CASE("ASIO integer formats round trip normalized interleaved samples") {
    const std::array<float, 7> input{-1.0F, -0.75F, -0.1F, 0.0F, 0.1F, 0.75F, 1.0F};

    for (const auto& format : integer_formats) {
        const auto decoded = decode_mono(format.type, encode_mono(format.type, input));
        CP_REQUIRE(decoded.size() == input.size());

        const auto scale_integer = std::int64_t{1} << (format.valid_bits - 1U);
        const auto tolerance = 1.0 / static_cast<double>(scale_integer) + 1.0e-7;
        for (std::size_t index = 0; index < input.size(); ++index) {
            const auto expected = input[index] >= 1.0F
                ? 1.0 - (1.0 / static_cast<double>(scale_integer))
                : static_cast<double>(input[index]);
            CP_REQUIRE_NEAR(decoded[index], expected, tolerance);
        }
    }
}

CP_TEST_CASE("ASIO floating point formats round trip in both byte orders") {
    const std::array<float, 7> input{-1.0F, -0.75F, -0.1F, 0.0F, 0.1F, 0.75F, 1.0F};
    const std::array sample_types{
        ASIOSTFloat32MSB,
        ASIOSTFloat64MSB,
        ASIOSTFloat32LSB,
        ASIOSTFloat64LSB,
    };

    for (const auto sample_type : sample_types) {
        const auto decoded = decode_mono(sample_type, encode_mono(sample_type, input));
        CP_REQUIRE(decoded == std::vector<float>(input.begin(), input.end()));
    }
}

CP_TEST_CASE("ASIO converter maps interleaved channels to independent planar formats") {
    const std::array<float, 4> input{
        0.5F, -0.5F,
        -1.0F, 1.0F,
    };
    std::array<std::byte, 4> integer_channel{};
    std::array<std::byte, 8> float_channel{};
    const std::array output_channels{
        MutableAsioChannelBuffer{integer_channel, ASIOSTInt16LSB},
        MutableAsioChannelBuffer{float_channel, ASIOSTFloat32MSB},
    };
    CP_REQUIRE(interleaved_float_to_asio(input, 2, output_channels)
        == SampleConversionResult::success);

    const std::array<std::byte, 4> expected_integer{
        std::byte{0x00}, std::byte{0x40},
        std::byte{0x00}, std::byte{0x80},
    };
    const std::array<std::byte, 8> expected_float{
        std::byte{0xBF}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x3F}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
    };
    CP_REQUIRE(integer_channel == expected_integer);
    CP_REQUIRE(float_channel == expected_float);

    std::array<float, 4> decoded{};
    const std::array input_channels{
        ConstAsioChannelBuffer{integer_channel, ASIOSTInt16LSB},
        ConstAsioChannelBuffer{float_channel, ASIOSTFloat32MSB},
    };
    CP_REQUIRE(asio_to_interleaved_float(input_channels, 2, decoded)
        == SampleConversionResult::success);
    CP_REQUIRE(decoded == input);
}

CP_TEST_CASE("ASIO converter validates shape and native buffer capacity before writing") {
    const std::array<float, 2> input{0.25F, -0.25F};
    std::array<std::byte, 4> storage{
        std::byte{0x55}, std::byte{0x55}, std::byte{0x55}, std::byte{0x55},
    };
    const std::array channels{
        MutableAsioChannelBuffer{storage, ASIOSTInt16LSB},
    };

    CP_REQUIRE(interleaved_float_to_asio(input, 1, channels)
        == SampleConversionResult::invalid_shape);
    const std::array<std::byte, 4> untouched{
        std::byte{0x55}, std::byte{0x55}, std::byte{0x55}, std::byte{0x55},
    };
    CP_REQUIRE(storage == untouched);

    CP_REQUIRE(interleaved_float_to_asio(input, 2, channels)
        == SampleConversionResult::success);

    std::array<std::byte, 3> too_small{};
    const std::array short_channels{
        MutableAsioChannelBuffer{too_small, ASIOSTInt16LSB},
    };
    CP_REQUIRE(interleaved_float_to_asio(input, 2, short_channels)
        == SampleConversionResult::buffer_too_small);
}
