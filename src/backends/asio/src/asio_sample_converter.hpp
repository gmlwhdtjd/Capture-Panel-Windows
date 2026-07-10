#pragma once

#include "asiosys.h"
#include "asio.h"

#include <cstddef>
#include <span>

namespace capture_panel::asio {

/// A writable native ASIO channel buffer. ASIO stores each channel as a
/// separate contiguous (planar) array of samples.
struct MutableAsioChannelBuffer {
    std::span<std::byte> bytes;
    ASIOSampleType sample_type = ASIOSTLastEntry;
};

/// A read-only native ASIO channel buffer.
struct ConstAsioChannelBuffer {
    std::span<const std::byte> bytes;
    ASIOSampleType sample_type = ASIOSTLastEntry;
};

enum class SampleConversionResult {
    success,
    invalid_shape,
    buffer_too_small,
    unsupported_sample_type,
};

/// Returns the native byte width of one ASIO sample, or zero when the type is
/// unsupported. DSD formats are deliberately not treated as PCM.
[[nodiscard]] std::size_t asio_sample_size(ASIOSampleType sample_type) noexcept;

[[nodiscard]] bool is_supported_asio_sample_type(ASIOSampleType sample_type) noexcept;

[[nodiscard]] const char* sample_conversion_result_message(
    SampleConversionResult result) noexcept;

/// Converts normalized interleaved float audio to native planar ASIO buffers.
///
/// `channels.size()` is the logical interleaved channel count. Each channel
/// view must contain at least `frame_count * asio_sample_size(type)` bytes and
/// `interleaved` must contain exactly `frame_count * channels.size()` samples.
/// The function performs no allocation and is safe to call from an ASIO
/// callback after the views have been prepared.
[[nodiscard]] SampleConversionResult interleaved_float_to_asio(
    std::span<const float> interleaved,
    std::size_t frame_count,
    std::span<const MutableAsioChannelBuffer> channels) noexcept;

/// Converts native planar ASIO buffers to normalized interleaved float audio.
/// Shape and capacity requirements mirror `interleaved_float_to_asio`.
[[nodiscard]] SampleConversionResult asio_to_interleaved_float(
    std::span<const ConstAsioChannelBuffer> channels,
    std::size_t frame_count,
    std::span<float> interleaved) noexcept;

} // namespace capture_panel::asio
